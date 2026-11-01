#define _GNU_SOURCE
#define TRANSACTIONS_PATH "data/app_transactions.txt"
#define CUSTOMER_DETAILS_PATH "data/app_customer_details.txt"
#define EMPLOYEE_DETAILS_PATH "data/app_employee_details.txt"
#define FEEDBACK_PATH "data/app_feedback.txt"
#define LOANS_PATH "data/app_loans.txt"
#include "feedback.h"
#include "loans.h"
#include "transactions.h"
#include "auth.h"
#include "storage.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 8080
#define BUFFER_SIZE 8192
#define MAX_SESSIONS 128

#define USERS_PATH    "data/users.txt"
#define ACCOUNTS_PATH "data/app_accounts.txt"
#define JOURNAL_PATH  "data/app_journal.log"


/* One entry for each username with an active connection. */
typedef struct {
    int active;
    char username[USERNAME_MAX + 1];
} ActiveSession;

/*
 * All handler threads share this registry.
 * The binary semaphore makes checking and reserving a session one
 * atomic operation from the perspective of other threads.
 */
static ActiveSession sessions[MAX_SESSIONS];
static sem_t sessions_sem; /* Binary semaphore for the shared session table. */


/* ---------------------------------------------------------
 * TCP communication helpers
 * --------------------------------------------------------- */

/* Send the complete message unless an error occurs. */
static int send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(
            fd,
            data + sent,
            length - sent,
            MSG_NOSIGNAL
        );

        if (n < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0)
            return -1;

        sent += (size_t)n;
    }

    return 0;
}

/*
 * The application protocol uses one newline-terminated
 * request or response per line.
 *
 * Return:
 *   1  complete line received
 *   0  peer closed without sending data
 *  -1  error or incomplete line
 *  -2  line too long
 */
static int recv_line(int fd, char *buffer, size_t capacity)
{
    if (capacity < 2)
        return -1;

    size_t used = 0;

    while (used + 1 < capacity) {
        char ch;

        ssize_t n = recv(fd, &ch, 1, 0);

        if (n == 0) {
            if (used == 0)
                return 0;

            buffer[used] = '\0';
            return -1;
        }

        if (n < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (ch == '\n') {
            buffer[used] = '\0';
            return 1;
        }

        if (ch == '\r')
            continue;

        buffer[used++] = ch;
    }

    buffer[used] = '\0';
    return -2;
}


/* Split a delimited request into nonempty fields. */
static int split_fields(
    char *line,
    char *fields[],
    size_t max_fields
)
{
    if (line == NULL || line[0] == '\0' || max_fields == 0)
        return -1;

    size_t count = 1;
    fields[0] = line;

    for (char *p = line; *p != '\0'; p++) {
        if (*p != '|')
            continue;

        if (count == max_fields)
            return -1;

        *p = '\0';

        if (fields[count - 1][0] == '\0' || p[1] == '\0')
            return -1;

        fields[count++] = p + 1;
    }

    for (size_t i = 0; i < count; i++) {
        if (fields[i][0] == '\0')
            return -1;
    }

    return (int)count;
}


/* Parse a nonnegative decimal integer expressed in paise. */
static int parse_paise(const char *text, int64_t *amount)
{
    if (text == NULL || text[0] == '\0')
        return -1;

    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return -1;
    }

    errno = 0;
    char *end = NULL;

    unsigned long long value = strtoull(text, &end, 10);

    if (errno == ERANGE ||
        end == text ||
        *end != '\0' ||
        value > (unsigned long long)INT64_MAX) {
        return -1;
    }

    *amount = (int64_t)value;
    return 0;
}


