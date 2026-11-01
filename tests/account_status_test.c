#include "storage.h"

#include <stdio.h>
#include <unistd.h>

#define ACCOUNTS "data/test_status_accounts.txt"
#define JOURNAL  "data/test_status_journal.log"

int main(void)
{
    unlink(ACCOUNTS);
    unlink(JOURNAL);

    if (storage_init(ACCOUNTS) != STORAGE_OK) {
        fprintf(stderr, "FAIL: Storage initialization.\n");
        return 1;
    }

    if (storage_create_account(
            ACCOUNTS,
            "ACCSTATUS0001",
            "CUSSTATUS0001",
            100000) != STORAGE_OK) {
        fprintf(stderr, "FAIL: Account creation.\n");
        return 1;
    }

    if (storage_set_account_status(
            ACCOUNTS, "ACCSTATUS0001", 'I') != STORAGE_OK) {
        fprintf(stderr, "FAIL: Account deactivation.\n");
        return 1;
    }

    AccountRecord account;

    if (storage_get_account(
            ACCOUNTS, "ACCSTATUS0001", &account) != STORAGE_OK ||
        account.status != 'I') {
        fprintf(stderr, "FAIL: Inactive status not saved.\n");
        return 1;
    }

    puts("PASS: Account deactivated.");

    if (storage_withdraw(
            ACCOUNTS, JOURNAL, "ACCSTATUS0001", 100) !=
            STORAGE_INACTIVE_ACCOUNT) {
        fprintf(stderr, "FAIL: Inactive account allowed withdrawal.\n");
        return 1;
    }

    puts("PASS: Inactive account rejects withdrawal.");

    if (storage_set_account_status(
            ACCOUNTS, "ACCSTATUS0001", 'A') != STORAGE_OK) {
        fprintf(stderr, "FAIL: Account reactivation.\n");
        return 1;
    }

    if (storage_get_account(
            ACCOUNTS, "ACCSTATUS0001", &account) != STORAGE_OK ||
        account.status != 'A') {
        fprintf(stderr, "FAIL: Active status not saved.\n");
        return 1;
    }

    puts("PASS: Account reactivated.");

    if (storage_withdraw(
            ACCOUNTS, JOURNAL, "ACCSTATUS0001", 100) != STORAGE_OK) {
        fprintf(stderr, "FAIL: Active account withdrawal.\n");
        return 1;
    }

    if (storage_get_account(
            ACCOUNTS, "ACCSTATUS0001", &account) != STORAGE_OK ||
        account.balance_paise != 99900) {
        fprintf(stderr, "FAIL: Balance changed incorrectly.\n");
        return 1;
    }

    puts("PASS: Active account can transact.");
    puts("ACCOUNT STATUS TEST PASSED.");

    return 0;
}