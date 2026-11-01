#ifndef TRANSACTIONS_H
#define TRANSACTIONS_H

#include <stddef.h>
#include <stdint.h>

/*
 * A successful call records one transaction.
 *
 * Deposit:  source_account = "-"
 *           destination_account = account receiving money
 *
 * Withdrawal: source_account = account losing money
 *             destination_account = "-"
 *
 * Transfer: source_account = sender
 *           destination_account = receiver
 *
 * amount_paise is stored as an integer to avoid floating-point errors.
 */
int transaction_log(
    const char *path,
    const char *type,
    const char *source_account,
    const char *destination_account,
    int64_t amount_paise,
    const char *actor
);

/* Returns readable history for one account in the supplied buffer. */
int transaction_history(
    const char *path,
    const char *account_id,
    char *buffer,
    size_t buffer_size
);

#endif