/* Format and send one complete line in the client/server protocol. */
static int send_responsef(int fd, const char *format, ...)
{
    char response[BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(response, sizeof(response) - 1, format, args);
    va_end(args);

    if (n < 0 || (size_t)n >= sizeof(response) - 1)
        return -1;

    response[n++] = '\n';
    return send_all(fd, response, (size_t)n);
}

static int send_error(int fd, const char *message)
{
    return send_responsef(fd, "ERROR|%s", message);
}


static const char *storage_error_name(int rc)
{
    switch (rc) {
        case STORAGE_ALREADY_EXISTS:
            return "ALREADY_EXISTS";

        case STORAGE_NOT_FOUND:
            return "NOT_FOUND";

        case STORAGE_INSUFFICIENT_FUNDS:
            return "INSUFFICIENT_FUNDS";

        case STORAGE_INACTIVE_ACCOUNT:
            return "INACTIVE_ACCOUNT";

        case STORAGE_INVALID_AMOUNT:
            return "INVALID_AMOUNT";

        case STORAGE_ACCOUNT_NOT_EMPTY:
            return "ACCOUNT_BALANCE_MUST_BE_ZERO";

        case STORAGE_FATAL:
            return "FATAL_STORAGE_ERROR";

        default:
            return "STORAGE_ERROR";
    }
}


/* ---------------------------------------------------------
 * One-session-per-user management
 * --------------------------------------------------------- */

/*
 * Return:
 *   0  session reserved
 *   1  username already active
 *  -1  registry full or semaphore error
 *
 * The duplicate check and reservation happen under one binary semaphore.
 */
static int reserve_session(const char *username)
{
    while (sem_wait(&sessions_sem) < 0) {
        if (errno == EINTR)
            continue;
        return -1;
    }

    int free_slot = -1;
    int result = -1;

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active &&
            strcmp(sessions[i].username, username) == 0) {
            result = 1;
            goto done;
        }

        if (!sessions[i].active && free_slot == -1)
            free_slot = i;
    }

    if (free_slot >= 0) {
        sessions[free_slot].active = 1;

        snprintf(
            sessions[free_slot].username,
            sizeof(sessions[free_slot].username),
            "%s",
            username
        );

        result = 0;
    }

done:
    if (sem_post(&sessions_sem) < 0)
        return -1;
    return result;
}


/* Release the session after logout or disconnection. */
static void release_session(const char *username)
{
    while (sem_wait(&sessions_sem) < 0) {
        if (errno == EINTR)
            continue;
        return;
    }

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active &&
            strcmp(sessions[i].username, username) == 0) {

            sessions[i].active = 0;
            sessions[i].username[0] = '\0';
            break;
        }
    }

    if (sem_post(&sessions_sem) < 0)
        perror("sem_post");
}


/* Read an account and ensure the customer owns it. */
static int customer_owns_account(
    const char *username,
    const char *account_id,
    AccountRecord *account
)
{
    int rc = storage_get_account(
        ACCOUNTS_PATH,
        account_id,
        account
    );

    if (rc != STORAGE_OK)
        return rc;

    if (strcmp(account->customer_id, username) != 0)
        return STORAGE_ERROR;

    return STORAGE_OK;
}

/*
 * Record a successful banking operation in the transaction ledger.
 * A logging failure must not tell the customer that the balance
 * operation failed, because the balance may already have changed.
 */
static void log_transaction_or_warn(
    const char *type,
    const char *source_account,
    const char *destination_account,
    int64_t amount_paise,
    const char *actor)
{
    if (transaction_log(
            TRANSACTIONS_PATH,
            type,
            source_account,
            destination_account,
            amount_paise,
            actor) != 0) {
        fprintf(stderr,
                "WARNING: Banking operation succeeded, "
                "but transaction history could not be recorded.\n");
    }
}
/*
 * Send transaction history as one response line.
 * Newlines between ledger entries become semicolons so the
 * existing line-based client/server protocol continues to work.
 */
static int send_transaction_history(int fd, const char *account_id)
{
    char history[8192];

    if (transaction_history(
            TRANSACTIONS_PATH,
            account_id,
            history,
            sizeof(history)) != 0) {
        return send_error(fd, "HISTORY_READ_FAILED");
    }

    char response[BUFFER_SIZE];

    int n = snprintf(response, sizeof(response), "OK|HISTORY|");

    if (n < 0 || (size_t)n >= sizeof(response))
        return -1;

    size_t used = (size_t)n;
    int has_entry = 0;

    char *save = NULL;
    char *line = strtok_r(history, "\n", &save);

    while (line != NULL) {
        /* Skip the heading returned by transaction_history(). */
        if (strncmp(line,
                    "Transaction history for ",
                    strlen("Transaction history for ")) == 0) {
            line = strtok_r(NULL, "\n", &save);
            continue;
        }

        n = snprintf(
            response + used,
            sizeof(response) - used,
            "%s%s",
            has_entry ? " ; " : "",
            line
        );

        if (n < 0 || (size_t)n >= sizeof(response) - used)
            return send_error(fd, "HISTORY_RESPONSE_TOO_LARGE");

        used += (size_t)n;
        has_entry = 1;

        line = strtok_r(NULL, "\n", &save);
    }

    if (!has_entry) {
        n = snprintf(
            response + used,
            sizeof(response) - used,
            "No transactions found."
        );

        if (n < 0 || (size_t)n >= sizeof(response) - used)
            return -1;

        used += (size_t)n;
    }

    if (used + 1 >= sizeof(response))
        return -1;

    response[used++] = '\n';

    return send_all(fd, response, used);
}
/* Convert loan storage errors into protocol error messages. */
static const char *loan_error_name(int rc)
{
    switch (rc) {
        case LOAN_NOT_FOUND:
            return "LOAN_NOT_FOUND";
        case LOAN_INVALID_STATE:
            return "INVALID_LOAN_STATE";
        case LOAN_FORBIDDEN:
            return "FORBIDDEN";
        default:
            return "LOAN_STORAGE_ERROR";
    }
}

