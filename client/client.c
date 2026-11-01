#define _GNU_SOURCE
#include "../include/storage.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 8080
#define BUFFER_SIZE 8192
#define USERNAME_MAX 31
#define PASSWORD_MAX 128

/* ANSI colors make the terminal menu easier to read. */
#define RESET   "\033[0m"
#define BLUE    "\033[1;34m"
#define CYAN    "\033[1;36m"
#define GREEN   "\033[1;32m"
#define RED     "\033[1;31m"
#define YELLOW  "\033[1;33m"

/* Send all bytes, handling partial sends. */
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

/* Receive one newline-terminated response. */
static int recv_line(int fd, char *buffer, size_t capacity)
{
    if (capacity < 2)
        return -1;

    size_t used = 0;

    while (used + 1 < capacity) {
        char ch;

        ssize_t n = recv(fd, &ch, 1, 0);

        if (n == 0)
            return used == 0 ? 0 : -1;

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

/*
 * Read one line of terminal input.
 *
 * If the input doesn't fit, discard the rest of that line
 * instead of accidentally treating it as another menu choice.
 */
static int read_input(
    const char *prompt,
    char *buffer,
    size_t capacity
)
{
    printf("%s", prompt);
    fflush(stdout);

    if (fgets(buffer, (int)capacity, stdin) == NULL)
        return -1;

    char *newline = strchr(buffer, '\n');

    if (newline == NULL) {
        int ch;

        while ((ch = getchar()) != '\n' && ch != EOF) {
            /* Discard excess characters. */
        }

        return -1;
    }

    *newline = '\0';

    /* Also support Windows-style CRLF input. */
    size_t len = strlen(buffer);

    if (len > 0 && buffer[len - 1] == '\r')
        buffer[len - 1] = '\0';

    return 0;
}

/* Read a required protocol field (pipe is the field separator). */
static int prompt_field(const char *prompt, char *buffer, size_t capacity,
                        const char *error_message)
{
    if (read_input(prompt, buffer, capacity) < 0 ||
        buffer[0] == '\0' || strchr(buffer, '|') != NULL) {
        printf(RED "%s\n" RESET, error_message);
        return -1;
    }
    return 0;
}

/* Passwords are hidden while typing; this demo stores them as plain text. */
static int prompt_password(const char *prompt, char *password, size_t capacity,
                           size_t minimum_length, const char *error_message)
{
    char *typed = getpass(prompt);
    if (typed == NULL || strlen(typed) < minimum_length ||
        strlen(typed) >= capacity || strchr(typed, '|') != NULL) {
        printf(RED "%s\n" RESET, error_message);
        return -1;
    }
    snprintf(password, capacity, "%s", typed);
    return 0;
}

/*
 * Send one request and receive its response.
 *
 * The TCP connection stays open; this function does not close it.
 */
static int request_server(
    int fd,
    const char *command,
    char *response,
    size_t response_capacity
)
{
    if (send_all(fd, command, strlen(command)) < 0)
        return -1;

    const char newline = '\n';

    if (send_all(fd, &newline, 1) < 0)
        return -1;

    int status = recv_line(
        fd,
        response,
        response_capacity
    );

    return status == 1 ? 0 : -1;
}

/*
 * Convert an amount such as "125.50" into paise (12550).
 *
 * Floating-point arithmetic is deliberately avoided because
 * monetary values should be represented exactly.
 */
static int parse_rupees(
    const char *text,
    int64_t *paise_out
)
{
    if (text == NULL || paise_out == NULL || text[0] == '\0')
        return -1;

    const char *dot = strchr(text, '.');

    /* A second decimal point is invalid. */
    if (dot != NULL && strchr(dot + 1, '.') != NULL)
        return -1;

    size_t whole_length = dot == NULL ?
                          strlen(text) :
                          (size_t)(dot - text);

    if (whole_length == 0 || whole_length >= 32)
        return -1;

    /* Validate the whole-rupee portion before parsing it. */
    for (size_t i = 0; i < whole_length; i++) {
        if (text[i] < '0' || text[i] > '9')
            return -1;
    }

    char whole_text[32];

    memcpy(whole_text, text, whole_length);
    whole_text[whole_length] = '\0';

    errno = 0;
    char *end = NULL;

    unsigned long long whole = strtoull(
        whole_text, &end, 10
    );

    if (errno == ERANGE ||
        end == whole_text ||
        *end != '\0') {
        return -1;
    }

    unsigned int fraction = 0;

    if (dot != NULL) {
        size_t fraction_length = strlen(dot + 1);

        if (fraction_length == 0 || fraction_length > 2)
            return -1;

        for (size_t i = 0; i < fraction_length; i++) {
            char c = dot[1 + i];

            if (c < '0' || c > '9')
                return -1;

            fraction = fraction * 10 + (unsigned int)(c - '0');
        }

        /* Convert a single fractional digit into tens of paise. */
        if (fraction_length == 1)
            fraction *= 10;
    }

    if (whole > ((unsigned long long)INT64_MAX - fraction) / 100)
        return -1;

    *paise_out = (int64_t)(whole * 100 + fraction);

    return 0;
}

/* Ask for an amount in rupees and convert it to paise. */
static int prompt_amount(int64_t *amount_paise)
{
    char input[64];

    if (read_input(
            "Amount in rupees (example 125.50): ",
            input,
            sizeof(input)
        ) < 0) {
        printf(RED "Invalid input length.\n" RESET);
        return -1;
    }

    if (parse_rupees(input, amount_paise) < 0) {
        printf(RED "Invalid monetary amount.\n" RESET);
        return -1;
    }

    return 0;
}

static int prompt_positive_amount(int64_t *amount_paise)
{
    if (prompt_amount(amount_paise) < 0)
        return -1;
    if (*amount_paise <= 0) {
        printf(RED "Amount must be greater than zero.\n" RESET);
        return -1;
    }
    return 0;
}

static int prompt_phone(char *phone, size_t capacity)
{
    if (prompt_field("Updated phone number (10-15 digits): ", phone,
                     capacity, "Invalid phone number.") < 0)
        return -1;

    size_t length = strlen(phone);
    if (length < 10 || length > 15) {
        printf(RED "Phone must contain 10-15 digits.\n" RESET);
        return -1;
    }
    for (size_t i = 0; i < length; ++i) {
        if (phone[i] < '0' || phone[i] > '9') {
            printf(RED "Phone must contain 10-15 digits.\n" RESET);
            return -1;
        }
    }
    return 0;
}

/* Print a ledger-style response as one item per line. */
static void display_list(const char *title, const char *items)
{
    char text[BUFFER_SIZE];
    snprintf(text, sizeof(text), "%s", items);

    for (char *p = text; (p = strstr(p, " ; ")) != NULL; ++p) {
        *p = '\n';
        memmove(p + 1, p + 3, strlen(p + 3) + 1);
    }

    printf("\n" CYAN "========== %s ==========" RESET "\n%s\n",
           title, text);
}

/* Display common responses compactly; the server remains the source of truth. */
static void display_response(const char *response)
{
    if (strncmp(response, "ERROR|", 6) == 0) {
        char message[BUFFER_SIZE];
        snprintf(message, sizeof(message), "%s", response + 6);
        for (char *p = message; *p; ++p)
            if (*p == '_') *p = ' ';
        printf(RED "\nOperation failed: %s\n" RESET, message);
        return;
    }

    char account_id[17];
    int64_t paise;
    char status;
    if (sscanf(response, "OK|BALANCE|%16[^|]|%" SCNd64 "|%c",
               account_id, &paise, &status) == 3) {
        const char *label = status == 'A' ? "ACTIVE" :
                            status == 'I' ? "INACTIVE" : "CLOSED";
        printf("\n" CYAN "========== ACCOUNT BALANCE ==========" RESET
               "\nAccount: %s\n" GREEN "Balance: Rs. %" PRId64 ".%02" PRId64
               RESET "\nStatus: %s\n", account_id, paise / 100,
               paise % 100, label);
        return;
    }

    static const struct {
        const char *prefix;
        const char *title;
    } lists[] = {
        {"OK|HISTORY|", "TRANSACTION HISTORY"},
        {"OK|LOANS|", "LOAN APPLICATIONS"},
        {"OK|FEEDBACK_LIST|", "CUSTOMER FEEDBACK"}
    };
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); ++i) {
        size_t length = strlen(lists[i].prefix);
        if (strncmp(response, lists[i].prefix, length) == 0) {
            display_list(lists[i].title, response + length);
            return;
        }
    }

    if (strncmp(response, "OK|ACCOUNT_STATUS|", 18) == 0) {
        char account_status[16];
        if (sscanf(response, "OK|ACCOUNT_STATUS|%16[^|]|%15s",
                   account_id, account_status) == 2) {
            const char *label =
                (strcmp(account_status, "A") == 0 ||
                 strcmp(account_status, "ACTIVE") == 0) ? "ACTIVE" :
                (strcmp(account_status, "I") == 0 ||
                 strcmp(account_status, "INACTIVE") == 0) ? "INACTIVE" :
                "CLOSED";
            printf(GREEN "Account %s is now %s.\n" RESET, account_id, label);
        }
        return;
    }

    if (strncmp(response, "OK|", 3) == 0) {
        char message[BUFFER_SIZE];
        snprintf(message, sizeof(message), "%s", response + 3);
        char *save = NULL;
        char *part = strtok_r(message, "|", &save);
        if (part == NULL) return;

        for (char *p = part; *p; ++p)
            if (*p == '_') *p = ' ';

        if (strcmp(part, "DEPOSIT") == 0) {
            printf(GREEN "Deposit completed successfully.\n" RESET);
            return;
        }
        if (strcmp(part, "WITHDRAW") == 0) {
            printf(GREEN "Withdrawal completed successfully.\n" RESET);
            return;
        }
        if (strcmp(part, "TRANSFER") == 0) {
            printf(GREEN "Fund transfer completed successfully.\n" RESET);
            return;
        }

        printf(GREEN "Success: %s" RESET, part);
        while ((part = strtok_r(NULL, "|", &save)) != NULL)
            printf(" | %s", part);
        putchar('\n');
        return;
    }

    printf("%s\n", response);
}

