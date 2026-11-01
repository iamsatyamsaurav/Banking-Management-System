#ifndef FEEDBACK_H
#define FEEDBACK_H

#include <stddef.h>

#define FEEDBACK_OK 0
#define FEEDBACK_NOT_FOUND 1
#define FEEDBACK_ERROR (-1)

int feedback_submit(const char *path, const char *customer, const char *message);
int feedback_list(const char *path, char *output, size_t output_size);

#endif
