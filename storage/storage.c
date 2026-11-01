#define _GNU_SOURCE

#include "storage.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t storage_mutex = PTHREAD_MUTEX_INITIALIZER;

#define JOURNAL_MAX_RECORDS 2
#define JOURNAL_MAGIC "BMSJNL1"

typedef struct {
    char magic[8];
    uint32_t record_count;
    uint64_t offsets[JOURNAL_MAX_RECORDS];
    unsigned char before[JOURNAL_MAX_RECORDS][ACCOUNT_RECORD_SIZE];
    char state;              /* P = prepared, C = committed */
} JournalRecord;


/* ---------- File locking ---------- */

static int set_file_lock(int fd, short lock_type, int command)
{
    struct flock lock;
    memset(&lock, 0, sizeof(lock));

    lock.l_type = lock_type;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    for (;;) {
        if (fcntl(fd, command, &lock) == 0)
            return 0;

        if (errno == EINTR)
            continue;

        return -1;
    }
}

static int begin_operation(
    const char *path,
    short lock_type,
    int *fd_out
)
{
    if (path == NULL || fd_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    int rc = pthread_mutex_lock(&storage_mutex);

    if (rc != 0) {
        errno = rc;
        return -1;
    }

    int fd = open(
        path,
        O_RDWR | O_CREAT | O_CLOEXEC,
        0600
    );

    if (fd < 0) {
        int saved = errno;
        pthread_mutex_unlock(&storage_mutex);
        errno = saved;
        return -1;
    }

    if (set_file_lock(fd, lock_type, F_OFD_SETLKW) < 0) {
        int saved = errno;
        close(fd);
        pthread_mutex_unlock(&storage_mutex);
        errno = saved;
        return -1;
    }

    *fd_out = fd;
    return 0;
}

static int end_operation(int fd)
{
    int failed = 0;
    int saved = 0;

    if (set_file_lock(fd, F_UNLCK, F_OFD_SETLK) < 0) {
        failed = 1;
        saved = errno;
    }

    if (close(fd) < 0 && !failed) {
        failed = 1;
        saved = errno;
    }

    int rc = pthread_mutex_unlock(&storage_mutex);

    if (rc != 0 && !failed) {
        failed = 1;
        saved = rc;
    }

    if (failed) {
        errno = saved;
        return -1;
    }

    return 0;
}

static int open_journal_locked(const char *path, int *fd_out)
{
    int fd = open(
        path,
        O_RDWR | O_CREAT | O_CLOEXEC,
        0600
    );

    if (fd < 0)
        return -1;

    if (set_file_lock(fd, F_WRLCK, F_OFD_SETLKW) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    *fd_out = fd;
    return 0;
}

static int close_journal_locked(int fd)
{
    int failed = 0;
    int saved = 0;

    if (set_file_lock(fd, F_UNLCK, F_OFD_SETLK) < 0) {
        failed = 1;
        saved = errno;
    }

    if (close(fd) < 0 && !failed) {
        failed = 1;
        saved = errno;
    }

    if (failed) {
        errno = saved;
        return -1;
    }

    return 0;
}


/* ---------- Input validation ---------- */

static int valid_identifier(const char *id, size_t max_length)
{
    if (id == NULL)
        return 0;

    size_t length = strnlen(id, max_length + 1);

    if (length == 0 || length > max_length)
        return 0;

    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)id[i];

        if (!(isalnum(c) || c == '_' || c == '-'))
            return 0;
    }

    return 1;
}


/* ---------- Reliable file reads and writes ---------- */

static int write_exact(
    int fd,
    const void *buffer,
    size_t length,
    off_t offset
)
{
    const unsigned char *bytes = buffer;
    size_t written = 0;

    while (written < length) {
        ssize_t n = pwrite(
            fd,
            bytes + written,
            length - written,
            offset + (off_t)written
        );

        if (n < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0) {
            errno = EIO;
            return -1;
        }

        written += (size_t)n;
    }

    return 0;
}

static int read_exact(
    int fd,
    void *buffer,
    size_t length,
    off_t offset
)
{
    unsigned char *bytes = buffer;
    size_t received = 0;

    while (received < length) {
        ssize_t n = pread(
            fd,
            bytes + received,
            length - received,
            offset + (off_t)received
        );

        if (n < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0) {
            errno = EIO;
            return -1;
        }

        received += (size_t)n;
    }

    return 0;
}


/* ---------- Account record encoding and decoding ---------- */

/*
 * Record layout (128 bytes):
 *
 * 0..15    account ID
 * 16       |
 * 17..32   customer ID
 * 33       |
 * 34..53   balance in paise, 20 digits
 * 54       |
 * 55       account status
 * 56..126  spaces
 * 127      newline
 */

