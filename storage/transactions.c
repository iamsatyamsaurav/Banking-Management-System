#define _GNU_SOURCE

#include "transactions.h"

#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

#define TX_LINE_SIZE 256
#define TX_HISTORY_LIMIT 20

/*
 * The ledger is an append-only text file.
 * Its lock serializes ledger access; account balance updates continue
 * to use the existing record-level locks in storage.c.
 */
static int set_ledger_lock(int fd, short type)
{
    struct flock lock = {0};

    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;  /* Entire ledger file, not account records. */

    int command = (type == F_UNLCK) ? F_OFD_SETLK : F_OFD_SETLKW;

    while (fcntl(fd, command, &lock) == -1) {
        if (errno == EINTR && type != F_UNLCK) {
            continue;
        }
        return -1;
    }

    return 0;
}

/* Prevent delimiters or newlines from corrupting the file format. */
static int valid_field(const char *value, size_t max_length)
{
    if (value == NULL || value[0] == '\0') {
        return 0;
    }

    size_t length = strnlen(value, max_length);

    if (length == 0 || length >= max_length) {
        return 0;
    }

    for (size_t i = 0; i < length; i++) {
        if (value[i] == '|' ||
            value[i] == '\n' ||
            value[i] == '\r') {
            return 0;
        }
    }

    return 1;
}

static int valid_type(const char *type)
{
    return type != NULL &&
           (strcmp(type, "DEPOSIT") == 0 ||
            strcmp(type, "WITHDRAW") == 0 ||
            strcmp(type, "TRANSFER") == 0);
}

static int write_all(int fd, const char *data, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t n = write(fd, data + written, length - written);

        if (n == -1 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            return -1;
        }

        written += (size_t)n;
    }

    return 0;
}

int transaction_log(
    const char *path,
    const char *type,
    const char *source_account,
    const char *destination_account,
    int64_t amount_paise,
    const char *actor)
{
    if (path == NULL ||
        !valid_type(type) ||
        !valid_field(source_account, 32) ||
        !valid_field(destination_account, 32) ||
        !valid_field(actor, 32) ||
        amount_paise <= 0) {
        return -1;
    }

    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);

    if (fd == -1) {
        perror("open transaction ledger");
        return -1;
    }

    if (set_ledger_lock(fd, F_WRLCK) == -1) {
        perror("lock transaction ledger");
        close(fd);
        return -1;
    }

    /* Keep the original end position in case writing fails partway. */
    off_t original_end = lseek(fd, 0, SEEK_END);

    struct timespec now;
    struct tm local_time;
    char time_text[24];
    char line[TX_LINE_SIZE];

    int result = -1;

    if (original_end == -1 ||
        clock_gettime(CLOCK_REALTIME, &now) == -1 ||
        localtime_r(&now.tv_sec, &local_time) == NULL) {
        perror("prepare transaction record");
        goto cleanup;
    }

    if (strftime(time_text, sizeof(time_text),
                 "%Y-%m-%d %H:%M:%S", &local_time) == 0) {
        goto cleanup;
    }

    int length = snprintf(
        line,
        sizeof(line),
        "%s|%s|%s|%s|%lld|%s\n",
        time_text,
        type,
        source_account,
        destination_account,
        (long long)amount_paise,
        actor
    );

    if (length < 0 || (size_t)length >= sizeof(line)) {
        goto cleanup;
    }

    /*
     * The ledger lock prevents another cooperating thread/process
     * from interleaving its record with this write.
     */
    if (write_all(fd, line, (size_t)length) == -1 || fsync(fd) == -1) {
        perror("write transaction record");

        /* Remove an incomplete final record if possible. */
        if (original_end != -1) {
            (void)ftruncate(fd, original_end);
            (void)fsync(fd);
        }

        goto cleanup;
    }

    result = 0;

cleanup:
    (void)set_ledger_lock(fd, F_UNLCK);
    close(fd);
    return result;
}

/* Split one ledger line into exactly six fields. */
static int split_record(char *line, char *fields[6])
{
    line[strcspn(line, "\r\n")] = '\0';

    size_t count = 0;
    char *save = NULL;
    char *token = strtok_r(line, "|", &save);

    while (token != NULL) {
        if (count == 6) {
            return -1;
        }

        fields[count++] = token;
        token = strtok_r(NULL, "|", &save);
    }

    return count == 6 ? 0 : -1;
}

