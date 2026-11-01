#define _GNU_SOURCE

#include "loans.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOAN_LINE_SIZE 512
#define LOAN_LIST_LIMIT 20

typedef struct {
    char id[16];
    char customer[32];
    char account[17];
    int64_t amount;
    char status[16];
    char assigned[32];
    char submitted[24];
    char purpose[128];
} LoanRecord;

/* Reject empty fields, pipes and line breaks in stored values. */
static int valid_field(const char *s, size_t capacity)
{
    if (s == NULL || s[0] == '\0')
        return 0;

    size_t n = strnlen(s, capacity);

    if (n == 0 || n >= capacity)
        return 0;

    for (size_t i = 0; i < n; i++) {
        if (s[i] == '|' || s[i] == '\n' || s[i] == '\r')
            return 0;
    }

    return 1;
}

static int copy_field(char *dst, size_t capacity, const char *src)
{
    size_t n = strlen(src);

    if (n >= capacity)
        return -1;

    memcpy(dst, src, n + 1);
    return 0;
}

/*
 * The separate .lock file remains stable even when an update
 * replaces the data file with a newly written temporary file.
 */
static int acquire_lock(const char *path, short type)
{
    char lock_path[1024];

    int n = snprintf(lock_path, sizeof(lock_path), "%s.lock", path);

    if (n < 0 || (size_t)n >= sizeof(lock_path))
        return -1;

    int fd = open(lock_path, O_RDWR | O_CREAT, 0600);

    if (fd == -1)
        return -1;

    struct flock lock = {0};
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    while (fcntl(fd, F_OFD_SETLKW, &lock) == -1) {
        if (errno == EINTR)
            continue;

        close(fd);
        return -1;
    }

    return fd;
}

static void release_lock(int fd)
{
    if (fd < 0)
        return;

    struct flock lock = {0};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    (void)fcntl(fd, F_OFD_SETLK, &lock);
    close(fd);
}

static int write_all(int fd, const char *data, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t n = write(fd, data + written, length - written);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        written += (size_t)n;
    }

    return 0;
}

/* Parse one pipe-separated line into a loan record. */
static int parse_record(char *line, LoanRecord *r)
{
    line[strcspn(line, "\r\n")] = '\0';

    char *fields[8];
    size_t count = 1;
    fields[0] = line;

    for (char *p = line; *p; p++) {
        if (*p != '|')
            continue;

        if (count == 8)
            return -1;

        *p = '\0';
        fields[count++] = p + 1;
    }

    if (count != 8)
        return -1;

    memset(r, 0, sizeof(*r));

    if (copy_field(r->id, sizeof(r->id), fields[0]) ||
        copy_field(r->customer, sizeof(r->customer), fields[1]) ||
        copy_field(r->account, sizeof(r->account), fields[2]) ||
        copy_field(r->status, sizeof(r->status), fields[4]) ||
        copy_field(r->assigned, sizeof(r->assigned), fields[5]) ||
        copy_field(r->submitted, sizeof(r->submitted), fields[6]) ||
        copy_field(r->purpose, sizeof(r->purpose), fields[7])) {
        return -1;
    }

    errno = 0;
    char *end = NULL;
    long long amount = strtoll(fields[3], &end, 10);

    if (errno || end == fields[3] || *end != '\0' || amount <= 0)
        return -1;

    r->amount = (int64_t)amount;
    return 0;
}

static int format_record(
    const LoanRecord *r, char *line, size_t capacity)
{
    int n = snprintf(
        line, capacity,
        "%s|%s|%s|%" PRId64 "|%s|%s|%s|%s\n",
        r->id, r->customer, r->account, r->amount,
        r->status, r->assigned, r->submitted, r->purpose
    );

    if (n < 0 || (size_t)n >= capacity)
        return -1;

    return n;
}

