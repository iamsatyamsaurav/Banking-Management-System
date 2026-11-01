#define _GNU_SOURCE
#include "feedback.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define FEEDBACK_LINE_SIZE 512
#define FEEDBACK_MESSAGE_MAX 300
#define FEEDBACK_LIST_LIMIT 10

/* The ledger uses one pipe-separated record per line. */
static int valid_field(const char *value, size_t max_length)
{
    if (value == NULL || value[0] == '\0') return 0;
    size_t n = strnlen(value, max_length + 1);
    if (n == 0 || n > max_length) return 0;
    for (size_t i = 0; i < n; ++i)
        if (value[i] == '|' || value[i] == '\n' || value[i] == '\r') return 0;
    return 1;
}

static int acquire_lock(const char *path, short type)
{
    char name[1024];
    int n = snprintf(name, sizeof(name), "%s.lock", path);
    if (n < 0 || (size_t)n >= sizeof(name)) return -1;

    int fd = open(name, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    struct flock lock = {0};
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    lock.l_len = 0;
    while (fcntl(fd, F_OFD_SETLKW, &lock) < 0) {
        if (errno == EINTR) continue;
        close(fd);
        return -1;
    }
    return fd;
}

static void release_lock(int fd)
{
    if (fd < 0) return;
    struct flock lock = {0};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    (void)fcntl(fd, F_OFD_SETLK, &lock);
    close(fd);
}

static int write_all(int fd, const char *data, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, data + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static int parse_record(char *line, char *customer, size_t customer_size,
                        char *message, size_t message_size)
{
    line[strcspn(line, "\r\n")] = '\0';
    char *separator = strchr(line, '|');
    if (separator == NULL || strchr(separator + 1, '|') != NULL) return -1;
    *separator = '\0';
    if (!valid_field(line, customer_size - 1) ||
        !valid_field(separator + 1, message_size - 1)) return -1;
    snprintf(customer, customer_size, "%s", line);
    snprintf(message, message_size, "%s", separator + 1);
    return 0;
}

int feedback_submit(const char *path, const char *customer, const char *message)
{
    if (path == NULL || !valid_field(customer, 31) ||
        !valid_field(message, FEEDBACK_MESSAGE_MAX)) return FEEDBACK_ERROR;

    int lock_fd = acquire_lock(path, F_WRLCK);
    if (lock_fd < 0) return FEEDBACK_ERROR;

    char line[FEEDBACK_LINE_SIZE];
    int n = snprintf(line, sizeof(line), "%s|%s\n", customer, message);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        release_lock(lock_fd);
        return FEEDBACK_ERROR;
    }

    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        release_lock(lock_fd);
        return FEEDBACK_ERROR;
    }
    int result = write_all(fd, line, (size_t)n) == 0 && fsync(fd) == 0
               ? FEEDBACK_OK : FEEDBACK_ERROR;
    if (close(fd) < 0) result = FEEDBACK_ERROR;
    release_lock(lock_fd);
    return result;
}

int feedback_list(const char *path, char *output, size_t output_size)
{
    if (path == NULL || output == NULL || output_size == 0) return FEEDBACK_ERROR;
    output[0] = '\0';

    int lock_fd = acquire_lock(path, F_RDLCK);
    if (lock_fd < 0) return FEEDBACK_ERROR;
    FILE *input = fopen(path, "r");
    if (input == NULL) {
        int missing = errno == ENOENT;
        release_lock(lock_fd);
        if (missing) {
            snprintf(output, output_size, "No feedback submitted yet.");
            return FEEDBACK_OK;
        }
        return FEEDBACK_ERROR;
    }

    char recent[FEEDBACK_LIST_LIMIT][FEEDBACK_LINE_SIZE] = {{0}};
    size_t next = 0, total = 0, capacity = 0;
    char *line = NULL;
    ssize_t length;
    int failed = 0;

    while ((length = getline(&line, &capacity, input)) != -1) {
        if ((size_t)length >= FEEDBACK_LINE_SIZE) continue;
        char scratch[FEEDBACK_LINE_SIZE], customer[32];
        char message[FEEDBACK_MESSAGE_MAX + 1];
        memcpy(scratch, line, (size_t)length + 1);
        if (parse_record(scratch, customer, sizeof(customer),
                         message, sizeof(message)) != 0) continue;
        snprintf(recent[next], sizeof(recent[next]), "%s", line);
        next = (next + 1) % FEEDBACK_LIST_LIMIT;
        ++total;
    }
    if (ferror(input)) failed = 1;
    free(line);
    fclose(input);
    release_lock(lock_fd);
    if (failed) return FEEDBACK_ERROR;

    if (total == 0) {
        snprintf(output, output_size, "No feedback submitted yet.");
        return FEEDBACK_OK;
    }

    size_t shown = total < FEEDBACK_LIST_LIMIT ? total : FEEDBACK_LIST_LIMIT;
    size_t start = total > FEEDBACK_LIST_LIMIT ? next : 0;
    size_t used = 0;
    for (size_t i = 0; i < shown; ++i) {
        char scratch[FEEDBACK_LINE_SIZE], customer[32];
        char message[FEEDBACK_MESSAGE_MAX + 1];
        size_t index = (start + i) % FEEDBACK_LIST_LIMIT;
        snprintf(scratch, sizeof(scratch), "%s", recent[index]);
        if (parse_record(scratch, customer, sizeof(customer),
                         message, sizeof(message)) != 0) continue;

        int n = snprintf(output + used, output_size - used,
                         "%sCustomer: %s | Message: %s",
                         used == 0 ? "" : " ; ", customer, message);
        if (n < 0 || (size_t)n >= output_size - used) return FEEDBACK_ERROR;
        used += (size_t)n;
    }
    return FEEDBACK_OK;
}
