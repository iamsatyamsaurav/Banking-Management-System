#include <inttypes.h>
#include "storage.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    const char *path = "data/accounts.txt";

    if (storage_init(path) != STORAGE_OK) {
        perror("storage_init");
        return 1;
    }

    /*
     * Initial balance = 123450 paise = ₹1234.50.
     * Re-running the test won't create a duplicate account.
     */
    int rc = storage_create_account(
        path,
        "ACC0000000001",
        "CUS0000000001",
        123450
    );

    if (rc == STORAGE_OK) {
        printf("Account created successfully.\n");
    } else if (rc == STORAGE_ALREADY_EXISTS) {
        printf("Account already exists; checking its record.\n");
    } else {
        perror("storage_create_account");
        return 1;
    }

    AccountRecord account;

    rc = storage_get_account(
        path,
        "ACC0000000001",
        &account
    );

    if (rc != STORAGE_OK) {
        perror("storage_get_account");
        return 1;
    }

    printf("Account ID: %s\n", account.account_id);
    printf("Customer ID: %s\n", account.customer_id);

    printf("Balance: ₹%" PRId64 ".%02" PRId64 "\n",
           account.balance_paise / 100,
           account.balance_paise % 100);

    printf("Status: %c\n", account.status);
    printf("File offset: %lld\n",
           (long long)account.record_offset);


    /* Profile updates should replace an existing fixed-width row. */
    const char *customer_file = "data/test_customer_profiles.txt";
    const char *employee_file = "data/test_employee_profiles.txt";
    unlink(customer_file);
    unlink(employee_file);
    if (storage_set_customer_details(customer_file, "CUS0000000001",
                                     "Customer One", "9876543210") != STORAGE_OK ||
        storage_set_customer_details(customer_file, "CUS0000000001",
                                     "Updated Customer", "9876543211") != STORAGE_OK ||
        storage_set_employee_details(employee_file, "staff_1",
                                     "Employee One", "9876543212") != STORAGE_OK) {
        fprintf(stderr, "profile-details storage test failed\n");
        return 1;
    }
    struct stat profile_info;
    if (stat(customer_file, &profile_info) < 0 ||
        profile_info.st_size != CUSTOMER_DETAILS_RECORD_SIZE ||
        stat(employee_file, &profile_info) < 0 ||
        profile_info.st_size != EMPLOYEE_DETAILS_RECORD_SIZE) {
        fprintf(stderr, "profile rows were not stored/replaced correctly\n");
        return 1;
    }
    unlink(customer_file);
    unlink(employee_file);
    puts("PROFILE DETAILS TEST PASSED");

    return 0;
}