CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -g
THREAD_FLAGS := -pthread
CPPFLAGS := -Iinclude
LDLIBS := -pthread
BUILD_DIR := build

SERVER_SOURCES := storage/storage.c storage/transactions.c storage/loans.c \
                  storage/feedback.c auth/auth.c server/server.c

.PHONY: all test clean clean-test-data

all: $(BUILD_DIR)/server $(BUILD_DIR)/client $(BUILD_DIR)/user_setup

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR) data

$(BUILD_DIR)/server: $(SERVER_SOURCES) include/auth.h include/storage.h include/transactions.h include/loans.h include/feedback.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) $(SERVER_SOURCES) -o $@ $(LDLIBS)

$(BUILD_DIR)/client: client/client.c include/storage.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) client/client.c -o $@ $(LDLIBS)

$(BUILD_DIR)/user_setup: auth/auth.c tools/user_setup.c include/auth.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) auth/auth.c tools/user_setup.c -o $@ $(LDLIBS)

$(BUILD_DIR)/storage_test: storage/storage.c tests/storage_test.c include/storage.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) storage/storage.c tests/storage_test.c -o $@

$(BUILD_DIR)/transaction_test: storage/storage.c tests/transaction_test.c include/storage.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) storage/storage.c tests/transaction_test.c -o $@

$(BUILD_DIR)/journal_recovery_test: storage/storage.c tests/journal_recovery_test.c include/storage.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) storage/storage.c tests/journal_recovery_test.c -o $@

$(BUILD_DIR)/transaction_ledger_test: storage/transactions.c tests/transaction_ledger_test.c include/transactions.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) storage/transactions.c tests/transaction_ledger_test.c -o $@

$(BUILD_DIR)/concurrent_withdrawal_test: storage/storage.c tests/concurrent_withdrawal_test.c include/storage.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) storage/storage.c tests/concurrent_withdrawal_test.c -o $@

$(BUILD_DIR)/record_lock_test: storage/storage.c tests/record_lock_test.c include/storage.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) storage/storage.c tests/record_lock_test.c -o $@

$(BUILD_DIR)/loan_test: storage/loans.c tests/loan_test.c include/loans.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) storage/loans.c tests/loan_test.c -o $@

$(BUILD_DIR)/feedback_test: storage/feedback.c tests/feedback_test.c include/feedback.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) storage/feedback.c tests/feedback_test.c -o $@

$(BUILD_DIR)/account_status_test: storage/storage.c tests/account_status_test.c include/storage.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) storage/storage.c tests/account_status_test.c -o $@

$(BUILD_DIR)/account_close_test: storage/storage.c tests/account_close_test.c include/storage.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) storage/storage.c tests/account_close_test.c -o $@

$(BUILD_DIR)/auth_cleanup_test: auth/auth.c tests/auth_cleanup_test.c include/auth.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(THREAD_FLAGS) $(CPPFLAGS) auth/auth.c tests/auth_cleanup_test.c -o $@ $(LDLIBS)

test: all $(BUILD_DIR)/storage_test $(BUILD_DIR)/transaction_test $(BUILD_DIR)/journal_recovery_test \
      $(BUILD_DIR)/transaction_ledger_test $(BUILD_DIR)/concurrent_withdrawal_test \
      $(BUILD_DIR)/record_lock_test $(BUILD_DIR)/loan_test $(BUILD_DIR)/feedback_test \
      $(BUILD_DIR)/account_status_test $(BUILD_DIR)/account_close_test \
      $(BUILD_DIR)/auth_cleanup_test
	@set -e; \
	$(MAKE) clean-test-data >/dev/null; \
	./$(BUILD_DIR)/storage_test; \
	./$(BUILD_DIR)/transaction_test; \
	./$(BUILD_DIR)/journal_recovery_test; \
	./$(BUILD_DIR)/transaction_ledger_test; \
	./$(BUILD_DIR)/concurrent_withdrawal_test; \
	./$(BUILD_DIR)/record_lock_test; \
	./$(BUILD_DIR)/loan_test; \
	./$(BUILD_DIR)/feedback_test; \
	./$(BUILD_DIR)/account_status_test; \
	./$(BUILD_DIR)/account_close_test; \
	./$(BUILD_DIR)/auth_cleanup_test; \
	echo "ALL 11 TESTS PASSED"

clean:
	rm -rf $(BUILD_DIR)

clean-test-data:
	rm -f data/accounts.txt data/accounts.txt.indexlock \
	  data/test_accounts.txt data/test_accounts.txt.indexlock \
	  data/test_journal.log data/test_journal_recovery.log data/test_journal_recovery_accounts.txt \
	  data/test_journal_recovery_accounts.txt.indexlock data/test_transactions.txt \
	  data/concurrent_accounts.txt data/concurrent_accounts.txt.indexlock \
	  data/concurrent_journal.log data/record_lock_probe.dat \
	  data/test_loans.txt data/test_loans.txt.lock \
	  data/test_feedback.txt data/test_feedback.txt.lock \
	  data/test_status_accounts.txt data/test_status_accounts.txt.indexlock \
	  data/test_status_journal.log data/test_close_accounts.txt \
	  data/test_close_accounts.txt.indexlock data/test_close_journal.log \
	  data/test_auth_cleanup_users.txt