static int append_text(
    char *out, size_t capacity, size_t *used,
    const char *format, ...)
{
    if (*used >= capacity)
        return -1;

    va_list args;
    va_start(args, format);

    int n = vsnprintf(out + *used, capacity - *used, format, args);

    va_end(args);

    if (n < 0 || (size_t)n >= capacity - *used)
        return -1;

    *used += (size_t)n;
    return 0;
}

static int make_timestamp(char *out, size_t capacity)
{
    time_t now = time(NULL);
    struct tm tm_value;

    if (now == (time_t)-1 ||
        localtime_r(&now, &tm_value) == NULL) {
        return -1;
    }

    return strftime(out, capacity, "%Y-%m-%d %H:%M:%S", &tm_value)
           ? 0 : -1;
}

int loans_apply(
    const char *path,
    const char *customer,
    const char *account_id,
    int64_t amount_paise,
    const char *purpose,
    char *loan_id,
    size_t loan_id_size)
{
    if (!path ||
        !valid_field(customer, sizeof(((LoanRecord *)0)->customer)) ||
        !valid_field(account_id, sizeof(((LoanRecord *)0)->account)) ||
        !valid_field(purpose, sizeof(((LoanRecord *)0)->purpose)) ||
        amount_paise <= 0 ||
        !loan_id || loan_id_size == 0) {
        return LOAN_ERROR;
    }

    int lock_fd = acquire_lock(path, F_WRLCK);

    if (lock_fd == -1)
        return LOAN_ERROR;

    unsigned int highest_id = 0;
    FILE *input = fopen(path, "r");

    if (input != NULL) {
        char *line = NULL;
        size_t capacity = 0;
        ssize_t length;

        while ((length = getline(&line, &capacity, input)) != -1) {
            if ((size_t)length >= LOAN_LINE_SIZE)
                continue;

            char scratch[LOAN_LINE_SIZE];
            memcpy(scratch, line, (size_t)length + 1);

            LoanRecord old;

            if (parse_record(scratch, &old) == 0) {
                unsigned int number;

                if (sscanf(old.id, "LN%u", &number) == 1 &&
                    number > highest_id) {
                    highest_id = number;
                }
            }
        }

        free(line);

        if (ferror(input)) {
            fclose(input);
            release_lock(lock_fd);
            return LOAN_ERROR;
        }

        fclose(input);
    } else if (errno != ENOENT) {
        release_lock(lock_fd);
        return LOAN_ERROR;
    }

    if (highest_id >= 999999) {
        release_lock(lock_fd);
        return LOAN_ERROR;
    }

    LoanRecord r = {0};

    snprintf(r.id, sizeof(r.id), "LN%06u", highest_id + 1);
    snprintf(r.customer, sizeof(r.customer), "%s", customer);
    snprintf(r.account, sizeof(r.account), "%s", account_id);
    r.amount = amount_paise;
    snprintf(r.status, sizeof(r.status), "PENDING");
    snprintf(r.assigned, sizeof(r.assigned), "-");
    snprintf(r.purpose, sizeof(r.purpose), "%s", purpose);

    if (make_timestamp(r.submitted, sizeof(r.submitted)) != 0) {
        release_lock(lock_fd);
        return LOAN_ERROR;
    }

    char line[LOAN_LINE_SIZE];
    int length = format_record(&r, line, sizeof(line));

    if (length < 0 || strlen(r.id) + 1 > loan_id_size) {
        release_lock(lock_fd);
        return LOAN_ERROR;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);

    if (fd == -1) {
        release_lock(lock_fd);
        return LOAN_ERROR;
    }

    int result = LOAN_ERROR;

    if (write_all(fd, line, (size_t)length) == 0 &&
        fsync(fd) == 0) {
        strcpy(loan_id, r.id);
        result = LOAN_OK;
    }

    close(fd);
    release_lock(lock_fd);
    return result;
}

/*
 * List matching applications in a compact format suitable for
 * sending over the project's line-based client/server protocol.
 */
