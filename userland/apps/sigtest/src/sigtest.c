#define _GNU_SOURCE
#include <signal.h>
#include <fenv.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <stlx/proc.h>

#define HELPER_STACK_SIZE   (64 * 1024)
#define MASKED_WAIT_SECONDS 5
#define VICTIM_WAIT_SECONDS 1
#define VICTIM_SETTLE_US    (200 * 1000)
#define FAULT_PAGE_SIZE     4096

/* A word here spans a 16-byte boundary, which faults even where misaligned exclusives do not */
#define MISALIGNED_WORD_OFFSET 14

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

static volatile sig_atomic_t handler_rounding = -1;

static void rounding_handler(int sig) {
    (void)sig;
    handler_rounding = fegetround();
}

/* A handler starts from the default FP environment and the interrupted one returns after it */
static void test_handler_fp_environment(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = rounding_handler;
    sigaction(SIGUSR1, &sa, NULL);

    fesetround(FE_UPWARD);
    raise(SIGUSR1);
    int restored = fegetround();
    fesetround(FE_TONEAREST);

    check("handler starts with round to nearest", handler_rounding == FE_TONEAREST);
    check("interrupted rounding mode survives the handler", restored == FE_UPWARD);
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

/* Child mode: SIGTERM blocked only by a wait's mask must kill as soon as the wait ends */
static int wait_mask_victim_child(void) {
    sigset_t term;
    sigemptyset(&term);
    sigaddset(&term, SIGTERM);

    struct timespec wait = { .tv_sec = VICTIM_WAIT_SECONDS, .tv_nsec = 0 };
    ppoll(NULL, 0, &wait, &term);
    _exit(1); /* only reached if the signal outlived the wait */
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

/* Child mode: a misaligned exclusive load must die by default SIGBUS */
static int align_victim_child(void) {
#if defined(__aarch64__)
    static uint64_t words[4] __attribute__((aligned(16)));
    uint32_t value;
    __asm__ volatile("ldxr %w0, [%1]" : "=r"(value) : "r"((char*)words + MISALIGNED_WORD_OFFSET) : "memory");
#endif
    return 1; /* only reached if the load never faulted */
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

static void test_wait_masks(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = usr1_handler;
    sigaction(SIGUSR1, &sa, NULL);

    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    sigprocmask(SIG_BLOCK, &usr1, NULL);

    sigset_t open_mask;
    sigset_t now;
    sigemptyset(&open_mask);
    struct timespec long_wait = { .tv_sec = MASKED_WAIT_SECONDS, .tv_nsec = 0 };

    usr1_count = 0;
    raise(SIGUSR1);
    int ret = ppoll(NULL, 0, &long_wait, &open_mask);
    int saved_errno = errno;
    sigprocmask(SIG_SETMASK, NULL, &now);

    check("ppoll mask lets a pending signal interrupt", ret == -1 && saved_errno == EINTR);
    check("ppoll mask runs the handler", usr1_count == 1);
    check("ppoll mask ends with the handler", sigismember(&now, SIGUSR1) == 1);

    usr1_count = 0;
    raise(SIGUSR1);
    ret = pselect(0, NULL, NULL, NULL, &long_wait, &open_mask);
    saved_errno = errno;
    sigprocmask(SIG_SETMASK, NULL, &now);

    check("pselect mask lets a pending signal interrupt", ret == -1 && saved_errno == EINTR);
    check("pselect mask runs the handler", usr1_count == 1);
    check("pselect mask ends with the handler", sigismember(&now, SIGUSR1) == 1);

    sigset_t usr2;
    sigemptyset(&usr2);
    sigaddset(&usr2, SIGUSR2);
    struct timespec no_wait = { .tv_sec = 0, .tv_nsec = 0 };
    ret = ppoll(NULL, 0, &no_wait, &usr2);
    sigprocmask(SIG_SETMASK, NULL, &now);

    check("ppoll timeout restores the mask",
          ret == 0 && sigismember(&now, SIGUSR2) == 0 && sigismember(&now, SIGUSR1) == 1);

    sigprocmask(SIG_UNBLOCK, &usr1, NULL);
}

static void test_wait_mask_fatal_signal(void) {
    static const char* args[] = { "--wait-mask-victim", NULL };
    int h = proc_create("/bin/sigtest", args);
    if (h < 0) {
        printf("  SKIP: self exec unavailable\n");
        return;
    }

    process_info info;
    proc_start(h);
    proc_info(h, &info);
    usleep(VICTIM_SETTLE_US);
    kill(info.pid, SIGTERM);

    int status = 0;
    proc_wait(h, &status);
    check("a signal blocked only for a wait kills as the wait returns",
          STLX_WIFSIGNALED(status) && STLX_WTERMSIG(status) == SIGTERM);
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

/* A misaligned exclusive load must kill the child with SIGBUS, not fault forever */
static void test_alignment_fault_kills(void) {
#if defined(__aarch64__)
    static const char* args[] = { "--align-victim", NULL };
    int h = proc_create("/bin/sigtest", args);
    if (h < 0) {
        printf("  SKIP: self exec unavailable\n");
        return;
    }

    proc_start(h);
    int status = 0;
    proc_wait(h, &status);
    check("default SIGBUS kills a misaligned exclusive load",
          STLX_WIFSIGNALED(status) && STLX_WTERMSIG(status) == SIGBUS);
#else
    printf("  SKIP: misaligned accesses do not fault on this architecture\n");
#endif
}

/* Functions whose first instruction faults, so a handler can check the reported address */
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl raise_illegal_instruction\n"
    "raise_illegal_instruction:\n"
#if defined(__x86_64__)
    "ud2\n"
#elif defined(__aarch64__)
    "udf #0\n"
#endif
    "ret\n"
    ".p2align 2\n"
    ".globl raise_breakpoint\n"
    "raise_breakpoint:\n"
#if defined(__x86_64__)
    "int3\n"
#elif defined(__aarch64__)
    "brk #0\n"
#endif
    "ret\n"
);

void raise_illegal_instruction(void);
void raise_breakpoint(void);

#if defined(__x86_64__)
#define ILLEGAL_INSN_SIZE      2
#define BREAKPOINT_RESUME_SKIP 0 /* int3 already reports the next instruction */
#define RFLAGS_TRAP               (1u << 8)
#define MXCSR_DIVIDE_BY_ZERO_FLAG (1u << 2)
#define MXCSR_DIVIDE_BY_ZERO_MASK (1u << 9)
#elif defined(__aarch64__)
#define ILLEGAL_INSN_SIZE      4
#define BREAKPOINT_RESUME_SKIP 4 /* brk reports itself */
#endif

static sigjmp_buf fault_escape;
static volatile sig_atomic_t fault_count = 0;
static volatile sig_atomic_t fault_code = 0;
static void* volatile fault_address = NULL;

static void reset_fault_record(void) {
    fault_count = 0;
    fault_code = 0;
    fault_address = NULL;
}

static void record_fault(const siginfo_t* info) {
    fault_count++;
    fault_code = info->si_code;
    fault_address = info->si_addr;
}

static void advance_pc(void* ctx, uintptr_t bytes) {
    ucontext_t* uc = ctx;
#if defined(__x86_64__)
    uc->uc_mcontext.gregs[REG_RIP] += bytes;
#elif defined(__aarch64__)
    uc->uc_mcontext.pc += bytes;
#endif
}

/* Records the fault and escapes to the test, the way capability probes do */
static void escape_fault(int sig, siginfo_t* info, void* ctx) {
    (void)sig;
    (void)ctx;
    record_fault(info);
    siglongjmp(fault_escape, 1);
}

static void skip_illegal_instruction(int sig, siginfo_t* info, void* ctx) {
    (void)sig;
    record_fault(info);
    advance_pc(ctx, ILLEGAL_INSN_SIZE);
}

static void resume_after_breakpoint(int sig, siginfo_t* info, void* ctx) {
    (void)sig;
    record_fault(info);
    advance_pc(ctx, BREAKPOINT_RESUME_SKIP);
}

static void install_fault_handler(int sig, void (*handler)(int, siginfo_t*, void*), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | flags;
    sigaction(sig, &sa, NULL);
}

static void* map_unmapped_page(void) {
    void* page = mmap(NULL, FAULT_PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        return NULL;
    }

    munmap(page, FAULT_PAGE_SIZE);
    return page;
}

static void refault_handler(int sig, siginfo_t* info, void* ctx) {
    (void)sig;
    (void)ctx;
    *(volatile char*)info->si_addr = 1;
}

/* Installed where the handler must never run, so running it fails the scenario */
static void exit_if_handled(int sig, siginfo_t* info, void* ctx) {
    (void)sig;
    (void)info;
    (void)ctx;
    _exit(2);
}

static int recurse_until_overflow(uint64_t depth) {
    volatile char frame[256];
    frame[0] = (char)depth;
    if (depth == UINT64_MAX) {
        return frame[0];
    }

    return recurse_until_overflow(depth + 1) + frame[0];
}

/* Child mode: each scenario must die from its fault although a handler is installed */
static int fault_victim_child(const char* scenario) {
    if (strcmp(scenario, "blocked") == 0) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGILL);
        install_fault_handler(SIGILL, skip_illegal_instruction, 0);
        sigprocmask(SIG_BLOCK, &set, NULL);
        raise_illegal_instruction();
    } else if (strcmp(scenario, "ignored") == 0) {
        signal(SIGILL, SIG_IGN);
        raise_illegal_instruction();
    } else if (strcmp(scenario, "resethand") == 0) {
        install_fault_handler(SIGILL, skip_illegal_instruction, SA_RESETHAND);
        raise_illegal_instruction();
        raise_illegal_instruction();
    } else if (strcmp(scenario, "refault") == 0) {
        volatile char* unmapped = map_unmapped_page();
        install_fault_handler(SIGSEGV, refault_handler, 0);
        *unmapped = 1;
    } else if (strcmp(scenario, "overflow") == 0) {
        install_fault_handler(SIGSEGV, exit_if_handled, 0);
        recurse_until_overflow(0);
    }

    return 1; /* only reached if the fault never killed */
}

static int run_fault_victim(const char* scenario) {
    const char* args[] = { "--fault-victim", scenario, NULL };
    int h = proc_create("/bin/sigtest", args);
    if (h < 0) {
        return -1;
    }

    proc_start(h);
    int status = 0;
    proc_wait(h, &status);
    return status;
}

static int died_from(int status, int sig) {
    return status >= 0 && STLX_WIFSIGNALED(status) && STLX_WTERMSIG(status) == sig;
}

/* An illegal instruction reaches its handler, which escapes the way capability probes do */
static void test_illegal_instruction_reaches_handler(void) {
    install_fault_handler(SIGILL, escape_fault, 0);
    reset_fault_record();

    if (sigsetjmp(fault_escape, 1) == 0) {
        raise_illegal_instruction();
    }

    check("SIGILL handler ran for an illegal instruction", fault_count == 1);
    check("SIGILL reports ILL_ILLOPC", fault_code == ILL_ILLOPC);
    check("SIGILL reports the faulting instruction",
          fault_address == (void*)(uintptr_t)raise_illegal_instruction);

    /* siglongjmp restored the mask, so the next probe is caught as well */
    if (sigsetjmp(fault_escape, 1) == 0) {
        raise_illegal_instruction();
    }

    check("a second illegal instruction is caught too", fault_count == 2);
}

/* A handler that steps past the instruction resumes the program after it */
static void test_handler_resumes_past_illegal_instruction(void) {
    install_fault_handler(SIGILL, skip_illegal_instruction, 0);
    reset_fault_record();

    raise_illegal_instruction();

    check("execution resumes after the skipped instruction", fault_count == 1);
}

static void test_unmapped_access_reports_maperr(void) {
    volatile char* unmapped = map_unmapped_page();
    if (!unmapped) {
        printf("  SKIP: mmap unavailable\n");
        return;
    }

    install_fault_handler(SIGSEGV, escape_fault, 0);
    reset_fault_record();

    if (sigsetjmp(fault_escape, 1) == 0) {
        unmapped[8] = 1;
    }

    check("SIGSEGV handler ran for an unmapped address", fault_count == 1);
    check("SIGSEGV reports SEGV_MAPERR", fault_code == SEGV_MAPERR);
    check("SIGSEGV reports the faulting address", fault_address == (void*)(unmapped + 8));
}

static char* volatile read_only_page = NULL;

static void grant_write_access(int sig, siginfo_t* info, void* ctx) {
    (void)sig;
    (void)ctx;
    record_fault(info);
    mprotect(read_only_page, FAULT_PAGE_SIZE, PROT_READ | PROT_WRITE);
}

/* A write to a read-only page reports SEGV_ACCERR, and completes once the handler grants access */
static void test_read_only_write_reports_accerr(void) {
    void* page = mmap(NULL, FAULT_PAGE_SIZE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        printf("  SKIP: mmap unavailable\n");
        return;
    }

    read_only_page = page;
    volatile char* target = read_only_page + 16;
    install_fault_handler(SIGSEGV, grant_write_access, 0);
    reset_fault_record();

    *target = 0x5A;

    check("SIGSEGV handler ran for a read-only page", fault_count == 1);
    check("SIGSEGV reports SEGV_ACCERR", fault_code == SEGV_ACCERR);
    check("SIGSEGV reports the written address", fault_address == (void*)target);
    check("the write completes once the handler grants access", *target == 0x5A);
    munmap(page, FAULT_PAGE_SIZE);
}

/* A breakpoint reaches its handler, and the program resumes after it */
static void test_breakpoint_reaches_handler(void) {
    install_fault_handler(SIGTRAP, resume_after_breakpoint, 0);
    reset_fault_record();

    raise_breakpoint();

    check("SIGTRAP handler ran for a breakpoint", fault_count == 1);
    check("SIGTRAP reports TRAP_BRKPT", fault_code == TRAP_BRKPT);
}

static void test_divide_error_reports_intdiv(void) {
#if defined(__x86_64__)
    volatile int numerator = 7;
    volatile int zero = 0;
    volatile int quotient = 0;
    install_fault_handler(SIGFPE, escape_fault, 0);
    reset_fault_record();

    if (sigsetjmp(fault_escape, 1) == 0) {
        quotient = numerator / zero;
    }

    (void)quotient;
    check("SIGFPE handler ran for an integer divide by zero", fault_count == 1);
    check("SIGFPE reports FPE_INTDIV", fault_code == FPE_INTDIV);
#else
    printf("  SKIP: integer division does not fault on this architecture\n");
#endif
}

#if defined(__x86_64__)
static volatile uint32_t handler_mxcsr = 0;

static void escape_simd_fault(int sig, siginfo_t* info, void* ctx) {
    __asm__ volatile("stmxcsr %0" : "=m"(handler_mxcsr));
    escape_fault(sig, info, ctx);
}
#endif

/* An unmasked SSE divide by zero reports FPE_FLTDIV, and its handler runs with it masked again */
static void test_simd_divide_by_zero_reports_fltdiv(void) {
#if defined(__x86_64__)
    uint32_t saved_mxcsr;
    __asm__ volatile("stmxcsr %0" : "=m"(saved_mxcsr));
    uint32_t unmasked_mxcsr = saved_mxcsr & ~MXCSR_DIVIDE_BY_ZERO_MASK;

    volatile double numerator = 1.0;
    volatile double denominator = 0.0;
    volatile double quotient = 0.0;
    uint32_t untrapped_mxcsr = 0;
    install_fault_handler(SIGFPE, escape_simd_fault, 0);
    reset_fault_record();

    if (sigsetjmp(fault_escape, 1) == 0) {
        __asm__ volatile("ldmxcsr %0" :: "m"(unmasked_mxcsr) : "memory");
        quotient = numerator / denominator;
        __asm__ volatile("stmxcsr %0" : "=m"(untrapped_mxcsr) :: "memory");
    }

    __asm__ volatile("ldmxcsr %0" :: "m"(saved_mxcsr) : "memory");
    (void)quotient;

    /* An emulated CPU may only set the flag of an unmasked exception, never trap */
    if (fault_count == 0 && (untrapped_mxcsr & MXCSR_DIVIDE_BY_ZERO_FLAG)) {
        printf("  SKIP: SIMD floating-point exceptions do not trap on this CPU\n");
        return;
    }

    check("SIGFPE handler ran for an SSE divide by zero", fault_count == 1);
    check("SIGFPE reports FPE_FLTDIV", fault_code == FPE_FLTDIV);
    check("the handler runs with divide by zero masked", (handler_mxcsr & MXCSR_DIVIDE_BY_ZERO_MASK) != 0);
#else
    printf("  SKIP: floating point does not trap on this architecture\n");
#endif
}

#if defined(__x86_64__)
static void stop_single_step(int sig, siginfo_t* info, void* ctx) {
    (void)sig;
    record_fault(info);
    ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_EFL] &= ~RFLAGS_TRAP;
}
#endif

