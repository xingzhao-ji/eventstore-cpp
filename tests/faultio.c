/* Test-only Linux interposer. Never linked into the shipped CLI/library. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int fsync(int fd) {
    int (*real_fsync)(int) = dlsym(RTLD_NEXT, "fsync");
    struct stat info;
    const char *mode = getenv("EVENTSTORE_TEST_FAIL_FSYNC");
    if (mode && fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
        if (strcmp(mode, "once") == 0) unsetenv("EVENTSTORE_TEST_FAIL_FSYNC");
        errno = EIO;
        return -1;
    }
    return real_fsync(fd);
}
int ftruncate(int fd, off_t length) {
    int (*real_truncate)(int, off_t) = dlsym(RTLD_NEXT, "ftruncate");
    if (getenv("EVENTSTORE_TEST_FAIL_TRUNCATE")) { errno = EIO; return -1; }
    return real_truncate(fd, length);
}
