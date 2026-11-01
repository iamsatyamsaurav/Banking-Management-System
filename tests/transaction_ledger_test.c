#include "transactions.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_LEDGER "data/test_transactions.txt"
#define OUTPUT_SIZE 8192

int main(void)
{
    unlink(TEST_LEDGER);

    /* Deposit Rs 500 into account 1. */
    if (transaction_log(
            TEST_LEDGER, "DEPOSIT", "-",
            "ACC0000000001", 50000,
            "CUS0000000001") != 0) {
        fprintf(stderr, "Failed to record deposit.\n");
        return 1;
    }

    /* Transfer Rs 100 from account 1 to account 2. */
    if (transaction_log(
            TEST_LEDGER, "TRANSFER",
            "ACC0000000001", "ACC0000000002",
            10000, "CUS0000000001") != 0) {
        fprintf(stderr, "Failed to record transfer.\n");
        return 1;
    }

    char history[OUTPUT_SIZE];

    if (transaction_history(
            TEST_LEDGER, "ACC0000000001",
            history, sizeof(history)) != 0) {
        fprintf(stderr, "Could not read account 1 history.\n");
        return 1;
    }

    printf("\n--- Account 1 history ---\n%s", history);

    if (strstr(history, "DEPOSIT") == NULL ||
        strstr(history, "TRANSFER") == NULL) {
        fprintf(stderr, "Account 1 history is incomplete.\n");
        return 1;
    }

    if (transaction_history(
            TEST_LEDGER, "ACC0000000002",
            history, sizeof(history)) != 0) {
        fprintf(stderr, "Could not read account 2 history.\n");
        return 1;
    }

    printf("\n--- Account 2 history ---\n%s", history);

    if (strstr(history, "TRANSFER") == NULL ||
        strstr(history, "ACC0000000001") == NULL) {
        fprintf(stderr, "Account 2 transfer history is incomplete.\n");
        return 1;
    }

    puts("\nTRANSACTION LEDGER TEST PASSED.");
    return 0;
}