/* Setting the trap flag single-steps one instruction into the handler */
static void test_single_step_reaches_handler(void) {
#if defined(__x86_64__)
    install_fault_handler(SIGTRAP, stop_single_step, 0);
    reset_fault_record();

    /* Steps over the red zone, since pushfq writes below the stack pointer */
    __asm__ volatile("sub $128, %%rsp\n\tpushfq\n\torq %0, (%%rsp)\n\tpopfq\n\tnop\n\tadd $128, %%rsp"
                     :: "i"(RFLAGS_TRAP) : "memory", "cc");

    check("single-step trap reached the handler once", fault_count == 1);
    check("single-step reports TRAP_TRACE", fault_code == TRAP_TRACE);
#else
    printf("  SKIP: user single-step is x86_64 only\n");
#endif
}

/* A misaligned exclusive load and a branch to a misaligned address report BUS_ADRALN */
static void test_alignment_faults_report_adraln(void) {
#if defined(__aarch64__)
    static uint64_t words[4] __attribute__((aligned(16)));
    char* misaligned = (char*)words + MISALIGNED_WORD_OFFSET;
    install_fault_handler(SIGBUS, escape_fault, 0);
    reset_fault_record();

    if (sigsetjmp(fault_escape, 1) == 0) {
        uint32_t value;
        __asm__ volatile("ldxr %w0, [%1]" : "=r"(value) : "r"(misaligned) : "memory");
    }

    check("SIGBUS handler ran for a misaligned exclusive load", fault_count == 1);
    check("SIGBUS reports BUS_ADRALN", fault_code == BUS_ADRALN);
    check("SIGBUS reports the misaligned address", fault_address == (void*)misaligned);

    void (*misaligned_target)(void) = (void (*)(void))((uintptr_t)raise_breakpoint + 2);
    reset_fault_record();
    if (sigsetjmp(fault_escape, 1) == 0) {
        misaligned_target();
    }

    check("a branch to a misaligned address raises SIGBUS",
          fault_count == 1 && fault_code == BUS_ADRALN && fault_address == (void*)(uintptr_t)misaligned_target);
#else
    printf("  SKIP: misaligned accesses do not fault on this architecture\n");
#endif
}

