#ifndef LOANS_H
#define LOANS_H

#include <stddef.h>
#include <stdint.h>

#define LOAN_OK 0
#define LOAN_NOT_FOUND 1
#define LOAN_INVALID_STATE 2
#define LOAN_FORBIDDEN 3
#define LOAN_ERROR (-1)

int loans_apply(
    const char *path,
    const char *customer,
    const char *account_id,
    int64_t amount_paise,
    const char *purpose,
    char *loan_id,
    size_t loan_id_size
);

int loans_list_customer(
    const char *path,
    const char *customer,
    char *output,
    size_t output_size
);

int loans_list_pending(
    const char *path,
    char *output,
    size_t output_size
);

int loans_list_assigned(
    const char *path,
    const char *employee,
    char *output,
    size_t output_size
);

int loans_assign(
    const char *path,
    const char *loan_id,
    const char *employee
);

int loans_decide(
    const char *path,
    const char *loan_id,
    const char *employee,
    const char *decision
);
/* Same decision operation, also returns the approved loan details. */
int loans_decide_with_details(
    const char *path,
    const char *loan_id,
    const char *employee,
    const char *decision,
    char *account_id,
    size_t account_id_size,
    int64_t *amount_paise
);
#endif