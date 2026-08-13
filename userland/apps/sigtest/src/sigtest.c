#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <stlx/proc.h>

#define HELPER_STACK_SIZE (64 * 1024)

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

static volatile sig_atomic_t usr1_count = 0;

static void usr1_handler(int sig) {
    (void)sig;
    usr1_count++;
}

static void test_basic_delivery(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = usr1_handler;

    check("sigaction installs handler", sigaction(SIGUSR1, &sa, NULL) == 0);

    raise(SIGUSR1);
    check("handler ran once", usr1_count == 1);

    raise(SIGUSR1);
    check("disposition survives delivery", usr1_count == 2);
}

static volatile sig_atomic_t info_signo = -1;
static volatile sig_atomic_t info_pid = -1;

static void usr2_handler(int sig, siginfo_t* info, void* ctx) {
    (void)sig;
    (void)ctx;
    info_signo = info->si_signo;
    info_pid = (sig_atomic_t)info->si_pid;
}

static void test_siginfo_delivery(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = usr2_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &sa, NULL);

    raise(SIGUSR2);
    check("SA_SIGINFO handler saw si_signo", info_signo == SIGUSR2);
    check("synthesized si_pid is 0", info_pid == 0);
}

static volatile sig_atomic_t defer_depth = 0;
static volatile sig_atomic_t defer_max_depth = 0;
static volatile sig_atomic_t defer_count = 0;

static void defer_handler(int sig) {
    (void)sig;
    defer_depth++;
    if (defer_depth > defer_max_depth) {
        defer_max_depth = defer_depth;
    }
    defer_count++;
    if (defer_count == 1) {
        /* Own signal is blocked during the handler, so this must pend */
        raise(SIGUSR1);
    }
    defer_depth--;
}

static void test_deferred_reentry(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = defer_handler;
    sigaction(SIGUSR1, &sa, NULL);

    defer_count = 0;
    raise(SIGUSR1);
    check("re-raise delivered after return", defer_count == 2);
    check("handler never nested", defer_max_depth == 1);
}

static void test_unblock_delivers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = usr1_handler;
    sigaction(SIGUSR1, &sa, NULL);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);

    usr1_count = 0;
    raise(SIGUSR1);
    check("blocked signal stays pending", usr1_count == 0);

    sigset_t pend;
    sigpending(&pend);
    check("sigpending reports it", sigismember(&pend, SIGUSR1) == 1);

    sigprocmask(SIG_UNBLOCK, &set, NULL);
    check("unblock delivers immediately", usr1_count == 1);
}

static volatile sig_atomic_t eintr_handler_ran = 0;

static void eintr_handler(int sig) {
    (void)sig;
    eintr_handler_ran = 1;
}

static void eintr_helper(void* arg) {
    (void)arg;

    /* Keep the signal blocked here so the main thread must receive it */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);

    usleep(200 * 1000);
    kill(getpid(), SIGUSR1);
    _exit(0);
}

static void test_read_eintr(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    /* No SA_RESTART: the interrupted read must fail with EINTR */
    sa.sa_handler = eintr_handler;
    sigaction(SIGUSR1, &sa, NULL);

    int fds[2];
    if (pipe(fds) != 0) {
        printf("  SKIP: pipe unavailable\n");
        return;
    }

    void* stk = mmap(NULL, HELPER_STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stk == MAP_FAILED) {
        printf("  SKIP: helper stack unavailable\n");
        close(fds[0]);
        close(fds[1]);
        return;
    }

    int h = proc_create_thread(eintr_helper, NULL,
                               (char*)stk + HELPER_STACK_SIZE, "sig_helper");
    if (h < 0) {
        printf("  SKIP: thread creation unavailable\n");
        munmap(stk, HELPER_STACK_SIZE);
        close(fds[0]);
        close(fds[1]);
        return;
    }
    proc_thread_start(h);

    char byte;
    ssize_t n = read(fds[0], &byte, 1);
    int saved_errno = errno;

    proc_thread_join(h, NULL);

    check("read interrupted by handler", n == -1);
    check("errno is EINTR", saved_errno == EINTR);
    check("handler ran before read returned", eintr_handler_ran == 1);

    munmap(stk, HELPER_STACK_SIZE);
    close(fds[0]);
    close(fds[1]);
}