static void* volatile helper_stack = NULL;
static volatile sig_atomic_t handled_on_helper_stack = 0;

static void skip_illegal_on_helper(int sig, siginfo_t* info, void* ctx) {
    char marker;
    uintptr_t here = (uintptr_t)&marker;
    uintptr_t base = (uintptr_t)helper_stack;
    handled_on_helper_stack = here >= base && here < base + HELPER_STACK_SIZE;
    skip_illegal_instruction(sig, info, ctx);
}

static void illegal_instruction_helper(void* arg) {
    (void)arg;
    raise_illegal_instruction();
    _exit(0);
}

/* A fault on a second thread runs the handler on that thread */
static void test_fault_on_thread_runs_its_handler(void) {
    helper_stack = mmap(NULL, HELPER_STACK_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (helper_stack == MAP_FAILED) {
        printf("  SKIP: helper stack unavailable\n");
        return;
    }

    install_fault_handler(SIGILL, skip_illegal_on_helper, 0);
    reset_fault_record();

    int h = proc_create_thread(illegal_instruction_helper, NULL,
                               (char*)helper_stack + HELPER_STACK_SIZE, "fault_helper");
    if (h < 0) {
        printf("  SKIP: thread creation unavailable\n");
        munmap(helper_stack, HELPER_STACK_SIZE);
        return;
    }

    proc_thread_start(h);
    proc_thread_join(h, NULL);

    check("the faulting thread ran the handler", fault_count == 1 && handled_on_helper_stack == 1);
    munmap(helper_stack, HELPER_STACK_SIZE);
}

/* A fault no handler can take kills instead of raising it again forever */
static void test_faults_without_a_runnable_handler_kill(void) {
    check("a blocked SIGILL kills", died_from(run_fault_victim("blocked"), SIGILL));
    check("an ignored SIGILL kills", died_from(run_fault_victim("ignored"), SIGILL));
    check("SA_RESETHAND makes the second fault fatal", died_from(run_fault_victim("resethand"), SIGILL));
    check("a fault inside its own handler kills", died_from(run_fault_victim("refault"), SIGSEGV));
    check("a stack overflow kills although a handler is installed",
          died_from(run_fault_victim("overflow"), SIGSEGV));
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "--pipe-victim") == 0) {
        return pipe_victim_child();
    }

    if (argc >= 2 && strcmp(argv[1], "--trap-victim") == 0) {
        return trap_victim_child();
    }

    if (argc >= 2 && strcmp(argv[1], "--wait-mask-victim") == 0) {
        return wait_mask_victim_child();
    }

    if (argc >= 2 && strcmp(argv[1], "--align-victim") == 0) {
        return align_victim_child();
    }

    if (argc >= 3 && strcmp(argv[1], "--fault-victim") == 0) {
        return fault_victim_child(argv[2]);
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("sigtest: running signal delivery tests\n");

    test_basic_delivery();
    test_siginfo_delivery();
    test_deferred_reentry();
    test_unblock_delivers();
    test_handler_fp_environment();
    test_read_eintr();
    test_read_restart();
    test_poll_eintr_despite_restart();
    test_wait_masks();
    test_wait_mask_fatal_signal();
    test_async_compute_delivery();
    test_sigpipe_dispositions();
    test_trap_default_kills();
    test_alignment_fault_kills();
    test_illegal_instruction_reaches_handler();
    test_handler_resumes_past_illegal_instruction();
    test_unmapped_access_reports_maperr();
    test_read_only_write_reports_accerr();
    test_breakpoint_reaches_handler();
    test_divide_error_reports_intdiv();
    test_simd_divide_by_zero_reports_fltdiv();
    test_single_step_reaches_handler();
    test_alignment_faults_report_adraln();
    test_fault_on_thread_runs_its_handler();
    test_faults_without_a_runnable_handler_kill();

    printf("sigtest: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