static int decode_record(
    const char bytes[ACCOUNT_RECORD_SIZE],
    AccountRecord *account
)
{
    if (bytes[16] != '|' ||
        bytes[33] != '|' ||
        bytes[54] != '|' ||
        bytes[127] != '\n') {
        errno = EBADMSG;
        return -1;
    }

    for (size_t i = 56; i < 127; i++) {
        if (bytes[i] != ' ') {
            errno = EBADMSG;
            return -1;
        }
    }

    size_t id_length = ACCOUNT_ID_MAX;

    while (id_length > 0 && bytes[id_length - 1] == ' ')
        id_length--;

    if (id_length == 0) {
        errno = EBADMSG;
        return -1;
    }

    memcpy(account->account_id, bytes, id_length);
    account->account_id[id_length] = '\0';

    if (!valid_identifier(account->account_id, ACCOUNT_ID_MAX)) {
        errno = EBADMSG;
        return -1;
    }

    size_t customer_length = CUSTOMER_ID_MAX;

    while (customer_length > 0 &&
           bytes[17 + customer_length - 1] == ' ')
        customer_length--;

    if (customer_length == 0) {
        errno = EBADMSG;
        return -1;
    }

    memcpy(
        account->customer_id,
        bytes + 17,
        customer_length
    );

    account->customer_id[customer_length] = '\0';

    if (!valid_identifier(account->customer_id, CUSTOMER_ID_MAX)) {
        errno = EBADMSG;
        return -1;
    }

    char balance_text[21];

    memcpy(balance_text, bytes + 34, 20);
    balance_text[20] = '\0';

    for (size_t i = 0; i < 20; i++) {
        if (balance_text[i] < '0' || balance_text[i] > '9') {
            errno = EBADMSG;
            return -1;
        }
    }

    errno = 0;
    char *end = NULL;

    long long balance = strtoll(balance_text, &end, 10);

    if (errno == ERANGE ||
        end == balance_text ||
        *end != '\0' ||
        balance < 0) {
        errno = EBADMSG;
        return -1;
    }

    account->balance_paise = (int64_t)balance;

    if (bytes[55] != 'A' && bytes[55] != 'I' && bytes[55] != 'C') {
        errno = EBADMSG;
        return -1;
    }

    account->status = bytes[55];

    return 0;
}

static int encode_record(
    const AccountRecord *account,
    char bytes[ACCOUNT_RECORD_SIZE]
)
{
    if (!valid_identifier(account->account_id, ACCOUNT_ID_MAX) ||
        !valid_identifier(account->customer_id, CUSTOMER_ID_MAX) ||
        account->balance_paise < 0 ||
        (account->status != 'A' && account->status != 'I' &&
         account->status != 'C')) {
        errno = EINVAL;
        return -1;
    }

    memset(bytes, ' ', ACCOUNT_RECORD_SIZE);

    memcpy(
        bytes,
        account->account_id,
        strlen(account->account_id)
    );

    bytes[16] = '|';

    memcpy(
        bytes + 17,
        account->customer_id,
        strlen(account->customer_id)
    );

    bytes[33] = '|';

    char balance_text[21];

    int length = snprintf(
        balance_text,
        sizeof(balance_text),
        "%020" PRId64,
        account->balance_paise
    );

    if (length != 20) {
        errno = ERANGE;
        return -1;
    }

    memcpy(bytes + 34, balance_text, 20);

    bytes[54] = '|';
    bytes[55] = account->status;
    bytes[127] = '\n';

    return 0;
}


/* ---------- Account lookup ---------- */

/*
 * Acquire a lock on exactly one account record.
 *
 * Each record occupies ACCOUNT_RECORD_SIZE bytes.
 * The caller must open the accounts file separately for
 * each transaction so separate OFD locks can conflict.
 */
static int set_record_lock(int fd, short lock_type, off_t offset)
{
    if (offset < 0 || offset % ACCOUNT_RECORD_SIZE != 0) {
        errno = EINVAL;
        return -1;
    }

    struct flock lock = {0};

    lock.l_type = lock_type;
    lock.l_whence = SEEK_SET;
    lock.l_start = offset;
    lock.l_len = ACCOUNT_RECORD_SIZE;

    int command = lock_type == F_UNLCK
                  ? F_OFD_SETLK
                  : F_OFD_SETLKW;

    for (;;) {
        if (fcntl(fd, command, &lock) == 0)
            return 0;

        if (errno == EINTR)
            continue;

        return -1;
    }
}


/*
 * Open a separate index-lock file.
 *
 * This lock protects the account table's structure during
 * lookup and account creation. It does NOT lock all balances.
 *
 * Example:
 *   data/app_accounts.txt
 *   data/app_accounts.txt.indexlock
 */