/* Show only the menu appropriate for the authenticated role. */
static void display_menu(const char *username, const char *role)
{
    static const char *const customer_menu =
        "  1. View account balance\n  2. Deposit money\n"
        "  3. Withdraw money\n  4. Transfer funds\n"
        "  5. View transaction history\n  6. Apply for a loan\n"
        "  7. View my loan applications\n  8. Submit feedback\n"
        "  9. Change password\n 10. Logout\n  0. Exit application\n";
    static const char *const employee_menu =
        "  1. View customer account balance\n  2. View customer passbook\n"
        "  3. View assigned loan applications\n  4. Decide on a loan application\n"
        "  5. Add new customer\n  6. Modify customer details\n"
        "  7. Close customer account\n  8. Change password\n"
        "  9. Logout\n  0. Exit application\n";
    static const char *const manager_menu =
        "  1. View account balance\n  2. View pending loan applications\n"
        "  3. Assign a loan application\n  4. Review customer feedback\n"
        "  5. Activate/Deactivate account\n  6. Change password\n"
        "  7. Logout\n  0. Exit application\n";
    static const char *const admin_menu =
        "  1. View account balance\n  2. Create account for an existing customer\n"
        "  3. Modify customer details\n  4. Modify employee details\n"
        "  5. Add new bank employee\n  6. Manage user roles\n"
        "  7. Change password\n  8. Logout\n  0. Exit application\n";

    const char *menu = strcmp(role, "CUSTOMER") == 0 ? customer_menu :
                       strcmp(role, "EMPLOYEE") == 0 ? employee_menu :
                       strcmp(role, "MANAGER") == 0 ? manager_menu : admin_menu;

    printf("\n" BLUE
           "============================================\n"
           "              SATYAM BANK\n"
           "        BANKING MANAGEMENT SYSTEM\n"
           "============================================\n" RESET
           " User: %s\n Role: %s\n" CYAN
           "+------------------------------------------+\n" RESET
           "%s" CYAN "+------------------------------------------+\n" RESET,
           username, role, menu);
}

