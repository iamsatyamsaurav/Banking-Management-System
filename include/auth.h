#ifndef AUTH_H
#define AUTH_H

#include <stddef.h>

#define AUTH_OK 0
#define AUTH_ALREADY_EXISTS 1
#define AUTH_INVALID_CREDENTIALS 2
#define AUTH_INACTIVE_USER 3
#define AUTH_ERROR (-1)

#define USERNAME_MAX 31
#define PASSWORD_MAX 128

typedef enum {
    ROLE_CUSTOMER,
    ROLE_EMPLOYEE,
    ROLE_MANAGER,
    ROLE_ADMIN,
    ROLE_INVALID
} UserRole;

/* Convert a role name into the corresponding enum. */
UserRole auth_parse_role(const char *name);

/* Return the printable name of a role. */
const char *auth_role_name(UserRole role);

/* Create a user with a demo password stored in the local users file. */
int auth_create_user(
    const char *path,
    const char *username,
    UserRole role,
    const char *password
);

/* Verify credentials and return the user's actual role. */
int auth_verify(
    const char *path,
    const char *username,
    const char *password,
    UserRole *role_out
);

/* Look up an active user's role without needing their password. */
int auth_get_role(
    const char *path,
    const char *username,
    UserRole *role_out
);
int auth_delete_user(const char *path, const char *username);
int auth_set_role(
    const char *path,
    const char *username,
    UserRole new_role
);
int auth_change_password(
    const char *path,
    const char *username,
    const char *current_password,
    const char *new_password
);
#endif