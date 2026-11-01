#define _GNU_SOURCE
#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(
            stderr,
            "Usage: %s USERNAME CUSTOMER|EMPLOYEE|MANAGER|ADMIN\n",
            argv[0]
        );

        return EXIT_FAILURE;
    }

    UserRole role = auth_parse_role(argv[2]);

    if (role == ROLE_INVALID) {
        fprintf(stderr, "Invalid role.\n");
        return EXIT_FAILURE;
    }

    /*
     * getpass() disables terminal echo while the password
     * is entered. Do not pass passwords through command-line
     * arguments, because command lines can be observable.
     */
    char *password = getpass("Enter password (minimum 8 characters): ");

    if (password == NULL) {
        fprintf(stderr, "Could not read password.\n");
        return EXIT_FAILURE;
    }

    char password_copy[PASSWORD_MAX + 1];

    snprintf(
        password_copy,
        sizeof(password_copy),
        "%s",
        password
    );

    password = getpass("Confirm password: ");

    if (password == NULL ||
        strcmp(password_copy, password) != 0) {
        fprintf(stderr, "Passwords do not match.\n");
        return EXIT_FAILURE;
    }

    int rc = auth_create_user(
        "data/users.txt",
        argv[1],
        role,
        password_copy
    );

    if (rc == AUTH_OK) {
        printf("User created successfully.\n");
        printf("Username: %s\n", argv[1]);
        printf("Role: %s\n", auth_role_name(role));
        return EXIT_SUCCESS;
    }

    if (rc == AUTH_ALREADY_EXISTS) {
        fprintf(stderr, "That username already exists.\n");
    } else {
        perror("auth_create_user");
    }

    return EXIT_FAILURE;
}