/* Prompt for an account ID. */
static int prompt_account_id(const char *prompt, char *account_id,
                             size_t capacity)
{
    return prompt_field(prompt, account_id, capacity, "Invalid account ID.");
}

/* Establish a TCP connection to the local banking server. */
static int connect_to_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(
            fd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)
        ) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}
/* Convert role names into readable labels. */
static const char *role_label(const char *role)
{
    if (strcmp(role, "ADMIN") == 0)
        return "Administrator";

    if (strcmp(role, "MANAGER") == 0)
        return "Manager";

    if (strcmp(role, "EMPLOYEE") == 0)
        return "Bank Employee";

    if (strcmp(role, "CUSTOMER") == 0)
        return "Customer";

    return "Unknown";
}

/* Display the login-type menu and return the selected role. */
static const char *select_login_role(void)
{
    char choice[16];

    for (;;) {
        printf(BLUE
               "\n============================================\n"
               "              SATYAM BANK\n"
               "============================================\n"
               RESET);

        printf("\nSelect Login Type\n\n");
        printf("1. Administrator\n");
        printf("2. Manager\n");
        printf("3. Bank Employee\n");
        printf("4. Customer\n");
        printf("0. Exit\n\n");

        if (read_input("Enter your choice: ",
                       choice, sizeof(choice)) < 0) {
            printf(RED "Invalid input.\n" RESET);
            continue;
        }

        if (strcmp(choice, "1") == 0)
            return "ADMIN";

        if (strcmp(choice, "2") == 0)
            return "MANAGER";

        if (strcmp(choice, "3") == 0)
            return "EMPLOYEE";

        if (strcmp(choice, "4") == 0)
            return "CUSTOMER";

        if (strcmp(choice, "0") == 0)
            return NULL;

        printf(RED "Invalid choice. Try again.\n" RESET);
    }
}
/* Login and receive the server-verified role. */
static int perform_login(
    int fd,
    char *username,
    size_t username_capacity,
    char *role,
    size_t role_capacity,
    const char *expected_role
)
{
    char password_copy[PASSWORD_MAX + 1] = {0};
    char request[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    char prompt[80];

    snprintf(prompt, sizeof(prompt), "%s username: ",
            role_label(expected_role));

    if (read_input(prompt, username, username_capacity) < 0) {
        printf(RED "Invalid username input.\n" RESET);
        return -1;
    }

    if (strchr(username, '|') != NULL || username[0] == '\0') {
        printf(RED "Invalid username.\n" RESET);
        return -1;
    }

    /* getpass() hides password characters while typing. */
    snprintf(prompt, sizeof(prompt), "%s password: ",
            role_label(expected_role));

    char *entered = getpass(prompt);

    if (entered == NULL) {
        printf(RED "Could not read password.\n" RESET);
        return -1;
    }

    size_t password_length = strnlen(
        entered, PASSWORD_MAX + 1
    );

    if (password_length == 0 ||
        password_length > PASSWORD_MAX ||
        strchr(entered, '|') != NULL) {
        printf(RED "Invalid password length or character.\n" RESET);
        return -1;
    }

    memcpy(password_copy, entered, password_length + 1);

    /*
     * LOGIN is the first request on this TCP connection.
     * The server derives the role from the trusted user file.
     */
    int n = snprintf(
        request,
        sizeof(request),
        "LOGIN|%s|%s",
        username,
        password_copy
    );

    if (n < 0 || (size_t)n >= sizeof(request)) {
        printf(RED "Login request too long.\n" RESET);
        return -1;
    }

    int rc = request_server(
        fd, request, response, sizeof(response)
    );

    if (rc < 0) {
        printf(RED "No response from server.\n" RESET);
        return -1;
    }

    if (strncmp(response, "OK|LOGIN|", 9) != 0) {
        display_response(response);
        return -1;
    }

    snprintf(role, role_capacity, "%s", response + 9);

    /* Make sure this account belongs to the selected role. */
    if (strcmp(role, expected_role) != 0) {
        printf(RED
            "This account belongs to %s, not %s.\n"
            RESET,
            role_label(role),
            role_label(expected_role));

        return -1;
    }

    printf(GREEN "\nLogin successful.\n" RESET);

    return 0;
}

/* Build a protocol request and reject truncation instead of sending it. */
static void set_command(char *buffer, size_t capacity, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buffer, capacity, format, args);
    va_end(args);

    if (n < 0 || (size_t)n >= capacity) {
        if (capacity > 0)
            buffer[0] = '\0';
        printf(RED "Request is too long.\n" RESET);
    }
}