static int open_index_lock(
    const char *accounts_path,
    short lock_type,
    int *fd_out
)
{
    if (accounts_path == NULL || fd_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t length = strlen(accounts_path);

    if (length > SIZE_MAX - 11) {
        errno = ENAMETOOLONG;
        return -1;
    }

    char *lock_path = malloc(length + 11);

    if (lock_path == NULL)
        return -1;

    snprintf(lock_path, length + 11,
             "%s.indexlock", accounts_path);

    int fd = open(
        lock_path,
        O_RDWR | O_CREAT | O_CLOEXEC,
        0600
    );

    free(lock_path);

    if (fd < 0)
        return -1;

    if (set_file_lock(fd, lock_type, F_OFD_SETLKW) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    *fd_out = fd;
    return 0;
}


/* Unlock and close the index-lock file. */
static int close_index_lock(int fd)
{
    if (set_file_lock(fd, F_UNLCK, F_OFD_SETLK) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    return close(fd);
}


/*
 * Find an account's byte offset while the caller holds the
 * index lock.
 *
 * Account IDs are immutable. We read only the ID field while
 * scanning, then acquire the actual record lock and reread
 * the complete record before a banking operation.
 *
 * Return:
 *   1  account found
 *   0  account not found
 *  -1  storage error or malformed file
 */
static int find_record_offset_locked(
    int fd,
    const char *account_id,
    off_t *offset_out
)
{
    struct stat info;

    if (fstat(fd, &info) < 0)
        return -1;

    if (info.st_size < 0 ||
        info.st_size % ACCOUNT_RECORD_SIZE != 0) {
        errno = EBADMSG;
        return -1;
    }

    char key[ACCOUNT_ID_MAX];

    memset(key, ' ', sizeof(key));

    memcpy(key, account_id, strlen(account_id));

    int found = 0;
    char prefix[ACCOUNT_ID_MAX + 1];

    for (off_t offset = 0;
         offset < info.st_size;
         offset += ACCOUNT_RECORD_SIZE) {

        if (read_exact(fd, prefix, sizeof(prefix), offset) < 0)
            return -1;

        if (prefix[ACCOUNT_ID_MAX] != '|') {
            errno = EBADMSG;
            return -1;
        }

        if (memcmp(prefix, key, ACCOUNT_ID_MAX) == 0) {
            if (found) {
                /* Duplicate account IDs indicate corrupt data. */
                errno = EBADMSG;
                return -1;
            }

            found = 1;
            *offset_out = offset;
        }
    }

    return found;
}


/*
 * Look up a record's stable byte offset.
 *
 * The index lock ensures that a concurrent account creation
 * cannot append a partially written record during the scan.
 */
static int locate_record_offset(
    const char *accounts_path,
    const char *account_id,
    off_t *offset_out
)
{
    if (!valid_identifier(account_id, ACCOUNT_ID_MAX) ||
        offset_out == NULL) {
        errno = EINVAL;
        return STORAGE_ERROR;
    }

    int index_fd;

    if (open_index_lock(
            accounts_path, F_RDLCK, &index_fd
        ) < 0) {
        return STORAGE_ERROR;
    }

    int data_fd = open(
        accounts_path,
        O_RDONLY | O_CLOEXEC
    );

    if (data_fd < 0) {
        int saved_errno = errno;
        close_index_lock(index_fd);
        errno = saved_errno;
        return STORAGE_ERROR;
    }

    int found = find_record_offset_locked(
        data_fd, account_id, offset_out
    );

    int saved_errno = errno;

    if (close(data_fd) < 0 && found >= 0) {
        saved_errno = errno;
        found = -1;
    }

    if (close_index_lock(index_fd) < 0) {
        saved_errno = errno;
        found = -1;
    }

    errno = saved_errno;

    if (found == 1)
        return STORAGE_OK;

    if (found == 0)
        return STORAGE_NOT_FOUND;

    return STORAGE_ERROR;
}

/* ---------- Small one-slot undo journal ---------- */

static int journal_record_valid(const JournalRecord *entry)
{
    if (memcmp(entry->magic, JOURNAL_MAGIC, sizeof(entry->magic)) != 0 ||
        entry->record_count < 1 || entry->record_count > JOURNAL_MAX_RECORDS ||
        (entry->state != 'P' && entry->state != 'C'))
        return 0;

    for (size_t i = 0; i < entry->record_count; ++i) {
        AccountRecord before;
        if (entry->offsets[i] > (uint64_t)LLONG_MAX ||
            entry->offsets[i] % ACCOUNT_RECORD_SIZE != 0 ||
            decode_record((const char *)entry->before[i], &before) < 0)
            return 0;
        for (size_t j = 0; j < i; ++j)
            if (entry->offsets[i] == entry->offsets[j]) return 0;
    }
    return 1;
}

static int clear_journal(int fd)
{
    return ftruncate(fd, 0) == 0 && fsync(fd) == 0 ? 0 : -1;
}

/* A short final record is safe to discard: account writes start only after
   the complete journal entry has been synchronized. */
static int read_journal(int fd, JournalRecord *entry, int *present)
{
    struct stat st;
    *present = 0;
    if (fstat(fd, &st) < 0 || st.st_size < 0) return STORAGE_ERROR;
    if (st.st_size == 0) return STORAGE_OK;
    if (st.st_size < (off_t)sizeof(*entry))
        return clear_journal(fd) == 0 ? STORAGE_OK : STORAGE_FATAL;
    if (st.st_size != (off_t)sizeof(*entry)) {
        errno = EBADMSG;
        return STORAGE_ERROR;
    }
    if (read_exact(fd, entry, sizeof(*entry), 0) < 0) return STORAGE_ERROR;
    if (!journal_record_valid(entry)) {
        errno = EBADMSG;
        return STORAGE_ERROR;
    }
    *present = 1;
    return STORAGE_OK;
}

/* A leftover committed entry is safe to clear. A prepared one must first be
   recovered under the whole-account-file lock at server startup. */
static int prepare_journal_for_update(int fd)
{
    JournalRecord entry;
    int present, result = read_journal(fd, &entry, &present);
    if (result != STORAGE_OK || !present) return result;
    if (entry.state == 'P') {
        errno = EBUSY;
        return STORAGE_FATAL;
    }
    return clear_journal(fd) == 0 ? STORAGE_OK : STORAGE_FATAL;
}

static int apply_journaled_updates(
    int accounts_fd,
    const char *journal_path,
    size_t record_count,
    const off_t offsets[JOURNAL_MAX_RECORDS],
    unsigned char before[JOURNAL_MAX_RECORDS][ACCOUNT_RECORD_SIZE],
    unsigned char after[JOURNAL_MAX_RECORDS][ACCOUNT_RECORD_SIZE]
)
{
    if (record_count < 1 || record_count > JOURNAL_MAX_RECORDS) {
        errno = EINVAL;
        return STORAGE_ERROR;
    }

    int jfd;
    if (open_journal_locked(journal_path, &jfd) < 0) return STORAGE_ERROR;
    int result = prepare_journal_for_update(jfd);
    if (result != STORAGE_OK) goto done;

    JournalRecord entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.magic, JOURNAL_MAGIC, sizeof(entry.magic));
    entry.record_count = (uint32_t)record_count;
    entry.state = 'P';
    for (size_t i = 0; i < record_count; ++i) {
        if (offsets[i] < 0 || offsets[i] % ACCOUNT_RECORD_SIZE != 0) {
            errno = EINVAL;
            result = STORAGE_ERROR;
            goto done;
        }
        entry.offsets[i] = (uint64_t)offsets[i];
        memcpy(entry.before[i], before[i], ACCOUNT_RECORD_SIZE);
    }
    if (!journal_record_valid(&entry)) {
        errno = EBADMSG;
        result = STORAGE_ERROR;
        goto done;
    }

    /* Save only original rows; committed rows are already fsynced. */
    if (ftruncate(jfd, 0) < 0 ||
        write_exact(jfd, &entry, sizeof(entry), 0) < 0 || fsync(jfd) < 0) {
        result = STORAGE_ERROR;
        goto done;
    }

    for (size_t i = 0; i < record_count; ++i)
        if (write_exact(accounts_fd, after[i], ACCOUNT_RECORD_SIZE,
                        offsets[i]) < 0) goto rollback;
    if (fsync(accounts_fd) < 0) goto rollback;

    /* Commit only after every account row is durable. */
    {
        char committed = 'C';
        if (write_exact(jfd, &committed, 1,
                        (off_t)offsetof(JournalRecord, state)) < 0 || fsync(jfd) < 0) {
            int saved = errno;
            char prepared = 'P';
            if (write_exact(jfd, &prepared, 1,
                            (off_t)offsetof(JournalRecord, state)) < 0 || fsync(jfd) < 0) {
                errno = saved;
                result = STORAGE_FATAL;
                goto done;
            }
            errno = saved;
            goto rollback;
        }
    }

    /* Cleanup is optional after commit; startup can safely clear a C entry. */
    (void)clear_journal(jfd);
    result = STORAGE_OK;
    goto done;

rollback:
    {
        int saved = errno, failed = 0;
        for (size_t i = 0; i < record_count; ++i)
            if (write_exact(accounts_fd, before[i], ACCOUNT_RECORD_SIZE,
                            offsets[i]) < 0) failed = 1;
        if (fsync(accounts_fd) < 0) failed = 1;
        if (!failed && clear_journal(jfd) < 0) failed = 1;
        errno = saved;
        result = failed ? STORAGE_FATAL : STORAGE_ERROR;
    }

done:
    if (close_journal_locked(jfd) < 0) result = STORAGE_FATAL;
    return result;
}

/* ---------- Public storage functions ---------- */

int storage_init(const char *path)
{
    int fd;

    if (begin_operation(path, F_WRLCK, &fd) < 0)
        return STORAGE_ERROR;

    int result = STORAGE_ERROR;
    struct stat info;

    if (fstat(fd, &info) == 0) {
        if (info.st_size >= 0 &&
            info.st_size % ACCOUNT_RECORD_SIZE == 0) {

            if (fsync(fd) == 0)
                result = STORAGE_OK;
        } else {
            errno = EBADMSG;
        }
    }

    if (end_operation(fd) < 0)
        return STORAGE_ERROR;

    return result;
}

int storage_create_account(
    const char *path,
    const char *account_id,
    const char *customer_id,
    int64_t initial_balance_paise
)
{
    if (!valid_identifier(account_id, ACCOUNT_ID_MAX) ||
        !valid_identifier(customer_id, CUSTOMER_ID_MAX) ||
        initial_balance_paise < 0) {
        errno = EINVAL;
        return STORAGE_ERROR;
    }

    /*
     * Take an exclusive index lock. Other account creations
     * and account-ID lookups must wait until the append finishes.
     */
    int index_fd;

    if (open_index_lock(path, F_WRLCK, &index_fd) < 0)
        return STORAGE_ERROR;

    int fd = open(
        path,
        O_RDWR | O_CREAT | O_CLOEXEC,
        0600
    );

    if (fd < 0) {
        int saved_errno = errno;
        close_index_lock(index_fd);
        errno = saved_errno;
        return STORAGE_ERROR;
    }

    int result = STORAGE_ERROR;
    off_t existing_offset;

    int found = find_record_offset_locked(
        fd, account_id, &existing_offset
    );

    if (found == 1) {
        result = STORAGE_ALREADY_EXISTS;
        goto cleanup;
    }

    if (found < 0)
        goto cleanup;

    /*
     * Appending a new record doesn't move existing records.
     * The exclusive index lock prevents concurrent append
     * operations from choosing the same starting offset.
     */
    struct stat info;

    if (fstat(fd, &info) < 0)
        goto cleanup;

    if (info.st_size < 0 ||
        info.st_size % ACCOUNT_RECORD_SIZE != 0) {
        errno = EBADMSG;
        goto cleanup;
    }

    AccountRecord account = {0};

    snprintf(
        account.account_id,
        sizeof(account.account_id),
        "%s",
        account_id
    );

    snprintf(
        account.customer_id,
        sizeof(account.customer_id),
        "%s",
        customer_id
    );

    account.balance_paise = initial_balance_paise;
    account.status = 'A';
    account.record_offset = info.st_size;

    char bytes[ACCOUNT_RECORD_SIZE];

    if (encode_record(&account, bytes) < 0)
        goto cleanup;

    if (write_exact(
            fd, bytes, sizeof(bytes), info.st_size
        ) < 0) {
        goto cleanup;
    }

    /* Make the new record's data durable before reporting success. */
    if (fsync(fd) < 0)
        goto cleanup;

    result = STORAGE_OK;

cleanup:
    {
        int saved_errno = errno;

        if (close(fd) < 0 && result == STORAGE_OK)
            result = STORAGE_ERROR;

        if (close_index_lock(index_fd) < 0)
            result = STORAGE_FATAL;

        errno = saved_errno;
    }

    return result;
}

int storage_get_account(
    const char *path,
    const char *account_id,
    AccountRecord *result
)
{
    if (result == NULL ||
        !valid_identifier(account_id, ACCOUNT_ID_MAX)) {
        errno = EINVAL;
        return STORAGE_ERROR;
    }

    /*
     * Find the stable offset without locking every account's
     * balance for the duration of the operation.
     */
    off_t offset;

    int rc = locate_record_offset(
        path, account_id, &offset
    );

    if (rc != STORAGE_OK)
        return rc;

    /*
     * Open a fresh file description for this operation.
     * This is important for OFD locks to coordinate properly.
     */
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return STORAGE_ERROR;

    if (set_record_lock(fd, F_RDLCK, offset) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return STORAGE_ERROR;
    }

    char bytes[ACCOUNT_RECORD_SIZE];
    AccountRecord account;

    int status = STORAGE_ERROR;

    if (read_exact(
            fd, bytes, sizeof(bytes), offset
        ) == 0 &&
        decode_record(bytes, &account) == 0 &&
        strcmp(account.account_id, account_id) == 0) {

        account.record_offset = offset;
        *result = account;
        status = STORAGE_OK;
    } else {
        /*
         * A record that doesn't match the discovered ID is
         * treated as a storage-integrity error.
         */
        if (errno == 0)
            errno = EBADMSG;
    }

    int saved_errno = errno;

    if (set_record_lock(fd, F_UNLCK, offset) < 0)
        status = STORAGE_FATAL;

    if (close(fd) < 0)
        status = STORAGE_FATAL;

    errno = saved_errno;

    return status;
}

/* ---------- Journal recovery ---------- */

int storage_recover(const char *accounts_path, const char *journal_path)
{
    int accounts_fd;
    if (begin_operation(accounts_path, F_WRLCK, &accounts_fd) < 0)
        return STORAGE_ERROR;

    int journal_fd = -1, result = STORAGE_ERROR;
    if (open_journal_locked(journal_path, &journal_fd) < 0)
        goto finish_accounts;

    JournalRecord entry;
    int present = 0;
    result = read_journal(journal_fd, &entry, &present);
    if (result != STORAGE_OK || !present) goto finish_journal;

    struct stat accounts_stat;
    if (fstat(accounts_fd, &accounts_stat) < 0 || accounts_stat.st_size < 0) {
        result = STORAGE_ERROR;
        goto finish_journal;
    }
    for (size_t i = 0; i < entry.record_count; ++i) {
        uint64_t pos = entry.offsets[i];
        if (accounts_stat.st_size < ACCOUNT_RECORD_SIZE ||
            pos > (uint64_t)LLONG_MAX ||
            (off_t)pos > accounts_stat.st_size - ACCOUNT_RECORD_SIZE) {
            errno = EBADMSG;
            result = STORAGE_ERROR;
            goto finish_journal;
        }
    }

    /* P means the transaction did not commit: restore the old rows.
       C means the updated rows were fsynced before the commit marker. */
    if (entry.state == 'P') {
        for (size_t i = 0; i < entry.record_count; ++i)
            if (write_exact(accounts_fd, entry.before[i], ACCOUNT_RECORD_SIZE,
                            (off_t)entry.offsets[i]) < 0) {
                result = STORAGE_FATAL;
                goto finish_journal;
            }
        if (fsync(accounts_fd) < 0) {
            result = STORAGE_FATAL;
            goto finish_journal;
        }
    }
    result = clear_journal(journal_fd) == 0 ? STORAGE_OK : STORAGE_FATAL;

finish_journal:
    if (close_journal_locked(journal_fd) < 0) result = STORAGE_FATAL;
finish_accounts:
    if (end_operation(accounts_fd) < 0) result = STORAGE_FATAL;
    return result;
}


/* ---------- Deposits and withdrawals ---------- */

static int adjust_balance(
    const char *accounts_path,
    const char *journal_path,
    const char *account_id,
    int64_t amount_paise,
    int is_deposit
)
{
    if (accounts_path == NULL ||
        journal_path == NULL ||
        !valid_identifier(account_id, ACCOUNT_ID_MAX)) {
        errno = EINVAL;
        return STORAGE_ERROR;
    }

    if (amount_paise <= 0)
        return STORAGE_INVALID_AMOUNT;

    /* Step 1: Locate the account's record offset. */
    off_t offset;

    int result = locate_record_offset(
        accounts_path, account_id, &offset
    );

    if (result != STORAGE_OK)
        return result;

    /*
     * Step 2: Open the account file independently and acquire
     * an exclusive lock on this record only.
     */
    int fd = open(
        accounts_path,
        O_RDWR | O_CLOEXEC
    );

    if (fd < 0)
        return STORAGE_ERROR;

    int lock_held = 0;

    if (set_record_lock(fd, F_WRLCK, offset) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return STORAGE_ERROR;
    }

    lock_held = 1;

    /*
     * Step 3: Read the latest record AFTER acquiring the lock.
     *
     * Another transaction might have changed the balance
     * between locating the record and acquiring its lock.
     */
    unsigned char before_record[ACCOUNT_RECORD_SIZE];

    AccountRecord account;

    if (read_exact(
            fd, before_record, sizeof(before_record), offset
        ) < 0 ||
        decode_record(
            (const char *)before_record, &account
        ) < 0 ||
        strcmp(account.account_id, account_id) != 0) {

        result = STORAGE_ERROR;
        goto cleanup;
    }

    account.record_offset = offset;

    if (account.status != 'A') {
        result = STORAGE_INACTIVE_ACCOUNT;
        goto cleanup;
    }

    /* Step 4: Validate the resulting balance. */
    int64_t new_balance;

    if (is_deposit) {
        if (account.balance_paise >
            INT64_MAX - amount_paise) {

            result = STORAGE_INVALID_AMOUNT;
            goto cleanup;
        }

        new_balance = account.balance_paise + amount_paise;

    } else {
        if (account.balance_paise < amount_paise) {
            result = STORAGE_INSUFFICIENT_FUNDS;
            goto cleanup;
        }

        new_balance = account.balance_paise - amount_paise;
    }

    /*
     * Build the old and new rows. The journal saves the old row
     * before the new balance is written.
     */
    off_t offsets[JOURNAL_MAX_RECORDS] = {
        offset, 0
    };

    unsigned char before[JOURNAL_MAX_RECORDS]
                       [ACCOUNT_RECORD_SIZE] = {{0}};

    unsigned char after[JOURNAL_MAX_RECORDS]
                      [ACCOUNT_RECORD_SIZE] = {{0}};

    memcpy(
        before[0],
        before_record,
        ACCOUNT_RECORD_SIZE
    );

    AccountRecord updated = account;
    updated.balance_paise = new_balance;

    if (encode_record(
            &updated, (char *)after[0]
        ) < 0) {

        result = STORAGE_ERROR;
        goto cleanup;
    }

    /*
     * Step 6: Execute the journalled update while retaining
     * the exclusive lock on this account record.
     */
    result = apply_journaled_updates(
        fd,
        journal_path,
        1,
        offsets,
        before,
        after
    );

cleanup:
    /*
     * Always release the lock on every exit path.
     * Never leave the account locked after a normal return.
     */
    {
        int saved_errno = errno;

        if (lock_held &&
            set_record_lock(fd, F_UNLCK, offset) < 0) {
            result = STORAGE_FATAL;
        }

        if (close(fd) < 0)
            result = STORAGE_FATAL;

        errno = saved_errno;
    }

    return result;
}

int storage_deposit(
    const char *accounts_path,
    const char *journal_path,
    const char *account_id,
    int64_t amount_paise
)
{
    return adjust_balance(
        accounts_path,
        journal_path,
        account_id,
        amount_paise,
        1
    );
}

int storage_withdraw(
    const char *accounts_path,
    const char *journal_path,
    const char *account_id,
    int64_t amount_paise
)
{
    return adjust_balance(
        accounts_path,
        journal_path,
        account_id,
        amount_paise,
        0
    );
}

/*
 * Change an account's status while holding an exclusive lock
 * on that account's fixed-width record.
 *
 * Only the status changes. The balance and customer ID remain
 * unchanged. fsync() makes the update durable before returning.
 */
int storage_set_account_status(
    const char *path,
    const char *account_id,
    char status)
{
    if (!valid_identifier(account_id, ACCOUNT_ID_MAX) ||
        (status != 'A' && status != 'I' && status != 'C')) {
        errno = EINVAL;
        return STORAGE_ERROR;
    }

    /* Find the stable byte offset of the account record. */
    AccountRecord located;

    int rc = storage_get_account(path, account_id, &located);

    if (rc != STORAGE_OK)
        return rc;

    int fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd == -1)
        return STORAGE_ERROR;

    off_t offset = located.record_offset;
    int result = STORAGE_ERROR;

    /* Lock only this account's 128-byte record. */
    if (set_record_lock(fd, F_WRLCK, offset) == -1)
        goto cleanup;

    char bytes[ACCOUNT_RECORD_SIZE];
    AccountRecord current;

    /* Re-read the record after acquiring the lock. */
    if (read_exact(fd, bytes, sizeof(bytes), offset) == -1)
        goto unlock;

    if (decode_record(bytes, &current) == -1)
        goto unlock;

    /* Confirm that the record at this offset is still the account. */
    if (strcmp(current.account_id, account_id) != 0) {
        result = STORAGE_NOT_FOUND;
        goto unlock;
    }

    current.record_offset = offset;

    /* A closed account is permanent and cannot be reactivated. */
    if (current.status == 'C' && status != 'C') {
        result = STORAGE_INACTIVE_ACCOUNT;
        goto unlock;
    }

    /* Close only zero-balance accounts; do not strand customer funds. */
    if (status == 'C' && current.status != 'C' &&
        current.balance_paise != 0) {
        result = STORAGE_ACCOUNT_NOT_EMPTY;
        goto unlock;
    }

    /* Avoid unnecessary writes when the status is already correct. */
    if (current.status == status) {
        result = STORAGE_OK;
        goto unlock;
    }

    current.status = status;

    if (encode_record(&current, bytes) == -1)
        goto unlock;

    if (write_exact(fd, bytes, sizeof(bytes), offset) == -1)
        goto unlock;

    if (fsync(fd) == -1)
        goto unlock;

    result = STORAGE_OK;

unlock:
    (void)set_record_lock(fd, F_UNLCK, offset);

cleanup:
    if (close(fd) == -1 && result == STORAGE_OK)
        result = STORAGE_FATAL;

    return result;
}

/* Permanently close a zero-balance account. */
int storage_close_account(const char *path, const char *account_id)
{
    return storage_set_account_status(path, account_id, 'C');
}

/* ---------- Two-account transfers ---------- */

int storage_transfer(
    const char *accounts_path,
    const char *journal_path,
    const char *from_account_id,
    const char *to_account_id,
    int64_t amount_paise
)
{
    if (accounts_path == NULL ||
        journal_path == NULL ||
        !valid_identifier(from_account_id, ACCOUNT_ID_MAX) ||
        !valid_identifier(to_account_id, ACCOUNT_ID_MAX)) {
        errno = EINVAL;
        return STORAGE_ERROR;
    }

    if (strcmp(from_account_id, to_account_id) == 0)
        return STORAGE_ERROR;

    if (amount_paise <= 0)
        return STORAGE_INVALID_AMOUNT;

    /*
     * Step 1: Locate both immutable record offsets.
     * The offsets remain stable because our storage design
     * appends accounts and never moves existing records.
     */
    off_t source_offset;
    off_t destination_offset;

    int result = locate_record_offset(
        accounts_path, from_account_id, &source_offset
    );

    if (result != STORAGE_OK)
        return result;

    result = locate_record_offset(
        accounts_path, to_account_id, &destination_offset
    );

    if (result != STORAGE_OK)
        return result;

    if (source_offset == destination_offset)
        return STORAGE_ERROR;

    /* Open a new file description for this transfer. */
    int fd = open(
        accounts_path,
        O_RDWR | O_CLOEXEC
    );

    if (fd < 0)
        return STORAGE_ERROR;

    /*
     * Step 2: Acquire both exclusive record locks in a
     * consistent order, regardless of transfer direction.
     */
    off_t first_offset =
        source_offset < destination_offset
        ? source_offset
        : destination_offset;

    off_t second_offset =
        source_offset < destination_offset
        ? destination_offset
        : source_offset;

    int first_locked = 0;
    int second_locked = 0;

    if (set_record_lock(
            fd, F_WRLCK, first_offset
        ) < 0) {

        result = STORAGE_ERROR;
        goto cleanup;
    }

    first_locked = 1;

    if (set_record_lock(
            fd, F_WRLCK, second_offset
        ) < 0) {

        result = STORAGE_ERROR;
        goto cleanup;
    }

    second_locked = 1;

    /*
     * Step 3: Read the two latest records while both locks
     * are held. The order of these arrays is source then
     * destination, not the order in which the locks were acquired.
     */
    unsigned char before[JOURNAL_MAX_RECORDS]
                       [ACCOUNT_RECORD_SIZE] = {{0}};

    unsigned char after[JOURNAL_MAX_RECORDS]
                      [ACCOUNT_RECORD_SIZE] = {{0}};

    off_t offsets[JOURNAL_MAX_RECORDS] = {
        source_offset,
        destination_offset
    };

    if (read_exact(
            fd,
            before[0],
            ACCOUNT_RECORD_SIZE,
            source_offset
        ) < 0 ||
        read_exact(
            fd,
            before[1],
            ACCOUNT_RECORD_SIZE,
            destination_offset
        ) < 0) {

        result = STORAGE_ERROR;
        goto cleanup;
    }

    AccountRecord source;
    AccountRecord destination;

    if (decode_record(
            (const char *)before[0], &source
        ) < 0 ||
        decode_record(
            (const char *)before[1], &destination
        ) < 0 ||
        strcmp(source.account_id, from_account_id) != 0 ||
        strcmp(destination.account_id, to_account_id) != 0) {

        result = STORAGE_ERROR;
        goto cleanup;
    }

    source.record_offset = source_offset;
    destination.record_offset = destination_offset;

    /* Step 4: Verify account states and money constraints. */
    if (source.status != 'A' || destination.status != 'A') {
        result = STORAGE_INACTIVE_ACCOUNT;
        goto cleanup;
    }

    if (source.balance_paise < amount_paise) {
        result = STORAGE_INSUFFICIENT_FUNDS;
        goto cleanup;
    }

    if (destination.balance_paise >
        INT64_MAX - amount_paise) {

        result = STORAGE_INVALID_AMOUNT;
        goto cleanup;
    }

    /*
     * Step 5: Calculate both new balances.
     * Neither record is modified until validation succeeds.
     */
    AccountRecord new_source = source;
    AccountRecord new_destination = destination;

    new_source.balance_paise -= amount_paise;
    new_destination.balance_paise += amount_paise;

    if (encode_record(
            &new_source, (char *)after[0]
        ) < 0 ||
        encode_record(
            &new_destination, (char *)after[1]
        ) < 0) {

        result = STORAGE_ERROR;
        goto cleanup;
    }

    /*
     * Save both old rows in one journal entry, then apply both new
     * rows while retaining both record locks.
     */
    result = apply_journaled_updates(
        fd,
        journal_path,
        2,
        offsets,
        before,
        after
    );

cleanup:
    /*
     * Release the second lock before the first.
     * Each lock covers exactly one ACCOUNT_RECORD_SIZE range.
     */
    if (second_locked &&
        set_record_lock(
            fd, F_UNLCK, second_offset
        ) < 0) {
        result = STORAGE_FATAL;
    }

    if (first_locked &&
        set_record_lock(
            fd, F_UNLCK, first_offset
        ) < 0) {
        result = STORAGE_FATAL;
    }

    if (close(fd) < 0)
        result = STORAGE_FATAL;

    return result;
}
/* Store customer/employee details using their fixed-width text formats. */
static int valid_profile_text(const char *value, size_t length)
{
    if (!value || length == 0) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c == '|' || c == '\n' || c == '\r' || c < 32) return 0;
    }
    return 1;
}

