#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <sys/types.h>

#define ACCOUNT_ID_MAX 16
#define CUSTOMER_ID_MAX 16
#define ACCOUNT_RECORD_SIZE 128

#define CUSTOMER_NAME_MAX 80
#define CUSTOMER_PHONE_MAX 15
#define CUSTOMER_DETAILS_RECORD_SIZE 114

#define STORAGE_INVALID_DETAILS 6
#define STORAGE_ACCOUNT_NOT_EMPTY 7
#define STORAGE_OK 0
#define STORAGE_ALREADY_EXISTS 1
#define STORAGE_NOT_FOUND 2
#define STORAGE_INSUFFICIENT_FUNDS 3
#define STORAGE_INACTIVE_ACCOUNT 4
#define STORAGE_INVALID_AMOUNT 5
#define STORAGE_ERROR (-1)
#define STORAGE_FATAL (-2)

#define EMPLOYEE_USERNAME_MAX 31
#define EMPLOYEE_DETAILS_RECORD_SIZE 129

typedef struct {
    char account_id[ACCOUNT_ID_MAX + 1];
    char customer_id[CUSTOMER_ID_MAX + 1];
    int64_t balance_paise;
    char status;
    off_t record_offset;
} AccountRecord;

int storage_init(const char *path);

int storage_create_account(
    const char *path,
    const char *account_id,
    const char *customer_id,
    int64_t initial_balance_paise
);

int storage_get_account(
    const char *path,
    const char *account_id,
    AccountRecord *result
);

int storage_recover(
    const char *accounts_path,
    const char *journal_path
);

int storage_deposit(
    const char *accounts_path,
    const char *journal_path,
    const char *account_id,
    int64_t amount_paise
);

int storage_withdraw(
    const char *accounts_path,
    const char *journal_path,
    const char *account_id,
    int64_t amount_paise
);

int storage_transfer(
    const char *accounts_path,
    const char *journal_path,
    const char *from_account_id,
    const char *to_account_id,
    int64_t amount_paise
);
int storage_set_account_status(
    const char *path,
    const char *account_id,
    char status
);
int storage_close_account(const char *path, const char *account_id);
int storage_set_customer_details(
    const char *path,
    const char *customer_id,
    const char *full_name,
    const char *phone
);
int storage_set_employee_details(
    const char *path,
    const char *username,
    const char *full_name,
    const char *phone
);
#endif