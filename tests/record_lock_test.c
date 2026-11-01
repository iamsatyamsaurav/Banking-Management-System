#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define RECORD_SIZE 128

static int lock_range(
    int fd,
    short lock_type,
    off_t start,
    int command
)
{
    struct flock lock = {0};

    lock.l_type = lock_type;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = RECORD_SIZE;

    return fcntl(fd, command, &lock);
}

int main(void)
{
    const char *path = "data/record_lock_probe.dat";

    int fd1 = open(
        path,
        O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC,
        0600
    );

    if (fd1 < 0) {
        perror("open fd1");
        return 1;
    }

    if (ftruncate(fd1, 2 * RECORD_SIZE) < 0) {
        perror("ftruncate");
        close(fd1);
        return 1;
    }

    /*
     * Open the same file again. This creates a separate
     * open file description, so the two OFD locks can conflict.
     */
    int fd2 = open(path, O_RDWR | O_CLOEXEC);

    if (fd2 < 0) {
        perror("open fd2");
        close(fd1);
        return 1;
    }

    /* Lock the first record exclusively through fd1. */
    if (lock_range(
            fd1,
            F_WRLCK,
            0,
            F_OFD_SETLKW
        ) < 0) {

        perror("lock record 0");
        close(fd2);
        close(fd1);
        return 1;
    }

    /*
     * Try the same record through fd2 without waiting.
     * A conflicting write lock should fail with EAGAIN.
     */
    errno = 0;

    if (lock_range(
            fd2,
            F_WRLCK,
            0,
            F_OFD_SETLK
        ) == 0) {

        fprintf(stderr,
                "FAIL: conflicting record lock was granted.\n");

        lock_range(fd2, F_UNLCK, 0, F_OFD_SETLK);
        lock_range(fd1, F_UNLCK, 0, F_OFD_SETLK);
        close(fd2);
        close(fd1);
        return 1;
    }

    if (errno != EAGAIN && errno != EACCES) {
        perror("unexpected locking error");
        lock_range(fd1, F_UNLCK, 0, F_OFD_SETLK);
        close(fd2);
        close(fd1);
        return 1;
    }

    printf("PASS: conflicting lock on record 0 was blocked.\n");

    /*
     * Record 1 occupies bytes 128–255.
     * It does not overlap record 0, so fd2 can lock it.
     */
    if (lock_range(
            fd2,
            F_WRLCK,
            RECORD_SIZE,
            F_OFD_SETLK
        ) < 0) {

        perror("lock record 1");
        lock_range(fd1, F_UNLCK, 0, F_OFD_SETLK);
        close(fd2);
        close(fd1);
        return 1;
    }

    printf("PASS: independent lock on record 1 was granted.\n");

    lock_range(fd2, F_UNLCK, RECORD_SIZE, F_OFD_SETLK);
    lock_range(fd1, F_UNLCK, 0, F_OFD_SETLK);

    close(fd2);
    close(fd1);

    unlink(path);

    printf("Record-lock primitive test passed.\n");

    return 0;
}