/* Send a loan listing using the existing one-line protocol. */
static int send_loan_list(
    int fd,
    const char *prefix,
    const char *list)
{
    char response[BUFFER_SIZE];

    int n = snprintf(
        response,
        sizeof(response),
        "%s%s\n",
        prefix,
        list
    );

    if (n < 0 || (size_t)n >= sizeof(response))
        return send_error(fd, "LOAN_RESPONSE_TOO_LARGE");

    return send_all(fd, response, (size_t)n);
}
/* Handle all balance-changing customer requests through one checked path. */
static int process_money_request(int fd, char **fields, const char *username,
                                 UserRole role)
{
    const char *operation = fields[0];
    int transfer = strcmp(operation, "TRANSFER") == 0;

    if (role != ROLE_CUSTOMER)
        return send_error(fd, "FORBIDDEN");

    AccountRecord account;
    int rc = customer_owns_account(username, fields[1], &account);
    if (rc != STORAGE_OK) {
        if (rc == STORAGE_ERROR)
            return send_error(fd, "FORBIDDEN_OR_STORAGE_ERROR");
        return send_error(fd, storage_error_name(rc));
    }

    int64_t amount;
    if (parse_paise(fields[transfer ? 3 : 2], &amount) < 0)
        return send_error(fd, "INVALID_AMOUNT");

    const char *source = "-";
    const char *destination = "-";
    if (strcmp(operation, "DEPOSIT") == 0) {
        rc = storage_deposit(ACCOUNTS_PATH, JOURNAL_PATH, fields[1], amount);
        destination = fields[1];
    } else if (strcmp(operation, "WITHDRAW") == 0) {
        rc = storage_withdraw(ACCOUNTS_PATH, JOURNAL_PATH, fields[1], amount);
        source = fields[1];
    } else {
        rc = storage_transfer(ACCOUNTS_PATH, JOURNAL_PATH,
                              fields[1], fields[2], amount);
        source = fields[1];
        destination = fields[2];
    }

    if (rc != STORAGE_OK)
        return send_error(fd, storage_error_name(rc));

    log_transaction_or_warn(operation, source, destination, amount, username);
    return send_responsef(fd, "OK|%s", operation);
}

/* ---------------------------------------------------------
 * Request authorization and execution
 * --------------------------------------------------------- */

/*
 * Only the authenticated username and server-derived role
 * are used for authorization; the client cannot select its role.
 */