static int list_loans(
    const char *path,
    int mode,
    const char *filter,
    char *output,
    size_t output_size)
{
    if (!path || !output || output_size == 0)
        return LOAN_ERROR;

    output[0] = '\0';

    int lock_fd = acquire_lock(path, F_RDLCK);

    if (lock_fd == -1)
        return LOAN_ERROR;

    FILE *input = fopen(path, "r");

    if (!input) {
        int no_file = (errno == ENOENT);
        release_lock(lock_fd);

        if (no_file) {
            snprintf(output, output_size, "No loan applications found.");
            return LOAN_OK;
        }

        return LOAN_ERROR;
    }

    size_t used = 0;
    int matches = 0;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;

    while (matches < LOAN_LIST_LIMIT &&
           (length = getline(&line, &capacity, input)) != -1) {
        if ((size_t)length >= LOAN_LINE_SIZE)
            continue;

        char scratch[LOAN_LINE_SIZE];
        memcpy(scratch, line, (size_t)length + 1);

        LoanRecord r;

        if (parse_record(scratch, &r) != 0)
            continue;

        int match = 0;

        if (mode == 1)
            match = strcmp(r.customer, filter) == 0;
        else if (mode == 2)
            match = strcmp(r.status, "PENDING") == 0;
        else if (mode == 3)
            match = strcmp(r.assigned, filter) == 0;

        if (!match)
            continue;

        if (append_text(
                output, output_size, &used,
                "%s%s | Customer: %s | Account: %s"
                " | Amount: Rs %" PRId64 ".%02" PRId64
                " | Status: %s | Assigned: %s"
                " | Submitted: %s | Purpose: %s",
                matches ? " ; " : "",
                r.id, r.customer, r.account,
                r.amount / 100, r.amount % 100,
                r.status, r.assigned, r.submitted, r.purpose) != 0) {
            free(line);
            fclose(input);
            release_lock(lock_fd);
            return LOAN_ERROR;
        }

        matches++;
    }

    free(line);

    int read_error = ferror(input);
    fclose(input);
    release_lock(lock_fd);

    if (read_error)
        return LOAN_ERROR;

    if (matches == 0)
        snprintf(output, output_size, "No loan applications found.");

    return LOAN_OK;
}

int loans_list_customer(
    const char *path, const char *customer,
    char *output, size_t output_size)
{
    if (!valid_field(customer, 32))
        return LOAN_ERROR;

    return list_loans(path, 1, customer, output, output_size);
}

int loans_list_pending(
    const char *path, char *output, size_t output_size)
{
    return list_loans(path, 2, NULL, output, output_size);
}

int loans_list_assigned(
    const char *path, const char *employee,
    char *output, size_t output_size)
{
    if (!valid_field(employee, 32))
        return LOAN_ERROR;

    return list_loans(path, 3, employee, output, output_size);
}

/*
 * Updates are rewritten through a temporary file because text records
 * can change length when their status or assigned employee changes.
 */
