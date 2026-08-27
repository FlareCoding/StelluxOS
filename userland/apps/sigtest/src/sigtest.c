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

/* Child mode: a write with no reader must die by default SIGPIPE */
static int pipe_victim_child(void) {
    int fds[2];
    if (pipe(fds) != 0) {
        return 1;
    }

    close(fds[0]);
    char b = 'x';
    write(fds[1], &b, 1);
    return 1; /* only reached if the signal never fired */
}

/* Child mode: a breakpoint trap must die by default SIGTRAP */
static int trap_victim_child(void) {
#if defined(__x86_64__)
    __asm__ volatile("int3");
#elif defined(__aarch64__)
    __asm__ volatile("brk #0");
#endif
    return 1; /* only reached if the trap never fired */
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

static volatile sig_atomic_t async_handler_ran = 0;

static void async_handler(int sig) {
    (void)sig;
    async_handler_ran = 1;
}

static void async_helper(void* arg) {
    (void)arg;

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, NULL);

    usleep(200 * 1000);
    kill(getpid(), SIGUSR2);
    _exit(0);
}

static void test_async_compute_delivery(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = async_handler;
    sigaction(SIGUSR2, &sa, NULL);
    async_handler_ran = 0;

    void* stk = mmap(NULL, HELPER_STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stk == MAP_FAILED) {
        printf("  SKIP: helper stack unavailable\n");
        return;
    }

    int h = proc_create_thread(async_helper, NULL,
                               (char*)stk + HELPER_STACK_SIZE, "sig_helper");
    if (h < 0) {
        printf("  SKIP: thread creation unavailable\n");
        munmap(stk, HELPER_STACK_SIZE);
        return;
    }

    proc_thread_start(h);

    /* Pure compute: no syscall happens until the handler flips the flag,
     * so only asynchronous delivery can end this loop. */
    volatile unsigned long spins = 0;
    while (!async_handler_ran) {
        spins++;
    }

    /* The interrupted loop's state must survive the full-register restore */
    unsigned long resume_point = spins;
    for (int i = 0; i < 1000; i++) {
        spins++;
    }

    proc_thread_join(h, NULL);

    check("handler fired mid-compute without a syscall", async_handler_ran == 1);
    check("interrupted loop state survived", spins == resume_point + 1000);

    munmap(stk, HELPER_STACK_SIZE);
}

static volatile sig_atomic_t sigpipe_count = 0;

static void sigpipe_handler(int sig) {
    (void)sig;
    sigpipe_count++;
}

static void test_sigpipe_dispositions(void) {
    int fds[2];
    char b = 'x';

    /* Ignored: the write fails with EPIPE and the process lives */
    signal(SIGPIPE, SIG_IGN);
    if (pipe(fds) != 0) {
        printf("  SKIP: pipe unavailable\n");
        return;
    }

    close(fds[0]);
    ssize_t n = write(fds[1], &b, 1);
    int saved_errno = errno;
    check("ignored SIGPIPE write fails EPIPE", n == -1 && saved_errno == EPIPE);
    close(fds[1]);

    /* Handled: the handler runs and EPIPE is still returned */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigpipe_handler;
    sigaction(SIGPIPE, &sa, NULL);
    if (pipe(fds) != 0) {
        printf("  SKIP: pipe unavailable\n");
        return;
    }

    close(fds[0]);
    n = write(fds[1], &b, 1);
    saved_errno = errno;

    check("handled SIGPIPE write fails EPIPE", n == -1 && saved_errno == EPIPE);
    check("SIGPIPE handler ran", sigpipe_count == 1);
    close(fds[1]);
    signal(SIGPIPE, SIG_DFL);

    /* Default: a child writing with no reader dies by SIGPIPE */
    static const char* args[] = { "--pipe-victim", NULL };
    int h = proc_create("/bin/sigtest", args);
    if (h < 0) {
        printf("  SKIP: self exec unavailable\n");
        return;
    }

    proc_start(h);
    int status = 0;
    proc_wait(h, &status);
    check("default SIGPIPE kills the writer",
          STLX_WIFSIGNALED(status) && STLX_WTERMSIG(status) == SIGPIPE);
}

/* A breakpoint trap must kill the child with SIGTRAP, not the kernel */
static void test_trap_default_kills(void) {
    static const char* args[] = { "--trap-victim", NULL };
    int h = proc_create("/bin/sigtest", args);
    if (h < 0) {
        printf("  SKIP: self exec unavailable\n");
        return;
    }

    proc_start(h);
    int status = 0;
    proc_wait(h, &status);
    check("default SIGTRAP kills a trapping child",
          STLX_WIFSIGNALED(status) && STLX_WTERMSIG(status) == SIGTRAP);
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "--pipe-victim") == 0) {
        return pipe_victim_child();
    }

    if (argc >= 2 && strcmp(argv[1], "--trap-victim") == 0) {
        return trap_victim_child();
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("sigtest: running signal delivery tests\n");

    test_basic_delivery();
    test_siginfo_delivery();
    test_deferred_reentry();
    test_unblock_delivers();
    test_read_eintr();
    test_read_restart();
    test_poll_eintr_despite_restart();
    test_async_compute_delivery();
    test_sigpipe_dispositions();
    test_trap_default_kills();

    printf("sigtest: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