static int process_request(
    int fd,
    char *request,
    const char *username,
    UserRole role,
    int *logout_requested
)
{
    char *fields[5];

    int count = split_fields(request, fields, 5);

    if (count < 0)
        return send_error(fd, "MALFORMED_REQUEST");

    *logout_requested = 0;
        /*
     * Reload the role for every request so role changes
     * made by an Administrator take effect immediately.
     */
    UserRole current_role;

    int auth_rc = auth_get_role(
        USERS_PATH,
        username,
        &current_role
    );

    if (auth_rc != AUTH_OK) {
        *logout_requested = 1;
        return send_error(fd, "SESSION_NO_LONGER_VALID");
    }

    role = current_role;
    /* LOGOUT */
    if (strcmp(fields[0], "LOGOUT") == 0 && count == 1) {
        if (send_responsef(fd, "OK|LOGOUT") < 0)
            return -1;

        *logout_requested = 1;
        return 0;
    }
        /* ACCOUNT_CLOSE|account_id — only employees may close accounts. */
    if (strcmp(fields[0], "ACCOUNT_CLOSE") == 0 && count == 2) {
        if (role != ROLE_EMPLOYEE)
            return send_error(fd, "FORBIDDEN");

        int rc = storage_close_account(ACCOUNTS_PATH, fields[1]);
        if (rc != STORAGE_OK)
            return send_error(fd, storage_error_name(rc));

        return send_responsef(fd, "OK|ACCOUNT_CLOSED|%s", fields[1]);
    }

    /*
     * CUSTOMER_CREATE|username|password|account_id|initial_paise
     * Only an employee may add a customer.
     */
    if (strcmp(fields[0], "CUSTOMER_CREATE") == 0 && count == 5) {
        if (role != ROLE_EMPLOYEE)
            return send_error(fd, "FORBIDDEN");

        /* Customer IDs in account records are limited to 16 characters. */
        if (strlen(fields[1]) > CUSTOMER_ID_MAX ||
            strlen(fields[3]) > ACCOUNT_ID_MAX ||
            strlen(fields[2]) == 0 ||
            strlen(fields[2]) > PASSWORD_MAX) {
            return send_error(fd, "INVALID_CUSTOMER_DETAILS");
        }

        int64_t initial_balance;

        if (parse_paise(fields[4], &initial_balance) < 0)
            return send_error(fd, "INVALID_AMOUNT");

        /* Avoid creating a login if the account ID already exists. */
        AccountRecord existing_account;

        int account_rc = storage_get_account(
            ACCOUNTS_PATH,
            fields[3],
            &existing_account
        );

        if (account_rc == STORAGE_OK)
            return send_error(fd, "ACCOUNT_ALREADY_EXISTS");

        if (account_rc != STORAGE_NOT_FOUND)
            return send_error(fd, "INVALID_ACCOUNT_ID");

        /* Create the customer's login with the CUSTOMER role. */
        int user_rc = auth_create_user(
            USERS_PATH,
            fields[1],
            ROLE_CUSTOMER,
            fields[2]
        );

        if (user_rc == AUTH_ALREADY_EXISTS)
            return send_error(fd, "USERNAME_ALREADY_EXISTS");

        if (user_rc != AUTH_OK)
            return send_error(fd, "CUSTOMER_CREATION_FAILED");

        /* Create the customer's bank account. */
        int rc = storage_create_account(
            ACCOUNTS_PATH,
            fields[3],
            fields[1],
            initial_balance
        );

        if (rc != STORAGE_OK) {
            /* Compensate for the login record if account creation fails. */
            if (auth_delete_user(USERS_PATH, fields[1]) != AUTH_OK) {
                fprintf(stderr,
                        "ERROR: account creation failed for %s and "
                        "its login record could not be rolled back.\n",
                        fields[1]);
                return send_error(
                    fd,
                    "ACCOUNT_CREATION_FAILED_LOGIN_CLEANUP_FAILED"
                );
            }
            return send_error(
                fd,
                "ACCOUNT_CREATION_FAILED_LOGIN_ROLLED_BACK"
            );
        }

        return send_responsef(fd, "OK|CUSTOMER_CREATED|%s|%s",
                              fields[1], fields[3]);
    }
    /*
 * EMPLOYEE_CREATE|username|password
 * Only an Administrator may create employee logins.
 */
    if (strcmp(fields[0], "EMPLOYEE_CREATE") == 0 && count == 3) {
        if (role != ROLE_ADMIN)
            return send_error(fd, "FORBIDDEN");

        if (strlen(fields[1]) == 0 ||
            strlen(fields[1]) > USERNAME_MAX ||
            strlen(fields[2]) == 0 ||
            strlen(fields[2]) > PASSWORD_MAX) {
            return send_error(fd, "INVALID_EMPLOYEE_DETAILS");
        }

        int user_rc = auth_create_user(
            USERS_PATH,
            fields[1],
            ROLE_EMPLOYEE,
            fields[2]
        );

        if (user_rc == AUTH_ALREADY_EXISTS)
            return send_error(fd, "USERNAME_ALREADY_EXISTS");

        if (user_rc != AUTH_OK)
            return send_error(fd, "EMPLOYEE_CREATION_FAILED");

        return send_responsef(fd, "OK|EMPLOYEE_CREATED|%s", fields[1]);
    }
    /*
    * CUSTOMER_DETAILS_SET|account_id|full_name|phone
    * Employees and administrators may modify customer details.
    */
    if (strcmp(fields[0], "CUSTOMER_DETAILS_SET") == 0 &&
        count == 4) {

        if (role != ROLE_EMPLOYEE && role != ROLE_ADMIN)
            return send_error(fd, "FORBIDDEN");

        AccountRecord account;

        int rc = storage_get_account(
            ACCOUNTS_PATH,
            fields[1],
            &account
        );

        if (rc != STORAGE_OK)
            return send_error(fd, storage_error_name(rc));

        rc = storage_set_customer_details(
            CUSTOMER_DETAILS_PATH,
            account.customer_id,
            fields[2],
            fields[3]
        );

        if (rc == STORAGE_INVALID_DETAILS)
            return send_error(fd, "INVALID_CUSTOMER_DETAILS");

        if (rc != STORAGE_OK)
            return send_error(fd, "CUSTOMER_DETAILS_UPDATE_FAILED");

        return send_responsef(fd, "OK|CUSTOMER_DETAILS_UPDATED");
    }

    /*
    * EMPLOYEE_DETAILS_SET|username|full_name|phone
    * Only an Administrator can modify employee details.
    */
    if (strcmp(fields[0], "EMPLOYEE_DETAILS_SET") == 0 &&
        count == 4) {

        if (role != ROLE_ADMIN)
            return send_error(fd, "FORBIDDEN");

        UserRole target_role;

        int user_rc = auth_get_role(
            USERS_PATH,
            fields[1],
            &target_role
        );

        if (user_rc != AUTH_OK)
            return send_error(fd, "EMPLOYEE_NOT_FOUND");

        if (target_role != ROLE_EMPLOYEE)
            return send_error(fd, "TARGET_NOT_EMPLOYEE");

        int rc = storage_set_employee_details(
            EMPLOYEE_DETAILS_PATH,
            fields[1],
            fields[2],
            fields[3]
        );

        if (rc == STORAGE_INVALID_DETAILS)
            return send_error(fd, "INVALID_EMPLOYEE_DETAILS");

        if (rc != STORAGE_OK)
            return send_error(fd, "EMPLOYEE_DETAILS_UPDATE_FAILED");

        return send_responsef(fd, "OK|EMPLOYEE_DETAILS_UPDATED");
    }
    /* HISTORY|account_id */
    if (strcmp(fields[0], "HISTORY") == 0 && count == 2) {

        /* Only customers and bank employees can view history here. */
        if (role != ROLE_CUSTOMER && role != ROLE_EMPLOYEE)
            return send_error(fd, "FORBIDDEN");

        AccountRecord account;

        int rc = storage_get_account(
            ACCOUNTS_PATH,
            fields[1],
            &account
        );

        if (rc != STORAGE_OK)
            return send_error(fd, storage_error_name(rc));

        /* Customers may only see accounts belonging to them. */
        if (role == ROLE_CUSTOMER &&
            strcmp(account.customer_id, username) != 0) {
            return send_error(fd, "FORBIDDEN");
        }

        return send_transaction_history(fd, fields[1]);
    }
        /* FEEDBACK_ADD|message — customer submits feedback. */
    if (strcmp(fields[0], "FEEDBACK_ADD") == 0 && count == 2) {
        if (role != ROLE_CUSTOMER)
            return send_error(fd, "FORBIDDEN");

        if (feedback_submit(FEEDBACK_PATH, username, fields[1]) != FEEDBACK_OK)
            return send_error(fd, "FEEDBACK_STORAGE_ERROR");

        return send_responsef(fd, "OK|FEEDBACK_ADDED");
    }

    /* FEEDBACK_LIST — manager reviews customer feedback. */
    if (strcmp(fields[0], "FEEDBACK_LIST") == 0 && count == 1) {
        if (role != ROLE_MANAGER)
            return send_error(fd, "FORBIDDEN");

        char list[BUFFER_SIZE - 32];

        int rc = feedback_list(
            FEEDBACK_PATH,
            list,
            sizeof(list)
        );

        if (rc != FEEDBACK_OK)
            return send_error(fd, "FEEDBACK_READ_FAILED");

        int sent = send_responsef(fd, "OK|FEEDBACK_LIST|%s", list);
        return sent < 0 ? send_error(fd, "FEEDBACK_RESPONSE_TOO_LARGE") : sent;
    }
        /* -----------------------------------------------------
     * LOAN_APPLY|account_id|amount_paise|purpose
     * ----------------------------------------------------- */
    if (strcmp(fields[0], "LOAN_APPLY") == 0 && count == 4) {
        if (role != ROLE_CUSTOMER)
            return send_error(fd, "FORBIDDEN");

        AccountRecord account;

        int rc = customer_owns_account(
            username, fields[1], &account
        );

        if (rc != STORAGE_OK) {
            if (rc == STORAGE_ERROR)
                return send_error(fd, "FORBIDDEN_OR_STORAGE_ERROR");

            return send_error(fd, storage_error_name(rc));
        }

        if (account.status != 'A')
            return send_error(fd, "INACTIVE_ACCOUNT");

        int64_t amount;

        if (parse_paise(fields[2], &amount) < 0 || amount <= 0)
            return send_error(fd, "INVALID_AMOUNT");

        char loan_id[16];

        rc = loans_apply(
            LOANS_PATH,
            username,
            fields[1],
            amount,
            fields[3],
            loan_id,
            sizeof(loan_id)
        );

        if (rc != LOAN_OK)
            return send_error(fd, loan_error_name(rc));

        return send_responsef(fd, "OK|LOAN_APPLIED|%s", loan_id);
    }

    /* LOAN_MY — customer's own applications. */
    if (strcmp(fields[0], "LOAN_MY") == 0 && count == 1) {
        if (role != ROLE_CUSTOMER)
            return send_error(fd, "FORBIDDEN");

        char list[BUFFER_SIZE - 32];

        int rc = loans_list_customer(
            LOANS_PATH, username, list, sizeof(list)
        );

        if (rc != LOAN_OK)
            return send_error(fd, loan_error_name(rc));

        return send_loan_list(fd, "OK|LOANS|", list);
    }

    /* LOAN_PENDING — pending applications for the manager. */
    if (strcmp(fields[0], "LOAN_PENDING") == 0 && count == 1) {
        if (role != ROLE_MANAGER)
            return send_error(fd, "FORBIDDEN");

        char list[BUFFER_SIZE - 32];

        int rc = loans_list_pending(
            LOANS_PATH, list, sizeof(list)
        );

        if (rc != LOAN_OK)
            return send_error(fd, loan_error_name(rc));

        return send_loan_list(fd, "OK|LOANS|", list);
    }

    /* LOAN_ASSIGN|loan_id|employee_username */
    if (strcmp(fields[0], "LOAN_ASSIGN") == 0 && count == 3) {
        if (role != ROLE_MANAGER)
            return send_error(fd, "FORBIDDEN");

        UserRole employee_role;

        int user_rc = auth_get_role(
            USERS_PATH, fields[2], &employee_role
        );

        if (user_rc != AUTH_OK || employee_role != ROLE_EMPLOYEE)
            return send_error(fd, "EMPLOYEE_NOT_FOUND");

        int rc = loans_assign(
            LOANS_PATH, fields[1], fields[2]
        );

        if (rc != LOAN_OK)
            return send_error(fd, loan_error_name(rc));

        return send_responsef(fd, "OK|LOAN_ASSIGNED|%s|%s",
                              fields[1], fields[2]);
    }

    /* LOAN_ASSIGNED — applications assigned to this employee. */
    if (strcmp(fields[0], "LOAN_ASSIGNED") == 0 && count == 1) {
        if (role != ROLE_EMPLOYEE)
            return send_error(fd, "FORBIDDEN");

        char list[BUFFER_SIZE - 32];

        int rc = loans_list_assigned(
            LOANS_PATH, username, list, sizeof(list)
        );

        if (rc != LOAN_OK)
            return send_error(fd, loan_error_name(rc));

        return send_loan_list(fd, "OK|LOANS|", list);
    }

    /* LOAN_DECIDE|loan_id|APPROVED or REJECTED */
    if (strcmp(fields[0], "LOAN_DECIDE") == 0 && count == 3) {
        if (role != ROLE_EMPLOYEE)
            return send_error(fd, "FORBIDDEN");

        char approved_account[ACCOUNT_ID_MAX + 1];
        int64_t approved_amount = 0;

        int approving = strcmp(fields[2], "APPROVED") == 0;

        int rc;

        if (approving) {
            rc = loans_decide_with_details(
                LOANS_PATH,
                fields[1],
                username,
                fields[2],
                approved_account,
                sizeof(approved_account),
                &approved_amount
            );
        } else {
            rc = loans_decide(
                LOANS_PATH, fields[1], username, fields[2]
            );
        }

        if (rc != LOAN_OK)
            return send_error(fd, loan_error_name(rc));

        /* Credit the customer's account only after approval. */
        if (approving) {
            rc = storage_deposit(
                ACCOUNTS_PATH,
                JOURNAL_PATH,
                approved_account,
                approved_amount
            );

            if (rc != STORAGE_OK) {
                fprintf(stderr,
                        "WARNING: Loan %s was approved but its amount "
                        "could not be credited.\n", fields[1]);

                return send_error(fd, "LOAN_APPROVED_CREDIT_FAILED");
            }

            /* Record the loan credit in the customer's transaction history. */
            log_transaction_or_warn(
                "DEPOSIT",
                "-",
                approved_account,
                approved_amount,
                username
            );
        }

        return send_responsef(fd, "OK|LOAN_DECISION|%s|%s",
                            fields[1], fields[2]);
    }
        /* ACCOUNT_STATUS|account_id|A or I */
    if (strcmp(fields[0], "ACCOUNT_STATUS") == 0 && count == 3) {
        if (role != ROLE_MANAGER)
            return send_error(fd, "FORBIDDEN");

        if (strcmp(fields[2], "A") != 0 &&
            strcmp(fields[2], "I") != 0) {
            return send_error(fd, "INVALID_STATUS");
        }

        int rc = storage_set_account_status(
            ACCOUNTS_PATH,
            fields[1],
            fields[2][0]
        );

        if (rc != STORAGE_OK)
            return send_error(fd, storage_error_name(rc));

        return send_responsef(fd, "OK|ACCOUNT_STATUS|%s|%s", fields[1],
                              fields[2][0] == 'A' ? "ACTIVE" : "INACTIVE");
    }

    /* BALANCE|account_id */
    if (strcmp(fields[0], "BALANCE") == 0 && count == 2) {
        AccountRecord account;

        int rc = storage_get_account(
            ACCOUNTS_PATH,
            fields[1],
            &account
        );

        if (rc != STORAGE_OK)
            return send_error(fd, storage_error_name(rc));

        /*
         * Customers can only inspect their own accounts.
         * Other authenticated roles may inspect balances in
         * this development-stage implementation.
         */
        if (role == ROLE_CUSTOMER &&
            strcmp(account.customer_id, username) != 0) {
            return send_error(fd, "FORBIDDEN");
        }

        return send_responsef(fd, "OK|BALANCE|%s|%" PRId64 "|%c",
                              account.account_id, account.balance_paise,
                              account.status);
    }

    /*
     * CREATE_ACCOUNT|account_id|customer_username|initial_paise
     *
     * Only an Administrator may create an account through this
     * temporary command. The customer must already exist.
     */
    if (strcmp(fields[0], "CREATE_ACCOUNT") == 0 && count == 4) {
        if (role != ROLE_ADMIN)
            return send_error(fd, "FORBIDDEN");

        UserRole customer_role;

        int user_rc = auth_get_role(
            USERS_PATH,
            fields[2],
            &customer_role
        );

        if (user_rc != AUTH_OK ||
            customer_role != ROLE_CUSTOMER) {
            return send_error(fd, "CUSTOMER_NOT_FOUND");
        }

        int64_t initial_balance;

        if (parse_paise(fields[3], &initial_balance) < 0)
            return send_error(fd, "INVALID_AMOUNT");

        int rc = storage_create_account(
            ACCOUNTS_PATH,
            fields[1],
            fields[2],
            initial_balance
        );

        if (rc != STORAGE_OK)
            return send_error(fd, storage_error_name(rc));

        return send_responsef(fd, "OK|ACCOUNT_CREATED|%s", fields[1]);
    }

    /* Balance-changing requests share ownership, amount and logging checks. */
    if ((strcmp(fields[0], "DEPOSIT") == 0 ||
         strcmp(fields[0], "WITHDRAW") == 0) && count == 3)
        return process_money_request(fd, fields, username, role);

    if (strcmp(fields[0], "TRANSFER") == 0 && count == 4)
        return process_money_request(fd, fields, username, role);

    /*
    * USER_ROLE_SET|username|new_role
    * Only an Administrator can change user roles.
    */
    if (strcmp(fields[0], "USER_ROLE_SET") == 0 &&
        count == 3) {

        if (role != ROLE_ADMIN)
            return send_error(fd, "FORBIDDEN");

        /* Prevent the Administrator from changing their own role. */
        if (strcmp(username, fields[1]) == 0)
            return send_error(fd, "CANNOT_CHANGE_OWN_ROLE");

        UserRole new_role = auth_parse_role(fields[2]);

        if (new_role == ROLE_INVALID)
            return send_error(fd, "INVALID_ROLE");

        int rc = auth_set_role(
            USERS_PATH,
            fields[1],
            new_role
        );

        if (rc == AUTH_INVALID_CREDENTIALS)
            return send_error(fd, "USER_NOT_FOUND");

        if (rc != AUTH_OK)
            return send_error(fd, "ROLE_UPDATE_FAILED");

        return send_responsef(fd, "OK|USER_ROLE_UPDATED|%s|%s",
                              fields[1], auth_role_name(new_role));
    }
        /*
     * PASSWORD_CHANGE|current_password|new_password
     * Any authenticated role may change its own password.
     */
    if (strcmp(fields[0], "PASSWORD_CHANGE") == 0 &&
        count == 3) {

        if (strlen(fields[2]) < 8 ||
            strlen(fields[2]) > PASSWORD_MAX) {
            return send_error(fd, "INVALID_NEW_PASSWORD");
        }

        int rc = auth_change_password(
            USERS_PATH,
            username,
            fields[1],
            fields[2]
        );

        if (rc == AUTH_INVALID_CREDENTIALS)
            return send_error(fd, "WRONG_CURRENT_PASSWORD");

        if (rc == AUTH_INACTIVE_USER)
            return send_error(fd, "USER_INACTIVE");

        if (rc != AUTH_OK)
            return send_error(fd, "PASSWORD_CHANGE_FAILED");

        return send_responsef(fd, "OK|PASSWORD_CHANGED");
    }
    return send_error(fd, "UNKNOWN_COMMAND_OR_WRONG_ARGUMENTS");
}


