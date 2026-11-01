#define _GNU_SOURCE
#include "storage.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define ACCOUNTS "data/test_journal_recovery_accounts.txt"
#define JOURNAL  "data/test_journal_recovery.log"
#define MAX_RECORDS 2

typedef struct {
    char magic[8];
    uint32_t record_count;
    uint64_t offsets[MAX_RECORDS];
    unsigned char before[MAX_RECORDS][ACCOUNT_RECORD_SIZE];
    char state;
} TestJournalRecord;

static int read_row(int fd, unsigned char row[ACCOUNT_RECORD_SIZE], off_t off)
{
    ssize_t n = pread(fd, row, ACCOUNT_RECORD_SIZE, off);
    return n == ACCOUNT_RECORD_SIZE ? 0 : -1;
}

static int write_all(int fd, const void *data, size_t size)
{
    const char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int expect_balance(const char *id, int64_t expected)
{
    AccountRecord account;
    if (storage_get_account(ACCOUNTS, id, &account) != STORAGE_OK ||
        account.balance_paise != expected) {
        fprintf(stderr, "Unexpected balance for %s\n", id);
        return -1;
    }
    return 0;
}

int main(void)
{
    unlink(ACCOUNTS);
    unlink(JOURNAL);
    unlink(ACCOUNTS ".indexlock");

    if (storage_init(ACCOUNTS) != STORAGE_OK ||
        storage_recover(ACCOUNTS, JOURNAL) != STORAGE_OK ||
        storage_create_account(ACCOUNTS, "ACC0000000001", "CUS0000000001", 100000) != STORAGE_OK ||
        storage_create_account(ACCOUNTS, "ACC0000000002", "CUS0000000002", 10000) != STORAGE_OK) {
        perror("test setup");
        return 1;
    }

    int fd = open(ACCOUNTS, O_RDWR | O_CLOEXEC);
    if (fd < 0) return 1;
    TestJournalRecord entry;
    memset(&entry, 0, sizeof(entry));
    memcpy(entry.magic, "BMSJNL1", 8);
    entry.record_count = 2;
    entry.offsets[0] = 0;
    entry.offsets[1] = ACCOUNT_RECORD_SIZE;
    entry.state = 'P';
    if (read_row(fd, entry.before[0], 0) < 0 ||
        read_row(fd, entry.before[1], ACCOUNT_RECORD_SIZE) < 0) {
        perror("read before-images");
        close(fd);
        return 1;
    }

    if (storage_transfer(ACCOUNTS, JOURNAL, "ACC0000000001", "ACC0000000002", 20000) != STORAGE_OK ||
        expect_balance("ACC0000000001", 80000) < 0 ||
        expect_balance("ACC0000000002", 30000) < 0) {
        fprintf(stderr, "Transfer setup failed\n");
        close(fd);
        return 1;
    }

    int jfd = open(JOURNAL, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (jfd < 0 || write_all(jfd, &entry, sizeof(entry)) < 0 || fsync(jfd) < 0) {
        perror("write prepared journal");
        if (jfd >= 0) close(jfd);
        close(fd);
        return 1;
    }
    close(jfd);
    close(fd);

    if (storage_recover(ACCOUNTS, JOURNAL) != STORAGE_OK ||
        expect_balance("ACC0000000001", 100000) < 0 ||
        expect_balance("ACC0000000002", 10000) < 0) {
        fprintf(stderr, "Prepared journal was not rolled back\n");
        return 1;
    }

    puts("PREPARED JOURNAL RECOVERY TEST PASSED");
    return 0;
}