static int update_loan(
    const char *path,
    const char *loan_id,
    const char *employee,
    int deciding,
    const char *decision,
    char *account_out,
    size_t account_out_size,
    int64_t *amount_out)
{
    if (!valid_field(loan_id, 16) ||
        !valid_field(employee, 32) ||
        ((account_out == NULL) != (amount_out == NULL)) ||
        (account_out != NULL && account_out_size == 0)) {
        return LOAN_ERROR;
    }

    int lock_fd = acquire_lock(path, F_WRLCK);

    if (lock_fd == -1)
        return LOAN_ERROR;

    FILE *input = fopen(path, "r");

    if (!input) {
        int no_file = (errno == ENOENT);
        release_lock(lock_fd);
        return no_file ? LOAN_NOT_FOUND : LOAN_ERROR;
    }

    char temp_path[1024];

    int n = snprintf(temp_path, sizeof(temp_path), "%s.tmpXXXXXX", path);

    if (n < 0 || (size_t)n >= sizeof(temp_path)) {
        fclose(input);
        release_lock(lock_fd);
        return LOAN_ERROR;
    }

    int temp_fd = mkstemp(temp_path);

    if (temp_fd == -1) {
        fclose(input);
        release_lock(lock_fd);
        return LOAN_ERROR;
    }

    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    int found = 0;
    int result = LOAN_NOT_FOUND;
    int failed = 0;

    while ((length = getline(&line, &capacity, input)) != -1) {
        int replace = 0;
        char updated[LOAN_LINE_SIZE];

        if ((size_t)length < LOAN_LINE_SIZE) {
            char scratch[LOAN_LINE_SIZE];
            memcpy(scratch, line, (size_t)length + 1);

            LoanRecord r;

            if (parse_record(scratch, &r) == 0 &&
                strcmp(r.id, loan_id) == 0) {
                found = 1;

                if (!deciding) {
                    if (strcmp(r.status, "PENDING") != 0 ||
                        strcmp(r.assigned, "-") != 0) {
                        result = LOAN_INVALID_STATE;
                    } else {
                        snprintf(r.status, sizeof(r.status), "ASSIGNED");
                        snprintf(r.assigned, sizeof(r.assigned), "%s",
                                 employee);
                        replace = 1;
                        result = LOAN_OK;
                    }
                } else {
                    if (strcmp(r.assigned, employee) != 0) {
                        result = LOAN_FORBIDDEN;
                    } else if (strcmp(r.status, "ASSIGNED") != 0) {
                        result = LOAN_INVALID_STATE;
                    } else if (account_out &&
                            copy_field(account_out, account_out_size,
                                        r.account) != 0) {
                        result = LOAN_ERROR;
                    } else {
                        if (amount_out)
                            *amount_out = r.amount;

                        snprintf(r.status, sizeof(r.status), "%s", decision);
                        replace = 1;
                        result = LOAN_OK;
                    }
                }

                if (replace) {
                    int out_length = format_record(
                        &r, updated, sizeof(updated));

                    if (out_length < 0 ||
                        write_all(temp_fd, updated,
                                  (size_t)out_length) != 0) {
                        failed = 1;
                        break;
                    }

                    continue;
                }
            }
        }

        /* Unmodified record: preserve its original bytes. */
        if (write_all(temp_fd, line, (size_t)length) != 0) {
            failed = 1;
            break;
        }
    }

    if (ferror(input))
        failed = 1;

    free(line);
    fclose(input);

    if (fsync(temp_fd) != 0)
        failed = 1;

    if (close(temp_fd) != 0)
        failed = 1;

    if (!found)
        result = LOAN_NOT_FOUND;

    if (failed)
        result = LOAN_ERROR;

    if (result == LOAN_OK) {
        if (rename(temp_path, path) != 0)
            result = LOAN_ERROR;
    }

    if (result != LOAN_OK)
        unlink(temp_path);

    release_lock(lock_fd);
    return result;
}

int loans_assign(
    const char *path, const char *loan_id, const char *employee)
{
    return update_loan(
        path, loan_id, employee, 0, NULL, NULL, 0, NULL
    );
}

int loans_decide(
    const char *path, const char *loan_id,
    const char *employee, const char *decision)
{
    if (!decision ||
        (strcmp(decision, "APPROVED") != 0 &&
         strcmp(decision, "REJECTED") != 0)) {
        return LOAN_ERROR;
    }

    return update_loan(
        path, loan_id, employee, 1, decision, NULL, 0, NULL
    );
}

int loans_decide_with_details(
    const char *path, const char *loan_id, const char *employee,
    const char *decision, char *account_id, size_t account_id_size,
    int64_t *amount_paise)
{
    if (!decision || !account_id || account_id_size == 0 || !amount_paise ||
        (strcmp(decision, "APPROVED") != 0 &&
         strcmp(decision, "REJECTED") != 0)) {
        return LOAN_ERROR;
    }

    return update_loan(
        path, loan_id, employee, 1, decision,
        account_id, account_id_size, amount_paise
    );
}