/* ---------------------------------------------------------
 * Client-handler thread
 * --------------------------------------------------------- */

static void *handle_client(void *arg)
{
    int client_fd = *(int *)arg;
    free(arg);

    char request[BUFFER_SIZE];
    char username[USERNAME_MAX + 1] = "";
    UserRole role = ROLE_INVALID;
    int session_reserved = 0;

    /*
     * The first message on a new connection must be LOGIN.
     * No banking command is accepted before authentication.
     */
    int status = recv_line(
        client_fd, request, sizeof(request)
    );

    if (status != 1)
        goto cleanup;

    char *fields[4];

    int count = split_fields(request, fields, 4);

    if (count != 3 || strcmp(fields[0], "LOGIN") != 0) {
        send_error(client_fd, "LOGIN_REQUIRED");
        goto cleanup;
    }

    /* Copy the username before clearing the request buffer. */
    if (strlen(fields[1]) > USERNAME_MAX) {
        send_error(client_fd, "LOGIN_FAILED");
        goto cleanup;
    }

    snprintf(username, sizeof(username), "%s", fields[1]);

    /*
     * Verify the credentials. Do not print or log the password.
     */
    int rc = auth_verify(
        USERS_PATH,
        username,
        fields[2],
        &role
    );

    memset(request, 0, sizeof(request));

    if (rc != AUTH_OK) {
        send_error(client_fd, "LOGIN_FAILED");
        goto cleanup;
    }

    /*
     * Reserve the username only after correct authentication.
     * The registry mutex prevents duplicate concurrent sessions.
     */
    rc = reserve_session(username);

    if (rc == 1) {
        send_error(client_fd, "SESSION_ALREADY_ACTIVE");
        goto cleanup;
    }

    if (rc != 0) {
        send_error(client_fd, "SESSION_REGISTRY_FULL");
        goto cleanup;
    }

    session_reserved = 1;

    if (send_responsef(client_fd, "OK|LOGIN|%s", auth_role_name(role)) < 0)
        goto cleanup;

    printf(
        "[Thread %lu] User %s logged in as %s.\n",
        (unsigned long)pthread_self(),
        username,
        auth_role_name(role)
    );

    /*
     * Persistent session: process successive commands until
     * LOGOUT, disconnection, or a communication error.
     */
    for (;;) {
        status = recv_line(
            client_fd, request, sizeof(request)
        );

        if (status != 1)
            break;

        int logout_requested = 0;

        if (process_request(
                client_fd,
                request,
                username,
                role,
                &logout_requested
            ) < 0) {
            break;
        }

        if (logout_requested)
            break;
    }

cleanup:
    /*
     * Every exit path must release the username reservation.
     * Otherwise the user would remain unable to log in again.
     */
    if (session_reserved) {
        release_session(username);

        printf(
            "[Thread %lu] User %s session closed.\n",
            (unsigned long)pthread_self(),
            username
        );
    }

    close(client_fd);
    return NULL;
}


