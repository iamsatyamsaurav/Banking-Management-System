#include "storage.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define THREAD_COUNT 10
#define WITHDRAWAL_PAISE 20000
#define INITIAL_BALANCE_PAISE 100000

#define ACCOUNTS_PATH "data/concurrent_accounts.txt"
#define JOURNAL_PATH  "data/concurrent_journal.log"

typedef struct {
    int thread_number;
    int result;
} WithdrawalTask;

/*
 * Each thread attempts to withdraw ₹200 from the same account.
 * The storage layer is responsible for locking and validation.
 */
static void *withdraw_money(void *arg)
{
    WithdrawalTask *task = arg;

    task->result = storage_withdraw(
        ACCOUNTS_PATH,
        JOURNAL_PATH,
        "ACC0000000001",
        WITHDRAWAL_PAISE
    );

    return NULL;
}

int main(void)
{
    /*
     * Use separate test files so we do not modify the accounts
     * used by the running application.
     */
    unlink(ACCOUNTS_PATH);
    unlink(JOURNAL_PATH);
    unlink(ACCOUNTS_PATH ".indexlock");

    if (storage_init(ACCOUNTS_PATH) != STORAGE_OK) {
        fprintf(stderr, "Could not initialize test accounts.\n");
        return EXIT_FAILURE;
    }

    if (storage_recover(
            ACCOUNTS_PATH,
            JOURNAL_PATH
        ) != STORAGE_OK) {
        fprintf(stderr, "Initial recovery failed.\n");
        return EXIT_FAILURE;
    }

    /* Create one account with ₹1,000. */
    if (storage_create_account(
            ACCOUNTS_PATH,
            "ACC0000000001",
            "CUS0000000001",
            INITIAL_BALANCE_PAISE
        ) != STORAGE_OK) {
        fprintf(stderr, "Could not create test account.\n");
        return EXIT_FAILURE;
    }

    pthread_t threads[THREAD_COUNT];
    WithdrawalTask tasks[THREAD_COUNT];

    int created = 0;

    /*
     * Start all withdrawal threads.
     * Each thread executes independently and competes for
     * the same account's exclusive record lock.
     */
    for (int i = 0; i < THREAD_COUNT; i++) {
        tasks[i].thread_number = i + 1;
        tasks[i].result = STORAGE_ERROR;

        int rc = pthread_create(
            &threads[i],
            NULL,
            withdraw_money,
            &tasks[i]
        );

        if (rc != 0) {
            fprintf(stderr,
                    "Could not create thread %d.\n",
                    i + 1);
            break;
        }

        created++;
    }

    /*
     * Wait for all threads that were successfully created.
     * Their results are checked only after they have finished.
     */
    for (int i = 0; i < created; i++) {
        int rc = pthread_join(threads[i], NULL);

        if (rc != 0) {
            fprintf(stderr,
                    "Could not join thread %d.\n",
                    i + 1);
            return EXIT_FAILURE;
        }
    }

    if (created != THREAD_COUNT) {
        fprintf(stderr,
                "Test incomplete: not all threads were created.\n");
        return EXIT_FAILURE;
    }

    int successful = 0;
    int insufficient_funds = 0;

    for (int i = 0; i < THREAD_COUNT; i++) {
        if (tasks[i].result == STORAGE_OK) {
            successful++;
        } else if (
            tasks[i].result == STORAGE_INSUFFICIENT_FUNDS
        ) {
            insufficient_funds++;
        } else {
            fprintf(stderr,
                    "Thread %d returned unexpected status %d.\n",
                    tasks[i].thread_number,
                    tasks[i].result);
            return EXIT_FAILURE;
        }
    }

    AccountRecord account;

    if (storage_get_account(
            ACCOUNTS_PATH,
            "ACC0000000001",
            &account
        ) != STORAGE_OK) {
        fprintf(stderr, "Could not read final balance.\n");
        return EXIT_FAILURE;
    }

    printf("Threads started: %d\n", created);
    printf("Successful withdrawals: %d\n", successful);
    printf("Rejected withdrawals: %d\n", insufficient_funds);

    printf("Final balance: ₹%" PRId64 ".%02" PRId64 "\n",
           account.balance_paise / 100,
           account.balance_paise % 100);

    if (successful != 5 ||
        insufficient_funds != 5 ||
        account.balance_paise != 0) {
        fprintf(stderr, "CONCURRENCY TEST FAILED.\n");
        return EXIT_FAILURE;
    }

    printf("CONCURRENCY TEST PASSED.\n");

    return EXIT_SUCCESS;
}