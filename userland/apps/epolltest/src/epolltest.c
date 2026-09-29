#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stlx/proc.h>

#define HELPER_STACK_SIZE (64 * 1024)
#define MAX_EVENTS        4
#define READ_DATA         0xC0FFEEULL
#define WRITE_DATA        0xBEEFULL
#define WRITER_DELAY_US   (100 * 1000)
#define MASKED_WAIT_MS    5000

static int passed = 0;
static int failed = 0;
static int writer_fd = -1;
static volatile sig_atomic_t usr1_count = 0;

static void check(const char* name, int cond) {
    if (cond) {
        printf("  PASS: %s\n", name);
        passed++;
    } else {
        printf("  FAIL: %s\n", name);
        failed++;
    }
}

static void usr1_handler(int sig) {
    (void)sig;
    usr1_count++;
}

static void test_ready_ends_are_reported(void) {
    int ep = epoll_create1(EPOLL_CLOEXEC);
    int fds[2];
    if (ep < 0 || pipe(fds) != 0) {
        printf("  SKIP: epoll or pipe unavailable\n");
        return;
    }

    struct epoll_event read_interest = { .events = EPOLLIN, .data.u64 = READ_DATA };
    struct epoll_event write_interest = { .events = EPOLLOUT, .data.u64 = WRITE_DATA };
    write(fds[1], "x", 1);
    check("the read end is added", epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &read_interest) == 0);
    check("the write end is added", epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &write_interest) == 0);

    struct epoll_event events[MAX_EVENTS];
    memset(events, 0, sizeof(events));
    int reported = epoll_wait(ep, events, MAX_EVENTS, 0);

    check("both ends are reported", reported == 2);
    check("the read end carries its data", events[0].events == EPOLLIN && events[0].data.u64 == READ_DATA);
    check("the write end carries its data", events[1].events == EPOLLOUT && events[1].data.u64 == WRITE_DATA);

    char byte = 0;
    read(fds[0], &byte, 1);
    reported = epoll_wait(ep, events, MAX_EVENTS, 0);

    check("a drained read end is no longer reported", reported == 1 && events[0].data.u64 == WRITE_DATA);

    close(fds[0]);
    close(fds[1]);
    close(ep);
}

static void test_one_shot(void) {
    int ep = epoll_create1(0);
    int fds[2];
    if (ep < 0 || pipe(fds) != 0) {
        printf("  SKIP: epoll or pipe unavailable\n");
        return;
    }

    struct epoll_event read_interest = { .events = EPOLLIN | EPOLLONESHOT, .data.u64 = READ_DATA };
    write(fds[1], "x", 1);
    epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &read_interest);

    struct epoll_event event;
    int first = epoll_wait(ep, &event, 1, 0);
    int second = epoll_wait(ep, &event, 1, 0);

    memset(&event, 0, sizeof(event));
    epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &read_interest);
    int rearmed = epoll_wait(ep, &event, 1, 0);

    check("a one-shot interest is reported once", first == 1 && second == 0);
    check("a change re-arms a one-shot interest", rearmed == 1 && event.data.u64 == READ_DATA);

    close(fds[0]);
    close(fds[1]);
    close(ep);
}

static void test_refusals(void) {
    int ep = epoll_create1(0);
    int fds[2];
    int directory = open("/", O_RDONLY);
    if (ep < 0 || pipe(fds) != 0 || directory < 0) {
        printf("  SKIP: epoll, pipe or directory unavailable\n");
        return;
    }

    struct epoll_event read_interest = { .events = EPOLLIN, .data.u64 = READ_DATA };
    epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &read_interest);

    check("a second add fails with EEXIST",
          epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &read_interest) == -1 && errno == EEXIST);
    check("removing an absent interest fails with ENOENT",
          epoll_ctl(ep, EPOLL_CTL_DEL, fds[1], NULL) == -1 && errno == ENOENT);
    check("a directory fails with EPERM",
          epoll_ctl(ep, EPOLL_CTL_ADD, directory, &read_interest) == -1 && errno == EPERM);
    check("an epoll watching itself fails with EINVAL",
          epoll_ctl(ep, EPOLL_CTL_ADD, ep, &read_interest) == -1 && errno == EINVAL);

    close(directory);
    close(fds[0]);
    close(fds[1]);
    close(ep);
}

static void write_after_delay(void* unused) {
    (void)unused;
    usleep(WRITER_DELAY_US);
    write(writer_fd, "x", 1);
    _exit(0);
}

static void test_blocking_wait(void) {
    int ep = epoll_create1(0);
    int fds[2];
    void* stack = mmap(NULL, HELPER_STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (ep < 0 || pipe(fds) != 0 || stack == MAP_FAILED) {
        printf("  SKIP: epoll, pipe or helper stack unavailable\n");
        return;
    }

    struct epoll_event read_interest = { .events = EPOLLIN, .data.u64 = READ_DATA };
    epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &read_interest);
    writer_fd = fds[1];

    int writer = proc_create_thread(write_after_delay, NULL, (char*)stack + HELPER_STACK_SIZE, "epoll_writer");
    if (writer < 0) {
        printf("  SKIP: thread creation unavailable\n");
        munmap(stack, HELPER_STACK_SIZE);
        return;
    }

    proc_thread_start(writer);

    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    int reported = epoll_wait(ep, &event, 1, -1);
    proc_thread_join(writer, NULL);

    check("a blocked wait wakes when the pipe fills", reported == 1 && event.data.u64 == READ_DATA);

    munmap(stack, HELPER_STACK_SIZE);
    close(fds[0]);
    close(fds[1]);
    close(ep);
}

static void test_pwait_mask(void) {
    int ep = epoll_create1(0);
    if (ep < 0) {
        printf("  SKIP: epoll unavailable\n");
        return;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = usr1_handler;
    sigaction(SIGUSR1, &sa, NULL);

    sigset_t usr1;
    sigset_t open_mask;
    sigset_t now;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    sigemptyset(&open_mask);
    sigprocmask(SIG_BLOCK, &usr1, NULL);

    struct epoll_event event;
    raise(SIGUSR1);
    int reported = epoll_pwait(ep, &event, 1, MASKED_WAIT_MS, &open_mask);
    int saved_errno = errno;
    sigprocmask(SIG_SETMASK, NULL, &now);

    check("epoll_pwait's mask lets a pending signal interrupt", reported == -1 && saved_errno == EINTR);
    check("epoll_pwait's mask runs the handler", usr1_count == 1);
    check("epoll_pwait's mask ends with the handler", sigismember(&now, SIGUSR1) == 1);

    sigprocmask(SIG_UNBLOCK, &usr1, NULL);
    close(ep);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("epolltest: running epoll tests\n");

    test_ready_ends_are_reported();
    test_one_shot();
    test_refusals();
    test_blocking_wait();
    test_pwait_mask();

    printf("epolltest: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
