#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

static int passed = 0;
static int failed = 0;

static void check(const char* name, int cond) {
    if (cond) {
        printf("  PASS: %s\n", name);
        passed++;
    } else {
        printf("  FAIL: %s\n", name);
        failed++;
    }
}

static void test_nonblocking_probe(void) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        printf("  SKIP: /dev/urandom not available\n");
        return;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    int ret = poll(&pfd, 1, 0);

    check("non-blocking probe returns 1", ret == 1);
    check("revents has POLLIN", (pfd.revents & POLLIN) != 0);

    close(fd);
}

static void test_timeout_sleep(void) {
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);

    int ret = poll(NULL, 0, 200);

    clock_gettime(CLOCK_MONOTONIC, &after);

    long elapsed_ms = (after.tv_sec - before.tv_sec) * 1000
                    + (after.tv_nsec - before.tv_nsec) / 1000000;

    check("nfds=0 timeout returns 0", ret == 0);
    check("elapsed >= 150ms", elapsed_ms >= 150);
}

static void test_negative_fd(void) {
    struct pollfd pfd = { .fd = -1, .events = POLLIN, .revents = 0x7FFF };
    int ret = poll(&pfd, 1, 0);

    check("negative fd returns 0", ret == 0);
    check("negative fd revents == 0", pfd.revents == 0);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("polltest: running poll() syscall tests\n");

    test_nonblocking_probe();
    test_timeout_sleep();
    test_negative_fd();

    printf("polltest: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
