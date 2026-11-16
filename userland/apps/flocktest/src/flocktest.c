#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include <stlx/proc.h>

#define LOCK_PATH        "/tmp/flocktest.lock"
#define SELF_PATH        "/bin/flocktest"
#define NS_PER_US        1000ULL
#define US_PER_SEC       1000000ULL
#define CHILD_GIVEN_FD   3
#define WATCHDOG_SECONDS 60

// Long enough for the main thread to block in flock before the releasing thread signals or unlocks
#define SETTLE_US (200 * 1000)

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

// Exit codes of the child modes, which try to lock the file without waiting
#define CHILD_LOCKED  0
#define CHILD_REFUSED 2
#define CHILD_FAILED  3

// What the releasing thread does while the main thread waits for the lock
struct release_plan {
    int holder_fd;
    int interrupts;
};

static atomic_int passed;
static atomic_int failed;
static _Atomic(const char*) blocking_check;
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

// Names the check about to block, which the watchdog reports if it never returns
static void watch(const char* name) {
    atomic_store(&blocking_check, name);
}

// Ends a run stuck in a blocking check with a FAIL line and the summary, since a lost wakeup would hang it silently
static void* end_hung_run(void* unused) {
    (void)unused;
    sleep(WATCHDOG_SECONDS);

    const char* name = atomic_load(&blocking_check);
    printf("  FAIL: %s (never returned)\n", name ? name : "setup");
    printf("flocktest: %d passed, %d failed\n", atomic_load(&passed), atomic_load(&failed) + 1);
    _exit(1);
}

static void count_usr1(int sig) {
    (void)sig;
    usr1_count++;
}

static uint64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * US_PER_SEC + (uint64_t)ts.tv_nsec / NS_PER_US;
}

static int open_lock_file(void) {
    return open(LOCK_PATH, O_RDWR | O_CREAT, 0644);
}

static void close_lock_files(const int* fds, size_t count) {
    for (size_t i = 0; i < count; i++) {
        close(fds[i]);
    }
}

// Opens the lock file `count` times, each open a description of its own, or leaves none open and returns -1
static int open_lock_files(int* fds, size_t count) {
    for (size_t i = 0; i < count; i++) {
        fds[i] = open_lock_file();
        if (fds[i] < 0) {
            close_lock_files(fds, i);
            return -1;
        }
    }

    return 0;
}

static int try_lock_as_child(int fd) {
    if (fd < 0) {
        return CHILD_FAILED;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        return CHILD_LOCKED;
    }

    return errno == EWOULDBLOCK ? CHILD_REFUSED : CHILD_FAILED;
}

// Runs this program in a child mode, handing it `given_fd` unless negative, and returns its exit code, or -1
static int run_child(const char* mode, int given_fd) {
    const char* args[] = { mode, NULL };
    int child = proc_create(SELF_PATH, args);
    if (child < 0) {
        return -1;
    }

    int given = given_fd < 0 ? 0 : proc_set_handle(child, CHILD_GIVEN_FD, given_fd);
    proc_start(child);

    int status = 0;
    if (proc_wait(child, &status) != 0 || given != 0 || !STLX_WIFEXITED(status)) {
        return -1;
    }

    return STLX_WEXITSTATUS(status);
}

static void expect_child(const char* name, const char* mode, int given_fd, int expected_code) {
    watch(name);
    check(name, run_child(mode, given_fd) == expected_code);
}

static void test_other_processes(void) {
    int locked = open_lock_file();
    if (locked < 0) {
        printf("  SKIP: lock file unavailable\n");
        return;
    }

    check("this process takes the lock", flock(locked, LOCK_EX | LOCK_NB) == 0);
    expect_child("another process opening the file is refused", "--try-own-open", -1, CHILD_REFUSED);
    expect_child("a process given the descriptor shares its lock", "--try-given", locked, CHILD_LOCKED);
    expect_child("the lock outlives the child's copy of the descriptor", "--try-own-open", -1, CHILD_REFUSED);

    flock(locked, LOCK_UN);
    expect_child("another process locks the file once it is free", "--try-own-open", -1, CHILD_LOCKED);

    close(locked);
}