/* Run the menu until the user logs out or the connection fails. */
static int run_menu(int fd, const char *username, const char *role)
{
    char choice_text[32];
    char command[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    for (;;) {
        command[0] = '\0';
        display_menu(username, role);

        if (read_input("Enter your choice: ",
                       choice_text,
                       sizeof(choice_text)) < 0) {
            printf(RED "Invalid menu input.\n" RESET);
            continue;
        }

        /* Logout ends the session and returns to the login prompt. */
        const char *logout_choice =
            strcmp(role, "CUSTOMER") == 0 ? "10" :
            strcmp(role, "MANAGER") == 0 ? "7" :
            strcmp(role, "EMPLOYEE") == 0 ? "9" :
            strcmp(role, "ADMIN") == 0 ? "8" : "-1";

        if (strcmp(choice_text, logout_choice) == 0 ||
            strcmp(choice_text, "0") == 0) {
            if (request_server(
                    fd, "LOGOUT", response, sizeof(response)
                ) < 0) {
                printf(RED "Could not contact server.\n" RESET);
                return -1;
            }

            if (strcmp(choice_text, "0") == 0) {
                printf(GREEN "Thank you for using SATYAM BANK.\n" RESET);
                return 0; /* Exit the client application. */
            }

            printf(GREEN "You have logged out.\n" RESET);
            return 1; /* Main reconnects and permits another login. */
        }

        /* View a balance: currently enabled for every role. */
        if (strcmp(choice_text, "1") == 0) {
            char account_id[64];

            if (prompt_account_id(
                    "Account ID: ",
                    account_id,
                    sizeof(account_id)
                ) < 0) {
                continue;
            }

            set_command(command, sizeof(command), "BALANCE|%s", account_id);
        }

        /* Customer deposit and withdrawal share the same input flow. */
        else if (strcmp(role, "CUSTOMER") == 0 &&
                 (strcmp(choice_text, "2") == 0 ||
                  strcmp(choice_text, "3") == 0)) {
            char account_id[64];
            int64_t amount;
            const char *operation = strcmp(choice_text, "2") == 0
                                    ? "DEPOSIT" : "WITHDRAW";

            if (prompt_account_id("Your account ID: ", account_id,
                                  sizeof(account_id)) < 0 ||
                prompt_positive_amount(&amount) < 0)
                continue;

            set_command(command, sizeof(command), "%s|%s|%" PRId64,
                        operation, account_id, amount);
        }

        /* Customer-only transfer. */
        else if (strcmp(role, "CUSTOMER") == 0 &&
                 strcmp(choice_text, "4") == 0) {
            char source[64];
            char destination[64];
            int64_t amount;

            if (prompt_account_id(
                    "Source account ID: ",
                    source,
                    sizeof(source)
                ) < 0) {
                continue;
            }

            if (prompt_account_id(
                    "Destination account ID: ",
                    destination,
                    sizeof(destination)
                ) < 0) {
                continue;
            }

            if (strcmp(source, destination) == 0) {
                printf(RED "Source and destination must differ.\n" RESET);
                continue;
            }

            if (prompt_positive_amount(&amount) < 0)
                continue;

            set_command(command, sizeof(command),
                        "TRANSFER|%s|%s|%" PRId64,
                        source, destination, amount);
        }

                /* Transaction history / employee passbook. */
        else if (
            (strcmp(role, "CUSTOMER") == 0 &&
             strcmp(choice_text, "5") == 0) ||
            (strcmp(role, "EMPLOYEE") == 0 &&
             strcmp(choice_text, "2") == 0)
        ) {
            char account_id[64];

            const char *prompt =
                strcmp(role, "CUSTOMER") == 0
                ? "Your account ID: "
                : "Customer account ID for passbook: ";

            if (prompt_account_id(
                    prompt,
                    account_id,
                    sizeof(account_id)
                ) < 0) {
                continue;
            }

            set_command(command, sizeof(command), "HISTORY|%s", account_id);
        }
         /* Customer applies for a loan. */
        else if (strcmp(role, "CUSTOMER") == 0 &&
                 strcmp(choice_text, "6") == 0) {
            char account_id[64];
            char purpose[128];
            int64_t amount;

            if (prompt_account_id(
                    "Your account ID: ",
                    account_id,
                    sizeof(account_id)
                ) < 0) {
                continue;
            }

            if (prompt_positive_amount(&amount) < 0)
                continue;

            if (prompt_field("Loan purpose (e.g. education): ", purpose,
                             sizeof(purpose), "Invalid loan purpose.") < 0)
                continue;

            set_command(command, sizeof(command),
                        "LOAN_APPLY|%s|%" PRId64 "|%s",
                        account_id, amount, purpose);
        }

        /* Customer views their applications. */
        else if (strcmp(role, "CUSTOMER") == 0 &&
                 strcmp(choice_text, "7") == 0) {
            set_command(command, sizeof(command), "LOAN_MY");
        }

        /* Manager views pending applications. */
        else if (strcmp(role, "MANAGER") == 0 &&
                 strcmp(choice_text, "2") == 0) {
            set_command(command, sizeof(command), "LOAN_PENDING");
        }

        /* Manager assigns an application to an employee. */
        else if (strcmp(role, "MANAGER") == 0 &&
                 strcmp(choice_text, "3") == 0) {
            char loan_id[16];
            char employee[USERNAME_MAX + 1];

            if (prompt_field("Loan application ID (e.g. LN000001): ", loan_id,
                             sizeof(loan_id), "Invalid loan ID.") < 0)
                continue;

            if (prompt_field("Employee username: ", employee, sizeof(employee),
                             "Invalid employee username.") < 0)
                continue;

            set_command(command, sizeof(command), "LOAN_ASSIGN|%s|%s", loan_id, employee);
        }

        /* Employee views applications assigned to them. */
        else if (strcmp(role, "EMPLOYEE") == 0 &&
                 strcmp(choice_text, "3") == 0) {
            set_command(command, sizeof(command), "LOAN_ASSIGNED");
        }

        /* Employee approves or rejects an assigned application. */
        else if (strcmp(role, "EMPLOYEE") == 0 &&
                 strcmp(choice_text, "4") == 0) {
            char loan_id[16];
            char decision_choice[16];
            const char *decision;

            if (prompt_field("Loan application ID (e.g. LN000001): ", loan_id,
                             sizeof(loan_id), "Invalid loan ID.") < 0)
                continue;

            printf("  1. Approve application\n");
            printf("  2. Reject application\n");

            if (read_input(
                    "Choose decision: ",
                    decision_choice,
                    sizeof(decision_choice)
                ) < 0) {
                continue;
            }

            if (strcmp(decision_choice, "1") == 0) {
                decision = "APPROVED";
            }
            else if (strcmp(decision_choice, "2") == 0) {
                decision = "REJECTED";
            }
            else {
                printf(RED "Invalid decision.\n" RESET);
                continue;
            }

            set_command(command, sizeof(command), "LOAN_DECIDE|%s|%s", loan_id, decision);
        }
                /* Customer submits feedback. */
        else if (strcmp(role, "CUSTOMER") == 0 &&
                 strcmp(choice_text, "8") == 0) {
            char message[301];

            if (prompt_field("Enter feedback (max 300 characters): ", message,
                             sizeof(message), "Invalid feedback message.") < 0)
                continue;

            set_command(command, sizeof(command), "FEEDBACK_ADD|%s", message);
        }

        /* Manager reviews recent customer feedback. */
        else if (strcmp(role, "MANAGER") == 0 &&
                 strcmp(choice_text, "4") == 0) {
            set_command(command, sizeof(command), "FEEDBACK_LIST");
        }
                /* Manager activates or deactivates an account. */
        else if (strcmp(role, "MANAGER") == 0 &&
                 strcmp(choice_text, "5") == 0) {
            char account_id[64];
            char status_choice[16];
            char status;

            if (prompt_account_id(
                    "Account ID: ",
                    account_id,
                    sizeof(account_id)
                ) < 0) {
                continue;
            }

            printf("  1. Activate account\n");
            printf("  2. Deactivate account\n");

            if (read_input(
                    "Choose action: ",
                    status_choice,
                    sizeof(status_choice)
                ) < 0) {
                continue;
            }

            if (strcmp(status_choice, "1") == 0) {
                status = 'A';
            }
            else if (strcmp(status_choice, "2") == 0) {
                status = 'I';
            }
            else {
                printf(RED "Invalid choice.\n" RESET);
                continue;
            }

            set_command(command, sizeof(command), "ACCOUNT_STATUS|%s|%c", account_id, status);
        }

        /* Employee adds a customer login and bank account. */
        else if (strcmp(role, "EMPLOYEE") == 0 &&
                 strcmp(choice_text, "5") == 0) {
            char customer_username[32];
            char password[PASSWORD_MAX + 1] = {0};
            char account_id[64];
            int64_t initial_balance;

            if (prompt_field("New customer username (max 16 characters): ",
                             customer_username, sizeof(customer_username),
                             "Invalid customer username.") < 0)
                continue;
            if (strlen(customer_username) > 16) {
                printf(RED "Invalid customer username.\n" RESET);
                continue;
            }

            if (prompt_password("Set customer password (hidden): ", password,
                                sizeof(password), 1, "Invalid password.") < 0)
                continue;

            if (prompt_account_id("New account ID: ", account_id,
                                  sizeof(account_id)) < 0)
                continue;
            if (strlen(account_id) > 16) {
                printf(RED "Invalid account ID.\n" RESET);
                continue;
            }

            if (prompt_amount(&initial_balance) < 0 ||
                initial_balance < 0) {
                printf(RED "Invalid initial balance.\n" RESET);
                continue;
            }

            set_command(command, sizeof(command),
            "CUSTOMER_CREATE|%s|%s|%s|%" PRId64,
            customer_username,
            password,
            account_id,
            initial_balance);

        }
        /* Employee modifies a customer's profile details. */
        else if (
                (strcmp(role, "EMPLOYEE") == 0 &&
                strcmp(choice_text, "6") == 0) ||
                (strcmp(role, "ADMIN") == 0 &&
                strcmp(choice_text, "3") == 0)
            ) {

            char account_id[64];
            char full_name[CUSTOMER_NAME_MAX + 1];
            char phone[CUSTOMER_PHONE_MAX + 1];

            if (prompt_account_id(
                    "Customer account ID: ",
                    account_id,
                    sizeof(account_id)
                ) < 0) {
                continue;
            }

            if (prompt_field("Updated customer full name: ", full_name,
                             sizeof(full_name), "Invalid customer name.") < 0)
                continue;

            if (prompt_phone(phone, sizeof(phone)) < 0)
                continue;

            set_command(command, sizeof(command),
                        "CUSTOMER_DETAILS_SET|%s|%s|%s",
                        account_id, full_name, phone);
        }
        /* Administrator modifies employee profile details. */
        else if (strcmp(role, "ADMIN") == 0 &&
                strcmp(choice_text, "4") == 0) {

            char employee_username[USERNAME_MAX + 1];
            char full_name[CUSTOMER_NAME_MAX + 1];
            char phone[CUSTOMER_PHONE_MAX + 1];

            if (prompt_field("Employee username: ", employee_username,
                             sizeof(employee_username),
                             "Invalid employee username.") < 0)
                continue;

            if (prompt_field("Updated employee full name: ", full_name,
                             sizeof(full_name), "Invalid employee name.") < 0)
                continue;

            if (prompt_phone(phone, sizeof(phone)) < 0)
                continue;

            set_command(command, sizeof(command),
            "EMPLOYEE_DETAILS_SET|%s|%s|%s",
            employee_username,
            full_name,
            phone);
        }
        /* Administrator creates a new employee login. */
    else if (strcmp(role, "ADMIN") == 0 &&
            strcmp(choice_text, "5") == 0) {

        char employee_username[USERNAME_MAX + 1];
        char password[PASSWORD_MAX + 1] = {0};

        if (prompt_field("New employee username: ", employee_username,
                         sizeof(employee_username),
                         "Invalid employee username.") < 0)
            continue;

        if (prompt_password("Set employee password (hidden): ", password,
                            sizeof(password), 1, "Invalid password.") < 0)
            continue;

        set_command(command, sizeof(command), "EMPLOYEE_CREATE|%s|%s",
                    employee_username, password);

    }

            /* Administrator manages user roles. */
        else if (strcmp(role, "ADMIN") == 0 &&
                 strcmp(choice_text, "6") == 0) {

            char target_username[USERNAME_MAX + 1];
            char new_role[16];

            if (prompt_field("Username whose role you want to change: ",
                             target_username, sizeof(target_username),
                             "Invalid username.") < 0)
                continue;

            printf(
                "Available roles: CUSTOMER, EMPLOYEE, MANAGER, ADMIN\n"
            );

            if (read_input(
                    "New role (uppercase): ",
                    new_role,
                    sizeof(new_role)
                ) < 0) {
                printf(RED "Invalid role.\n" RESET);
                continue;
            }

            if (strcmp(new_role, "CUSTOMER") != 0 &&
                strcmp(new_role, "EMPLOYEE") != 0 &&
                strcmp(new_role, "MANAGER") != 0 &&
                strcmp(new_role, "ADMIN") != 0) {

                printf(RED "Invalid role.\n" RESET);
                continue;
            }

            set_command(command, sizeof(command), "USER_ROLE_SET|%s|%s",
                        target_username, new_role);
        }
        /* Administrator-only account creation. */
        else if (strcmp(role, "ADMIN") == 0 &&
                 strcmp(choice_text, "2") == 0) {
            char account_id[64];
            char customer_username[64];
            int64_t amount;

            if (prompt_account_id(
                    "New account ID: ",
                    account_id,
                    sizeof(account_id)
                ) < 0) {
                continue;
            }

            if (prompt_field("Existing customer username (max 16 characters): ",
                             customer_username, sizeof(customer_username),
                             "Invalid customer username.") < 0)
                continue;
            if (strlen(customer_username) > 16) {
                printf(RED "Invalid customer username.\n" RESET);
                continue;
            }

            if (prompt_amount(&amount) < 0)
                continue;

            set_command(command, sizeof(command),
            "CREATE_ACCOUNT|%s|%s|%" PRId64,
            account_id,
            customer_username,
            amount);
        }

        /* Employee closes an account after its balance reaches zero. */
        else if (strcmp(role, "EMPLOYEE") == 0 &&
                 strcmp(choice_text, "7") == 0) {
            char account_id[64];
            char confirmation[16];

            if (prompt_account_id("Account ID to close: ",
                                  account_id, sizeof(account_id)) < 0) {
                continue;
            }

            printf(RED
                   "Account closure is permanent and requires a zero balance.\n"
                   RESET);
            if (read_input("Type CLOSE to confirm: ",
                           confirmation, sizeof(confirmation)) < 0 ||
                strcmp(confirmation, "CLOSE") != 0) {
                printf("Account closure cancelled.\n");
                continue;
            }

            set_command(command, sizeof(command), "ACCOUNT_CLOSE|%s", account_id);
        }

        /* Any authenticated role can change its own password. */
        else if (
            (strcmp(role, "CUSTOMER") == 0 &&
             strcmp(choice_text, "9") == 0) ||
            (strcmp(role, "EMPLOYEE") == 0 &&
             strcmp(choice_text, "8") == 0) ||
            (strcmp(role, "MANAGER") == 0 &&
             strcmp(choice_text, "6") == 0) ||
            (strcmp(role, "ADMIN") == 0 &&
             strcmp(choice_text, "7") == 0)
        ) {
            char current_password[PASSWORD_MAX + 1] = {0};
            char new_password[PASSWORD_MAX + 1] = {0};

            if (prompt_password("Current password (hidden): ",
                                current_password, sizeof(current_password), 1,
                                "Invalid current password input.") < 0 ||
                prompt_password("New password (minimum 8 characters): ",
                                new_password, sizeof(new_password), 8,
                                "Invalid new password. Use 8-128 characters and no '|'.") < 0)
                continue;

            set_command(command, sizeof(command), "PASSWORD_CHANGE|%s|%s",
                        current_password, new_password);

        }
        else {
            printf(RED "Invalid choice for this role.\n" RESET);
            continue;
        }

        /* A formatting error leaves command empty; never send a partial request. */
        if (command[0] == '\0')
            continue;

        /* The server remains responsible for authorization. */
        if (request_server(
                fd, command, response, sizeof(response)
            ) < 0) {
            printf(RED "Connection lost.\n" RESET);
            return -1;
        }

        display_response(response);
    }
}

int main(void)
{
    for (;;) {
        /* First ask which type of user wants to log in. */
        const char *selected_role = select_login_role();

        if (selected_role == NULL) {
            printf("Thank you for using SATYAM BANK.\n");
            return EXIT_SUCCESS;
        }

        /* Connect to the server. */
        int fd = connect_to_server();

        if (fd < 0) {
            fprintf(stderr,
                    "Could not connect. Start the server first.\n");
            return EXIT_FAILURE;
        }

        char username[USERNAME_MAX + 1];
        char role[32];

        /* Show login prompts for the selected role. */
        if (perform_login(fd, username, sizeof(username),
                          role, sizeof(role), selected_role) < 0) {
            close(fd);
            continue;
        }

        /* Run the appropriate menu after successful login. */
        int result = run_menu(fd, username, role);

        close(fd);

        /* Logout returns to the login-type menu. */
        if (result == 1)
            continue;

        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
}
