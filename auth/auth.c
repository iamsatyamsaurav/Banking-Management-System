#define _GNU_SOURCE
#include "auth.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Each user is one 180-byte text record, so updates happen in place.
 * Layout: username[31] | role[8] | password_length[3] |
 * password[128] | status[1] + padding/newline.
 */
#define USER_RECORD_SIZE 180
#define USER_FILE_LIMIT (16 * 1024 * 1024)
#define NAME_OFFSET 0
#define NAME_WIDTH 31
#define ROLE_OFFSET 32
#define ROLE_WIDTH 8
#define PASSLEN_OFFSET 41
#define PASSWORD_OFFSET 45
#define STATUS_OFFSET 174

#define USER_ACTIVE 'A'
#define USER_INACTIVE 'I'
#define USER_DELETED 'D'

typedef struct {
    char username[USERNAME_MAX + 1];
    char password[PASSWORD_MAX + 1];
    UserRole role;
    char status;
} UserRecord;

UserRole auth_parse_role(const char *name)
{
    if (name == NULL) return ROLE_INVALID;
    if (strcmp(name, "CUSTOMER") == 0) return ROLE_CUSTOMER;
    if (strcmp(name, "EMPLOYEE") == 0) return ROLE_EMPLOYEE;
    if (strcmp(name, "MANAGER") == 0) return ROLE_MANAGER;
    if (strcmp(name, "ADMIN") == 0) return ROLE_ADMIN;
    return ROLE_INVALID;
}

const char *auth_role_name(UserRole role)
{
    switch (role) {
        case ROLE_CUSTOMER: return "CUSTOMER";
        case ROLE_EMPLOYEE: return "EMPLOYEE";
        case ROLE_MANAGER:  return "MANAGER";
        case ROLE_ADMIN:    return "ADMIN";
        default:            return "INVALID";
    }
}

