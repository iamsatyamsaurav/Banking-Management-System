#include "storage.h"

#include <stdio.h>
#include <unistd.h>

#define ACCOUNTS "data/test_close_accounts.txt"
#define JOURNAL  "data/test_close_journal.log"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        return 1; \
    } \
} while (0)

int main(void)
{
    unlink(ACCOUNTS);
    unlink(JOURNAL);

    CHECK(storage_init(ACCOUNTS) == STORAGE_OK,
          "storage initialization");
    CHECK(storage_create_account(
              ACCOUNTS, "ACCCLOSE000001", "CUSCLOSE000001", 500) ==
              STORAGE_OK,
          "create account with balance");

    CHECK(storage_close_account(ACCOUNTS, "ACCCLOSE000001") ==
              STORAGE_ACCOUNT_NOT_EMPTY,
          "cannot close account with non-zero balance");

    CHECK(storage_withdraw(
              ACCOUNTS, JOURNAL, "ACCCLOSE000001", 500) == STORAGE_OK,
          "withdraw balance before closure");

    CHECK(storage_close_account(ACCOUNTS, "ACCCLOSE000001") == STORAGE_OK,
          "close zero-balance account");

    AccountRecord account;
    CHECK(storage_get_account(ACCOUNTS, "ACCCLOSE000001", &account) ==
              STORAGE_OK,
          "read closed account");
    CHECK(account.status == 'C' && account.balance_paise == 0,
          "closed state is persisted with zero balance");

    CHECK(storage_deposit(
              ACCOUNTS, JOURNAL, "ACCCLOSE000001", 100) ==
              STORAGE_INACTIVE_ACCOUNT,
          "closed account rejects deposits");
    CHECK(storage_set_account_status(
              ACCOUNTS, "ACCCLOSE000001", 'A') ==
              STORAGE_INACTIVE_ACCOUNT,
          "manager cannot reactivate a closed account");

    puts("PASS: zero-balance close, closed-state persistence, transaction block, and no reactivation.");
    return 0;
}
