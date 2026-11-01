#define _GNU_SOURCE
#include "auth.h"

#include <stdio.h>
#include <unistd.h>

#define TEST_USERS_PATH "data/test_auth_cleanup_users.txt"

int main(void)
{
    const char *username = "rollback_test_user";
    const char *password = "DemoOnlyPass123!";
    UserRole role = ROLE_INVALID;

    unlink(TEST_USERS_PATH);

    if (auth_create_user(TEST_USERS_PATH, username,
                         ROLE_CUSTOMER, password) != AUTH_OK) {
        fprintf(stderr, "FAIL: could not create test user.\n");
        unlink(TEST_USERS_PATH);
        return 1;
    }

    if (auth_get_role(TEST_USERS_PATH, username, &role) != AUTH_OK ||
        role != ROLE_CUSTOMER) {
        fprintf(stderr, "FAIL: test user was not readable after creation.\n");
        unlink(TEST_USERS_PATH);
        return 1;
    }

    if (auth_verify(TEST_USERS_PATH, username, password, &role) != AUTH_OK ||
        role != ROLE_CUSTOMER ||
        auth_verify(TEST_USERS_PATH, username, "WrongPassword123", &role) !=
            AUTH_INVALID_CREDENTIALS) {
        fprintf(stderr, "FAIL: password verification behaved incorrectly.\n");
        unlink(TEST_USERS_PATH);
        return 1;
    }

    if (auth_change_password(TEST_USERS_PATH, username, password,
                             "NewDemoPassword123") != AUTH_OK ||
        auth_verify(TEST_USERS_PATH, username, password, &role) !=
            AUTH_INVALID_CREDENTIALS ||
        auth_verify(TEST_USERS_PATH, username, "NewDemoPassword123", &role) !=
            AUTH_OK) {
        fprintf(stderr, "FAIL: password change did not take effect.\n");
        unlink(TEST_USERS_PATH);
        return 1;
    }

    if (auth_set_role(TEST_USERS_PATH, username, ROLE_MANAGER) != AUTH_OK ||
        auth_get_role(TEST_USERS_PATH, username, &role) != AUTH_OK ||
        role != ROLE_MANAGER) {
        fprintf(stderr, "FAIL: role change did not take effect.\n");
        unlink(TEST_USERS_PATH);
        return 1;
    }

    if (auth_delete_user(TEST_USERS_PATH, username) != AUTH_OK) {
        fprintf(stderr, "FAIL: auth_delete_user did not remove the test user.\n");
        unlink(TEST_USERS_PATH);
        return 1;
    }

    if (auth_get_role(TEST_USERS_PATH, username, &role) == AUTH_OK) {
        fprintf(stderr, "FAIL: deleted test user still has an active role.\n");
        unlink(TEST_USERS_PATH);
        return 1;
    }

    /* Confirm deletion frees the username for a valid retry. */
    if (auth_create_user(TEST_USERS_PATH, username,
                         ROLE_CUSTOMER, password) != AUTH_OK) {
        fprintf(stderr, "FAIL: username could not be reused after rollback.\n");
        unlink(TEST_USERS_PATH);
        return 1;
    }

    if (auth_delete_user(TEST_USERS_PATH, username) != AUTH_OK) {
        fprintf(stderr, "FAIL: final test-user cleanup failed.\n");
        unlink(TEST_USERS_PATH);
        return 1;
    }

    unlink(TEST_USERS_PATH);
    printf("AUTH CLEANUP TEST PASSED.\n");
    return 0;
}
