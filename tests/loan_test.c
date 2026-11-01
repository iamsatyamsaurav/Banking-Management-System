#include "loans.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_PATH "data/test_loans.txt"

int main(void)
{
    unlink(TEST_PATH);
    unlink(TEST_PATH ".lock");

    char loan_id[16];
    char output[8192];

    /* Customer submits a loan application for Rs 2,500. */
    if (loans_apply(
            TEST_PATH,
            "CUS0000000001",
            "ACC0000000001",
            250000,
            "Education expenses",
            loan_id,
            sizeof(loan_id)) != LOAN_OK) {
        fprintf(stderr, "FAIL: Could not create loan application.\n");
        return 1;
    }

    printf("Created loan application: %s\n", loan_id);

    if (loans_list_customer(
            TEST_PATH, "CUS0000000001",
            output, sizeof(output)) != LOAN_OK ||
        strstr(output, "PENDING") == NULL) {
        fprintf(stderr, "FAIL: Pending application not visible.\n");
        return 1;
    }

    puts("PASS: Customer can view pending application.");

    /* Manager assigns it to employee01. */
    if (loans_assign(TEST_PATH, loan_id, "employee01") != LOAN_OK) {
        fprintf(stderr, "FAIL: Loan assignment failed.\n");
        return 1;
    }

    if (loans_list_assigned(
            TEST_PATH, "employee01",
            output, sizeof(output)) != LOAN_OK ||
        strstr(output, "ASSIGNED") == NULL) {
        fprintf(stderr, "FAIL: Assigned application not visible.\n");
        return 1;
    }

    puts("PASS: Assigned employee can view the application.");

    /* The assigned employee approves the application. */
    if (loans_decide(
            TEST_PATH, loan_id, "employee01", "APPROVED") != LOAN_OK) {
        fprintf(stderr, "FAIL: Loan approval failed.\n");
        return 1;
    }

    if (loans_list_customer(
            TEST_PATH, "CUS0000000001",
            output, sizeof(output)) != LOAN_OK ||
        strstr(output, "APPROVED") == NULL) {
        fprintf(stderr, "FAIL: Approval status was not saved.\n");
        return 1;
    }

    puts("PASS: Customer can see the approved status.");

    /* An already decided application must not be decided again. */
    if (loans_decide(
            TEST_PATH, loan_id, "employee01", "REJECTED")
            != LOAN_INVALID_STATE) {
        fprintf(stderr, "FAIL: Repeated decision was not blocked.\n");
        return 1;
    }

    puts("PASS: Repeated decision is rejected.");
    puts("LOAN STORAGE TEST PASSED.");

    return 0;
}