/* ---------------------------------------------------------
 * Server entry point
 * --------------------------------------------------------- */

int main(void)
{
    /*
     * Recover account transactions before accepting clients.
     * Never allow clients to operate on accounts before recovery.
     */
    if (storage_init(ACCOUNTS_PATH) != STORAGE_OK) {
        perror("storage_init");
        return EXIT_FAILURE;
    }

    if (storage_recover(
            ACCOUNTS_PATH,
            JOURNAL_PATH
        ) != STORAGE_OK) {
        perror("storage_recover");
        return EXIT_FAILURE;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int reuse = 1;

    if (setsockopt(
            server_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)
        ) < 0) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    /* Development server: only accept local-machine connections. */
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(
            server_fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)
        ) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    pthread_attr_t attributes;

    int rc = pthread_attr_init(&attributes);

    if (rc != 0) {
        fprintf(stderr, "pthread_attr_init: %s\n", strerror(rc));
        close(server_fd);
        return EXIT_FAILURE;
    }

    rc = pthread_attr_setdetachstate(
        &attributes,
        PTHREAD_CREATE_DETACHED
    );

    if (rc != 0) {
        fprintf(
            stderr,
            "pthread_attr_setdetachstate: %s\n",
            strerror(rc)
        );

        pthread_attr_destroy(&attributes);
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (sem_init(&sessions_sem, 0, 1) < 0) {
        perror("sem_init");
        pthread_attr_destroy(&attributes);
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf(
        "Banking server listening on 127.0.0.1:%d\n",
        PORT
    );
    fflush(stdout);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_addr,
            &client_len
        );

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;

            perror("accept");
            break;
        }

        int *argument = malloc(sizeof(*argument));

        if (argument == NULL) {
            perror("malloc");
            close(client_fd);
            continue;
        }

        *argument = client_fd;

        pthread_t thread_id;

        rc = pthread_create(
            &thread_id,
            &attributes,
            handle_client,
            argument
        );

        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            close(client_fd);
            free(argument);
        }
    }

    pthread_attr_destroy(&attributes);
    close(server_fd);

    return EXIT_FAILURE;
}