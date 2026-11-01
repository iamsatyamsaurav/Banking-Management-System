# SATYAM BANK — Banking Management System

IIIT Bangalore · CS-513 System Software mini-project

A C client/server application that demonstrates role-based banking workflows, TCP socket communication, pthread-based concurrency, a binary semaphore for the active-session registry, file locking, fixed-width text records, and single-slot undo-journal recovery for balance changes. This is an academic demonstration, **not a production banking system**.

## Requirements

- Linux/WSL with GCC and `make` (`sudo apt install build-essential make` on Ubuntu)
- Run all commands from the project root because data paths are relative to the current directory

## Build and test

```bash
make
make test
```

The test target compiles the client, server, setup utility, and 11 tests. `make clean-test-data` removes only named test artifacts; it does not erase the normal application data files. `make clean` removes generated executables.

## First-time setup

Create an administrator and manager. The utility asks for a password without echoing it:

```bash
./build/user_setup admin01 ADMIN
./build/user_setup manager01 MANAGER
```

Log in as `admin01` and choose **Add new bank employee** to create an employee login. The employee can then create a customer and an account.

## Run the application

Open two Ubuntu terminals, both in this project directory.

Terminal 1:

```bash
./build/server
```

Terminal 2:

```bash
./build/client
```

The client connects to `127.0.0.1:8080`. **Logout** closes the current session and returns to the login prompt; **Exit application** logs out and closes the client. Use demo-only credentials and data.

## Main modules

| File | Responsibility |
|---|---|
| `client/client.c` | Terminal menus, password prompts, TCP client, command formatting and response display. |
| `server/server.c` | TCP listener, one detached pthread per client, authentication, role checks, sessions and command dispatch. |
| `auth/auth.c` | Fixed-width user records, demo plaintext passwords, role changes and password updates. |
| `storage/storage.c` | Fixed-width account records, balances, account status, record/index locks, transaction journal and recovery. |
| `storage/transactions.c` | Append-only transaction history for successful deposit/withdrawal/transfer operations. |
| `storage/loans.c` | Loan application storage, manager assignment and employee decisions. |
| `storage/feedback.c` | Customer feedback storage and manager review. |
| `tools/user_setup.c` | Trusted local utility for creating the initial accounts/roles. |

See [`docs/architecture.svg`](docs/architecture.svg), [`docs/project-report.md`](docs/project-report.md), and [`docs/viva-notes.md`](docs/viva-notes.md).

## Data and safety notes

Application records are stored under `data/`; account balances use integer paise to avoid floating-point money arithmetic. To keep this academic version easy to study, passwords are stored as plaintext in fixed-width user records. This is intentionally simplified and is **not safe for real users or production deployment**. Use dummy credentials only. User records use the simplified fixed-width format; feedback records and the journal format have also changed. Before running this version over an existing project directory, remove old generated demo data under `data/` (keep `.gitkeep`); do not reuse data from the previous version.

The server binds only to loopback, so it accepts connections from the same machine. Do not put real customer information or credentials in this academic project. The tests create named `data/test_*` and related files; use `make clean-test-data` after testing if you want to remove them.

## Features and current design boundaries

- Customer: balance, deposit, withdrawal, transfer, history, loan application/list, feedback and password change.
- Employee: customer creation/details update, passbook/history, assigned loans, loan decision, zero-balance account closure and password change.
- Manager: loan assignment, customer account activation/deactivation, feedback review and password change.
- Administrator: employee creation, customer/employee details, user role management and password change.
- Closed accounts have a permanent `CLOSED` status; they cannot transact or be reactivated. Deactivated accounts can be reactivated.
- Balance-changing operations use a single-slot undo journal with recovery at server startup. The separate history ledger is appended after the balance mutation; a history-write error is warned about rather than reversing a completed balance operation.

This implementation uses the server/client process boundary and sockets for IPC, pthreads for per-client concurrency, a binary POSIX semaphore for the active-session table, and file locks for shared record access. It deliberately uses a thread-per-client model rather than `fork()` per client.