static volatile sig_atomic_t restart_handler_ran = 0;
static int g_restart_wfd;

static void restart_handler(int sig) {
    (void)sig;
    restart_handler_ran = 1;
}

static void restart_helper(void* arg) {
    (void)arg;

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);

    usleep(150 * 1000);
    kill(getpid(), SIGUSR1);      /* interrupts the read, restart re-blocks */
    usleep(150 * 1000);
    char b = 'r';
    write(g_restart_wfd, &b, 1);  /* completes the restarted read */
    _exit(0);
}

static void test_read_restart(void) {
    /* signal() installs with SA_RESTART, the read must complete instead
     * of failing with EINTR */
    if (signal(SIGUSR1, restart_handler) == SIG_ERR) {
        printf("  SKIP: signal unavailable\n");
        return;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        printf("  SKIP: pipe unavailable\n");
        return;
    }
    g_restart_wfd = fds[1];

    void* stk = mmap(NULL, HELPER_STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stk == MAP_FAILED) {
        printf("  SKIP: helper stack unavailable\n");
        close(fds[0]);
        close(fds[1]);
        return;
    }
    int h = proc_create_thread(restart_helper, NULL,
                               (char*)stk + HELPER_STACK_SIZE, "sig_helper");
    if (h < 0) {
        printf("  SKIP: thread creation unavailable\n");
        munmap(stk, HELPER_STACK_SIZE);
        close(fds[0]);
        close(fds[1]);
        return;
    }
    proc_thread_start(h);

    char byte = 0;
    ssize_t n = read(fds[0], &byte, 1);

    proc_thread_join(h, NULL);

    check("restarted read completed", n == 1 && byte == 'r');
    check("handler ran during restart", restart_handler_ran == 1);

    munmap(stk, HELPER_STACK_SIZE);
    close(fds[0]);
    close(fds[1]);
}

static void test_poll_eintr_despite_restart(void) {
    /* poll is never restarted, SA_RESTART or not (as on Linux) */
    signal(SIGUSR1, restart_handler);

    int fds[2];
    if (pipe(fds) != 0) {
        printf("  SKIP: pipe unavailable\n");
        return;
    }

    void* stk = mmap(NULL, HELPER_STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stk == MAP_FAILED) {
        printf("  SKIP: helper stack unavailable\n");
        close(fds[0]);
        close(fds[1]);
        return;
    }
    int h = proc_create_thread(eintr_helper, NULL,
                               (char*)stk + HELPER_STACK_SIZE, "sig_helper");
    if (h < 0) {
        printf("  SKIP: thread creation unavailable\n");
        munmap(stk, HELPER_STACK_SIZE);
        close(fds[0]);
        close(fds[1]);
        return;
    }
    proc_thread_start(h);

    struct pollfd pfd = { .fd = fds[0], .events = POLLIN, .revents = 0 };
    int ret = poll(&pfd, 1, -1);
    int saved_errno = errno;

    proc_thread_join(h, NULL);

    check("poll interrupted despite SA_RESTART", ret == -1);
    check("poll errno is EINTR", saved_errno == EINTR);

    munmap(stk, HELPER_STACK_SIZE);
    close(fds[0]);
    close(fds[1]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("sigtest: running signal delivery tests\n");

    test_basic_delivery();
    test_siginfo_delivery();
    test_deferred_reentry();
    test_unblock_delivers();
    test_read_eintr();
    test_read_restart();
    test_poll_eintr_despite_restart();

    printf("sigtest: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
