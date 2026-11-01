#include "feedback.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_PATH "data/test_feedback.txt"

int main(void)
{
    unlink(TEST_PATH);
    unlink(TEST_PATH ".lock");
    char output[8192];

    if (feedback_submit(TEST_PATH, "CUS0000000001",
                        "The mobile banking experience is good.") != FEEDBACK_OK ||
        feedback_submit(TEST_PATH, "CUS0000000002",
                        "Please improve the transaction notifications.") != FEEDBACK_OK) {
        fprintf(stderr, "FAIL: Feedback submission failed.\n");
        return 1;
    }

    if (feedback_list(TEST_PATH, output, sizeof(output)) != FEEDBACK_OK) {
        fprintf(stderr, "FAIL: Could not list feedback.\n");
        return 1;
    }
    printf("\n--- Manager feedback review ---\n%s\n", output);
    if (strstr(output, "CUS0000000001") == NULL ||
        strstr(output, "CUS0000000002") == NULL ||
        strstr(output, "mobile banking experience") == NULL ||
        strstr(output, "transaction notifications") == NULL) {
        fprintf(stderr, "FAIL: Feedback entries are missing.\n");
        return 1;
    }
    puts("\nFEEDBACK STORAGE TEST PASSED.");
    return 0;
}