static int append_text(
    char *buffer,
    size_t buffer_size,
    size_t *used,
    const char *format,
    ...)
{
    if (*used >= buffer_size) {
        return -1;
    }

    va_list args;
    va_start(args, format);

    int n = vsnprintf(
        buffer + *used,
        buffer_size - *used,
        format,
        args
    );

    va_end(args);

    if (n < 0 || (size_t)n >= buffer_size - *used) {
        return -1;
    }

    *used += (size_t)n;
    return 0;
}

int transaction_history(
    const char *path,
    const char *account_id,
    char *buffer,
    size_t buffer_size)
{
    if (path == NULL ||
        !valid_field(account_id, 32) ||
        buffer == NULL ||
        buffer_size == 0) {
        return -1;
    }

    buffer[0] = '\0';

    int fd = open(path, O_RDONLY);

    if (fd == -1) {
        if (errno == ENOENT) {
            snprintf(buffer, buffer_size, "No transactions found.\n");
            return 0;
        }

        perror("open transaction history");
        return -1;
    }

    if (set_ledger_lock(fd, F_RDLCK) == -1) {
        perror("lock transaction history");
        close(fd);
        return -1;
    }

    int copy_fd = dup(fd);

    if (copy_fd == -1) {
        (void)set_ledger_lock(fd, F_UNLCK);
        close(fd);
        return -1;
    }

    FILE *file = fdopen(copy_fd, "r");

    if (file == NULL) {
        close(copy_fd);
        (void)set_ledger_lock(fd, F_UNLCK);
        close(fd);
        return -1;
    }

    /*
     * Retain the 20 most recent matching records so a large ledger
     * does not produce an unlimited response.
     */
    char recent[TX_HISTORY_LIMIT][TX_LINE_SIZE] = {{0}};
    size_t next = 0;
    size_t total_matches = 0;

    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;

    while ((length = getline(&line, &capacity, file)) != -1) {
        if ((size_t)length >= TX_LINE_SIZE) {
            continue;
        }

        char scratch[TX_LINE_SIZE];
        memcpy(scratch, line, (size_t)length + 1);

        char *fields[6];

        if (split_record(scratch, fields) == -1) {
            continue;
        }

        /* Source or destination match means the account is involved. */
        if (strcmp(fields[2], account_id) != 0 &&
            strcmp(fields[3], account_id) != 0) {
            continue;
        }

        snprintf(recent[next], sizeof(recent[next]), "%s", line);

        next = (next + 1) % TX_HISTORY_LIMIT;
        total_matches++;
    }

    free(line);
    fclose(file);

    (void)set_ledger_lock(fd, F_UNLCK);
    close(fd);

    size_t used = 0;

    if (append_text(buffer, buffer_size, &used,
                    "Transaction history for %s "
                    "(up to %d most recent):\n",
                    account_id, TX_HISTORY_LIMIT) == -1) {
        return -1;
    }

    if (total_matches == 0) {
        return append_text(buffer, buffer_size, &used,
                           "No transactions found for this account.\n");
    }

    size_t displayed = total_matches < TX_HISTORY_LIMIT
                     ? total_matches : TX_HISTORY_LIMIT;
    size_t start = total_matches > TX_HISTORY_LIMIT ? next : 0;

    for (size_t i = 0; i < displayed; i++) {
        size_t index = (start + i) % TX_HISTORY_LIMIT;
        char record[TX_LINE_SIZE];

        snprintf(record, sizeof(record), "%s", recent[index]);

        char *fields[6];

        if (split_record(record, fields) == -1) {
            continue;
        }

        char *end = NULL;
        long long amount = strtoll(fields[4], &end, 10);

        if (end == fields[4] || *end != '\0' || amount <= 0) {
            continue;
        }

        if (append_text(
                buffer, buffer_size, &used,
                "%s | %-8s | from %-16s -> to %-16s"
                " | Rs %lld.%02lld | by %s\n",
                fields[0],
                fields[1],
                fields[2],
                fields[3],
                amount / 100,
                amount % 100,
                fields[5]) == -1) {
            return -1;
        }
    }

    return 0;
}