// Sends SIGUSR1 to the process when asked, which the waiting main thread takes since this thread blocks it, then
// releases the lock, a pause before each
static void* release_after_pause(void* arg) {
    struct release_plan* plan = arg;

    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &usr1, NULL);

    if (plan->interrupts) {
        usleep(SETTLE_US);
        kill(getpid(), SIGUSR1);
    }

    usleep(SETTLE_US);
    flock(plan->holder_fd, LOCK_UN);

    return NULL;
}

// Locks the file through one open, then waits to lock it through a second open until another thread releases the
// first. Returns the wait's flock result, or -1 when setup fails, with errno in `out_error`
static int wait_for_held_lock(int interrupts, int* out_error) {
    int fds[2];
    if (open_lock_files(fds, COUNT_OF(fds)) != 0) {
        *out_error = errno;
        return -1;
    }

    int holder = fds[0];
    int waiter = fds[1];
    if (flock(holder, LOCK_EX | LOCK_NB) != 0) {
        *out_error = errno;
        close_lock_files(fds, COUNT_OF(fds));
        return -1;
    }

    struct release_plan plan = { holder, interrupts };
    pthread_t releaser;
    int created = pthread_create(&releaser, NULL, release_after_pause, &plan);
    if (created != 0) {
        *out_error = created;
        close_lock_files(fds, COUNT_OF(fds));
        return -1;
    }

    int result = flock(waiter, LOCK_EX);
    *out_error = errno;

    pthread_join(releaser, NULL);
    close_lock_files(fds, COUNT_OF(fds));

    return result;
}

static void test_blocked_lock_waits_for_the_holder(void) {
    int error = 0;
    uint64_t started_us = monotonic_us();
    watch("a blocked lock waiting for its holder");
    int result = wait_for_held_lock(0, &error);

    check("a blocked lock is granted once the holder releases it", result == 0);
    check("a blocked lock waits for the holder", monotonic_us() - started_us >= SETTLE_US / 2);
}

static void test_signal_interrupts_blocked_lock(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = count_usr1;
    sigaction(SIGUSR1, &action, NULL);
    usr1_count = 0;

    int error = 0;
    watch("a blocked lock waiting for a signal");
    int result = wait_for_held_lock(1, &error);

    check("a signal ends a blocked lock with EINTR", result == -1 && error == EINTR);
    check("the interrupting handler ran once", usr1_count == 1);
}

static void test_restart_resumes_blocked_lock(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = count_usr1;
    action.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &action, NULL);
    usr1_count = 0;

    int error = 0;
    watch("a blocked lock restarted after a signal");
    int result = wait_for_held_lock(1, &error);

    check("SA_RESTART restarts a blocked lock until it is granted", result == 0);
    check("the restarting handler ran once", usr1_count == 1);
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "--try-own-open") == 0) {
        return try_lock_as_child(open_lock_file());
    }

    if (argc >= 2 && strcmp(argv[1], "--try-given") == 0) {
        return try_lock_as_child(CHILD_GIVEN_FD);
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("flocktest: running whole-file lock tests\n");

    // The watchdog starts with every signal blocked, so the SIGUSR1 checks reach the main thread
    sigset_t all_signals;
    sigset_t main_mask;
    sigfillset(&all_signals);
    pthread_sigmask(SIG_BLOCK, &all_signals, &main_mask);
    pthread_t watchdog;
    if (pthread_create(&watchdog, NULL, end_hung_run, NULL) == 0) {
        pthread_detach(watchdog);
    }

    pthread_sigmask(SIG_SETMASK, &main_mask, NULL);

    test_other_processes();
    test_blocked_lock_waits_for_the_holder();
    test_signal_interrupts_blocked_lock();
    test_restart_resumes_blocked_lock();

    unlink(LOCK_PATH);
    printf("flocktest: %d passed, %d failed\n", atomic_load(&passed), atomic_load(&failed));
    return atomic_load(&failed) > 0 ? 1 : 0;
}