static int valid_username(const char *s)
{
    if (s == NULL) return 0;
    size_t n = strnlen(s, USERNAME_MAX + 1);
    if (n == 0 || n > USERNAME_MAX) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int valid_password(const char *s)
{
    if (s == NULL) return 0;
    size_t n = strnlen(s, PASSWORD_MAX + 1);
    if (n < 8 || n > PASSWORD_MAX) return 0;
    return strpbrk(s, "|\r\n") == NULL;
}

static int lock_file(int fd, short type)
{
    struct flock lock = {0};
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0; /* The whole user file. */
    int cmd = type == F_UNLCK ? F_OFD_SETLK : F_OFD_SETLKW;

    while (fcntl(fd, cmd, &lock) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static int read_exact(int fd, void *buffer, size_t size, off_t offset)
{
    unsigned char *p = buffer;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, p + done, size - done, offset + (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n == 0) errno = EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}

static int write_exact(int fd, const void *buffer, size_t size, off_t offset)
{
    const unsigned char *p = buffer;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pwrite(fd, p + done, size - done, offset + (off_t)done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n == 0) errno = EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}

static size_t trim_field(const char *src, size_t width)
{
    while (width > 0 && src[width - 1] == ' ') --width;
    return width;
}

static int encode_user(const UserRecord *u, char row[USER_RECORD_SIZE])
{
    const char *role = auth_role_name(u->role);
    size_t name_len = strlen(u->username);
    size_t pass_len = strlen(u->password);
    if (!valid_username(u->username) || !valid_password(u->password) ||
        u->role == ROLE_INVALID ||
        (u->status != USER_ACTIVE && u->status != USER_INACTIVE &&
         u->status != USER_DELETED)) {
        errno = EINVAL;
        return -1;
    }

    memset(row, ' ', USER_RECORD_SIZE);
    memcpy(row + NAME_OFFSET, u->username, name_len);
    row[31] = '|';
    memcpy(row + ROLE_OFFSET, role, strlen(role));
    row[40] = '|';
    row[41] = (char)('0' + pass_len / 100);
    row[42] = (char)('0' + (pass_len / 10) % 10);
    row[43] = (char)('0' + pass_len % 10);
    row[44] = '|';
    memcpy(row + PASSWORD_OFFSET, u->password, pass_len);
    row[173] = '|';
    row[STATUS_OFFSET] = u->status;
    row[179] = '\n';
    return 0;
}

static int decode_user(const char row[USER_RECORD_SIZE], UserRecord *u)
{
    if (row[31] != '|' || row[40] != '|' || row[44] != '|' ||
        row[173] != '|' || row[179] != '\n') {
        errno = EBADMSG;
        return -1;
    }

    size_t name_len = trim_field(row + NAME_OFFSET, NAME_WIDTH);
    size_t role_len = trim_field(row + ROLE_OFFSET, ROLE_WIDTH);
    size_t password_len = 0;
    for (int i = PASSLEN_OFFSET; i < PASSLEN_OFFSET + 3; ++i) {
        if (row[i] < '0' || row[i] > '9') { errno = EBADMSG; return -1; }
        password_len = password_len * 10 + (size_t)(row[i] - '0');
    }
    if (name_len == 0 || name_len > USERNAME_MAX || role_len == 0 ||
        password_len < 8 || password_len > PASSWORD_MAX ||
        (row[STATUS_OFFSET] != USER_ACTIVE &&
         row[STATUS_OFFSET] != USER_INACTIVE &&
         row[STATUS_OFFSET] != USER_DELETED)) {
        errno = EBADMSG;
        return -1;
    }

    memset(u, 0, sizeof(*u));
    memcpy(u->username, row + NAME_OFFSET, name_len);
    u->username[name_len] = '\0';
    if (!valid_username(u->username)) { errno = EBADMSG; return -1; }

    char role_name[ROLE_WIDTH + 1];
    memcpy(role_name, row + ROLE_OFFSET, role_len);
    role_name[role_len] = '\0';
    u->role = auth_parse_role(role_name);
    if (u->role == ROLE_INVALID) { errno = EBADMSG; return -1; }

    memcpy(u->password, row + PASSWORD_OFFSET, password_len);
    u->password[password_len] = '\0';
    if (!valid_password(u->password)) { errno = EBADMSG; return -1; }
    u->status = row[STATUS_OFFSET];
    return 0;
}

static int file_size(int fd, off_t *size_out)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (st.st_size < 0 || st.st_size > USER_FILE_LIMIT ||
        st.st_size % USER_RECORD_SIZE != 0) {
        errno = EBADMSG;
        return -1;
    }
    *size_out = st.st_size;
    return 0;
}

/* Finds a non-deleted user and remembers the first reusable deleted row. */
static int find_user(int fd, const char *username, UserRecord *out,
                     off_t *offset_out, off_t *deleted_offset_out)
{
    off_t size;
    if (file_size(fd, &size) < 0) return -1;
    if (deleted_offset_out) *deleted_offset_out = -1;

    for (off_t offset = 0; offset < size; offset += USER_RECORD_SIZE) {
        char row[USER_RECORD_SIZE];
        UserRecord current;
        if (read_exact(fd, row, sizeof(row), offset) < 0 ||
            decode_user(row, &current) < 0) return -1;

        if (current.status == USER_DELETED) {
            if (deleted_offset_out && *deleted_offset_out < 0)
                *deleted_offset_out = offset;
            continue;
        }
        if (strcmp(current.username, username) == 0) {
            if (out) *out = current;
            if (offset_out) *offset_out = offset;
            return 1;
        }
    }
    return 0;
}

static int finish_operation(int fd, int locked, int result)
{
    int saved = errno;
    if (locked && lock_file(fd, F_UNLCK) < 0)
        result = AUTH_ERROR;
    if (close(fd) < 0)
        result = AUTH_ERROR;
    errno = saved;
    return result;
}

int auth_create_user(const char *path, const char *username, UserRole role,
                     const char *password)
{
    if (path == NULL || !valid_username(username) ||
        !valid_password(password) || role == ROLE_INVALID) {
        errno = EINVAL;
        return AUTH_ERROR;
    }

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return AUTH_ERROR;
    if (lock_file(fd, F_WRLCK) < 0) { int e = errno; close(fd); errno = e; return AUTH_ERROR; }

    UserRecord existing;
    off_t reusable = -1;
    int found = find_user(fd, username, &existing, NULL, &reusable);
    if (found < 0) return finish_operation(fd, 1, AUTH_ERROR);
    if (found == 1) return finish_operation(fd, 1, AUTH_ALREADY_EXISTS);

    off_t size;
    if (file_size(fd, &size) < 0) return finish_operation(fd, 1, AUTH_ERROR);
    UserRecord user = {0};
    snprintf(user.username, sizeof(user.username), "%s", username);
    snprintf(user.password, sizeof(user.password), "%s", password);
    user.role = role;
    user.status = USER_ACTIVE;
    char row[USER_RECORD_SIZE];
    if (encode_user(&user, row) < 0) return finish_operation(fd, 1, AUTH_ERROR);

    off_t target = reusable >= 0 ? reusable : size;
    int result = AUTH_ERROR;
    if (write_exact(fd, row, sizeof(row), target) == 0 && fsync(fd) == 0)
        result = AUTH_OK;
    return finish_operation(fd, 1, result);
}

int auth_verify(const char *path, const char *username, const char *password,
                UserRole *role_out)
{
    if (path == NULL || !valid_username(username) ||
        !valid_password(password) || role_out == NULL)
        return AUTH_INVALID_CREDENTIALS;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return AUTH_ERROR;
    if (lock_file(fd, F_RDLCK) < 0) { int e = errno; close(fd); errno = e; return AUTH_ERROR; }

    UserRecord user;
    int found = find_user(fd, username, &user, NULL, NULL);
    int result = AUTH_ERROR;
    if (found == 0) result = AUTH_INVALID_CREDENTIALS;
    else if (found < 0) result = AUTH_ERROR;
    else if (user.status == USER_INACTIVE) result = AUTH_INACTIVE_USER;
    else if (strcmp(password, user.password) != 0) result = AUTH_INVALID_CREDENTIALS;
    else { *role_out = user.role; result = AUTH_OK; }
    return finish_operation(fd, 1, result);
}

int auth_get_role(const char *path, const char *username, UserRole *role_out)
{
    if (path == NULL || !valid_username(username) || role_out == NULL) {
        errno = EINVAL;
        return AUTH_ERROR;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return AUTH_ERROR;
    if (lock_file(fd, F_RDLCK) < 0) { int e = errno; close(fd); errno = e; return AUTH_ERROR; }

    UserRecord user;
    int found = find_user(fd, username, &user, NULL, NULL);
    int result;
    if (found < 0) result = AUTH_ERROR;
    else if (found == 0) result = AUTH_INVALID_CREDENTIALS;
    else if (user.status == USER_INACTIVE) result = AUTH_INACTIVE_USER;
    else { *role_out = user.role; result = AUTH_OK; }
    return finish_operation(fd, 1, result);
}

int auth_delete_user(const char *path, const char *username)
{
    if (path == NULL || !valid_username(username)) { errno = EINVAL; return AUTH_ERROR; }
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return AUTH_ERROR;
    if (lock_file(fd, F_WRLCK) < 0) { int e = errno; close(fd); errno = e; return AUTH_ERROR; }

    UserRecord user;
    off_t offset;
    int found = find_user(fd, username, &user, &offset, NULL);
    int result = AUTH_ERROR;
    if (found == 0) result = AUTH_INVALID_CREDENTIALS;
    else if (found == 1) {
        /* Clear the old credentials while keeping this row reusable. */
        snprintf(user.username, sizeof(user.username), "deleted");
        snprintf(user.password, sizeof(user.password), "Deleted00");
        user.role = ROLE_CUSTOMER;
        user.status = USER_DELETED;
        char row[USER_RECORD_SIZE];
        if (encode_user(&user, row) == 0 &&
            write_exact(fd, row, sizeof(row), offset) == 0 && fsync(fd) == 0)
            result = AUTH_OK;
    }
    return finish_operation(fd, 1, result);
}

int auth_set_role(const char *path, const char *username, UserRole new_role)
{
    if (path == NULL || !valid_username(username) ||
        new_role < ROLE_CUSTOMER || new_role > ROLE_ADMIN) {
        errno = EINVAL;
        return AUTH_ERROR;
    }
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return AUTH_ERROR;
    if (lock_file(fd, F_WRLCK) < 0) { int e = errno; close(fd); errno = e; return AUTH_ERROR; }

    UserRecord user;
    off_t offset;
    int found = find_user(fd, username, &user, &offset, NULL);
    int result = AUTH_ERROR;
    if (found == 0) result = AUTH_INVALID_CREDENTIALS;
    else if (found == 1) {
        user.role = new_role;
        char row[USER_RECORD_SIZE];
        if (encode_user(&user, row) == 0 &&
            write_exact(fd, row, sizeof(row), offset) == 0 && fsync(fd) == 0)
            result = AUTH_OK;
    }
    return finish_operation(fd, 1, result);
}

int auth_change_password(const char *path, const char *username,
                         const char *current_password, const char *new_password)
{
    if (path == NULL || !valid_username(username) ||
        !valid_password(current_password) || !valid_password(new_password)) {
        errno = EINVAL;
        return AUTH_ERROR;
    }
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return AUTH_ERROR;
    if (lock_file(fd, F_WRLCK) < 0) { int e = errno; close(fd); errno = e; return AUTH_ERROR; }

    UserRecord user;
    off_t offset;
    int found = find_user(fd, username, &user, &offset, NULL);
    int result = AUTH_ERROR;
    if (found == 0) result = AUTH_INVALID_CREDENTIALS;
    else if (found == 1 && user.status == USER_INACTIVE) result = AUTH_INACTIVE_USER;
    else if (found == 1 && strcmp(user.password, current_password) != 0)
        result = AUTH_INVALID_CREDENTIALS;
    else if (found == 1) {
        snprintf(user.password, sizeof(user.password), "%s", new_password);
        char row[USER_RECORD_SIZE];
        if (encode_user(&user, row) == 0 &&
            write_exact(fd, row, sizeof(row), offset) == 0 && fsync(fd) == 0)
            result = AUTH_OK;
    }
    return finish_operation(fd, 1, result);
}