static int set_profile_details(
    const char *path, const char *key, const char *name, const char *phone,
    size_t key_width, size_t record_size, int strict_key)
{
    if (!path || !key || !name || !phone) return STORAGE_INVALID_DETAILS;

    size_t key_len = strnlen(key, key_width + 1);
    size_t name_len = strnlen(name, CUSTOMER_NAME_MAX + 1);
    size_t phone_len = strnlen(phone, CUSTOMER_PHONE_MAX + 1);
    if (!key_len || key_len > key_width || !name_len || name_len > CUSTOMER_NAME_MAX ||
        phone_len < 10 || phone_len > CUSTOMER_PHONE_MAX ||
        (strict_key ? !valid_identifier(key, key_width) : !valid_profile_text(key, key_len)) ||
        !valid_profile_text(name, name_len)) {
        errno = EINVAL;
        return STORAGE_INVALID_DETAILS;
    }
    for (size_t i = 0; i < phone_len; ++i) {
        if (phone[i] < '0' || phone[i] > '9') {
            errno = EINVAL;
            return STORAGE_INVALID_DETAILS;
        }
    }

    char record[EMPLOYEE_DETAILS_RECORD_SIZE + 1];
    int n = snprintf(record, sizeof(record), "%-*.*s|%-80.80s|%-15.15s\n",
                     (int)key_width, (int)key_width, key, name, phone);
    if (n < 0 || (size_t)n != record_size) {
        errno = EINVAL;
        return STORAGE_ERROR;
    }

    int fd;
    if (begin_operation(path, F_WRLCK, &fd) < 0) return STORAGE_ERROR;
    int result = STORAGE_ERROR, saved_errno = 0;
    struct stat info;

    if (fstat(fd, &info) < 0) {
        saved_errno = errno;
        goto done;
    }
    if (info.st_size < 0 || info.st_size % (off_t)record_size != 0) {
        saved_errno = EBADMSG;
        goto done;
    }

    off_t target = info.st_size;
    char existing[EMPLOYEE_DETAILS_RECORD_SIZE];
    char existing_key[EMPLOYEE_USERNAME_MAX + 1];
    for (off_t offset = 0; offset < info.st_size; offset += (off_t)record_size) {
        if (read_exact(fd, existing, record_size, offset) < 0) {
            saved_errno = errno;
            goto done;
        }
        memcpy(existing_key, existing, key_width);
        existing_key[key_width] = '\0';
        for (size_t i = key_width; i > 0 && existing_key[i - 1] == ' '; --i)
            existing_key[i - 1] = '\0';
        if (strcmp(existing_key, key) == 0) {
            target = offset;
            break;
        }
    }

    if (write_exact(fd, record, record_size, target) < 0 || fsync(fd) < 0) {
        saved_errno = errno;
        goto done;
    }
    result = STORAGE_OK;

done:
    if (end_operation(fd) < 0 && result == STORAGE_OK) {
        result = STORAGE_ERROR;
        saved_errno = errno;
    }
    if (result != STORAGE_OK) errno = saved_errno ? saved_errno : EIO;
    return result;
}

int storage_set_customer_details(
    const char *path, const char *customer_id, const char *full_name, const char *phone)
{
    return set_profile_details(path, customer_id, full_name, phone,
                               CUSTOMER_ID_MAX, CUSTOMER_DETAILS_RECORD_SIZE, 0);
}

int storage_set_employee_details(
    const char *path, const char *username, const char *full_name, const char *phone)
{
    return set_profile_details(path, username, full_name, phone,
                               EMPLOYEE_USERNAME_MAX, EMPLOYEE_DETAILS_RECORD_SIZE, 1);
}
