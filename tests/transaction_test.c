#include "storage.h"

#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

static int show_balance(const char *accounts, const char *id)
{
    AccountRecord account;

    int rc = storage_get_account(accounts, id, &account);

    if (rc != STORAGE_OK)
        return rc;

    printf(
        "Balance of %s: ₹%" PRId64 ".%02" PRId64 "\n",
        id,
        account.balance_paise / 100,
        account.balance_paise % 100
    );

    return 0;
}

static int check_balance(
    const char *accounts,
    const char *id,
    int64_t expected_paise
)
{
    AccountRecord account;

    int rc = storage_get_account(accounts, id, &account);

    if (rc != STORAGE_OK)
        return -1;

    if (account.balance_paise != expected_paise) {
        fprintf(
            stderr,
            "Unexpected balance for %s: expected %" PRId64
            ", got %" PRId64 " paise\n",
            id,
            expected_paise,
            account.balance_paise
        );

        return -1;
    }

    return 0;
}

int main(void)
{
    /*
     * These are dedicated test files.
     * The test removes them at the beginning.
     * Do not change these paths to your real account files.
     */
    const char *accounts = "data/test_accounts.txt";
    const char *journal = "data/test_journal.log";

    unlink(accounts);
    unlink(journal);

    if (storage_init(accounts) != STORAGE_OK) {
        perror("storage_init");
        return 1;
    }

    if (storage_recover(accounts, journal) != STORAGE_OK) {
        perror("initial recovery");
        return 1;
    }

    /* Create Account A with ₹1000 and Account B with ₹200. */
    if (storage_create_account(
            accounts,
            "ACC0000000001",
            "CUS0000000001",
            100000
        ) != STORAGE_OK ||
        storage_create_account(
            accounts,
            "ACC0000000002",
            "CUS0000000002",
            20000
        ) != STORAGE_OK) {

        fprintf(stderr, "Account creation failed.\n");
        return 1;
    }

    /* Deposit ₹250 into Account A. */
    if (storage_deposit(
            accounts,
            journal,
            "ACC0000000001",
            25000
        ) != STORAGE_OK) {

        perror("deposit");
        return 1;
    }

    /* Withdraw ₹300 from Account A. */
    if (storage_withdraw(
            accounts,
            journal,
            "ACC0000000001",
            30000
        ) != STORAGE_OK) {

        perror("withdraw");
        return 1;
    }

    /* Transfer ₹125 from Account A to Account B. */
    if (storage_transfer(
            accounts,
            journal,
            "ACC0000000001",
            "ACC0000000002",
            12500
        ) != STORAGE_OK) {

        perror("transfer");
        return 1;
    }

    if (show_balance(accounts, "ACC0000000001") != 0 ||
        show_balance(accounts, "ACC0000000002") != 0) {

        fprintf(stderr, "Could not read balances.\n");
        return 1;
    }

    /* Expected balances: A = ₹825, B = ₹325. */
    if (check_balance(
            accounts, "ACC0000000001", 82500
        ) != 0 ||
        check_balance(
            accounts, "ACC0000000002", 32500
        ) != 0) {

        return 1;
    }

    /* Reject an unaffordable withdrawal. */
    int rc = storage_withdraw(
        accounts,
        journal,
        "ACC0000000001",
        200000
    );

    if (rc != STORAGE_INSUFFICIENT_FUNDS) {
        fprintf(
            stderr,
            "Expected insufficient funds, got %d\n",
            rc
        );

        return 1;
    }

    puts("Insufficient-funds withdrawal rejected.");

    /* Reject a transfer to the same account. */
    rc = storage_transfer(
        accounts,
        journal,
        "ACC0000000001",
        "ACC0000000001",
        100
    );

    if (rc != STORAGE_ERROR) {
        fprintf(
            stderr,
            "Expected invalid self-transfer, got %d\n",
            rc
        );

        return 1;
    }

    puts("Self-transfer rejected.");

    /*
     * Reapply committed journal records and verify that balances
     * remain correct after recovery.
     */
    if (storage_recover(accounts, journal) != STORAGE_OK) {
        perror("recovery after transactions");
        return 1;
    }

    if (show_balance(accounts, "ACC0000000001") != 0 ||
        show_balance(accounts, "ACC0000000002") != 0) {

        fprintf(stderr, "Could not read balances after recovery.\n");
        return 1;
    }

    if (check_balance(
            accounts, "ACC0000000001", 82500
        ) != 0 ||
        check_balance(
            accounts, "ACC0000000002", 32500
        ) != 0) {

        return 1;
    }

    puts("Transaction and recovery test passed.");

    return 0;
}