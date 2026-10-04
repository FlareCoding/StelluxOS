/*
 * pthreadtest - verifies the musl pthread ABI over thread only clone.
 *
 * Covers create and join with return values, contended mutexes,
 * condvar broadcast, thread local storage, the shared file table
 * required by POSIX threads, detached threads, FUTEX_WAKE_OP across
 * threads, and the design contract that fork style clone stays
 * unimplemented.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>

static int passed = 0;
static int failed = 0;

static void check(const char* what, int ok) {
    if (ok) {
        passed++;
        printf("  PASS: %s\n", what);
    } else {
        failed++;
        printf("  FAIL: %s\n", what);
    }
}

/* --- create and join with return values --- */

static void* return_value_worker(void* arg) {
    return (void*)((long)arg * 7);
}

static void test_create_join(void) {
    pthread_t th[4];

    for (long i = 0; i < 4; i++) {
        int rc = pthread_create(&th[i], NULL, return_value_worker, (void*)i);

        if (rc != 0) {
            check("pthread_create succeeds", 0);
            return;
        }
    }

    int values_ok = 1;

    for (long i = 0; i < 4; i++) {
        void* ret = NULL;

        if (pthread_join(th[i], &ret) != 0 || (long)ret != i * 7) {
            values_ok = 0;
        }
    }

    check("join returns each worker's value", values_ok);
}

/* --- contended mutex counter --- */

#define MUTEX_THREADS 4
#define MUTEX_ITERS   50000

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static long counter = 0;

static void* counter_worker(void* arg) {
    (void)arg;

    for (int i = 0; i < MUTEX_ITERS; i++) {
        pthread_mutex_lock(&counter_lock);
        counter++;
        pthread_mutex_unlock(&counter_lock);
    }

    return NULL;
}

static void test_mutex_contention(void) {
    pthread_t th[MUTEX_THREADS];

    for (int i = 0; i < MUTEX_THREADS; i++) {
        pthread_create(&th[i], NULL, counter_worker, NULL);
    }

    for (int i = 0; i < MUTEX_THREADS; i++) {
        pthread_join(th[i], NULL);
    }

    check("contended mutex counter is exact",
          counter == (long)MUTEX_THREADS * MUTEX_ITERS);
}

/* --- condvar broadcast wakes every waiter --- */

#define WAITERS 3

static pthread_mutex_t cv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv      = PTHREAD_COND_INITIALIZER;
static int cv_go = 0;
static int cv_woken = 0;

static void* cv_waiter(void* arg) {
    (void)arg;

    pthread_mutex_lock(&cv_lock);

    while (!cv_go) {
        pthread_cond_wait(&cv, &cv_lock);
    }

    cv_woken++;
    pthread_mutex_unlock(&cv_lock);

    return NULL;
}

static void test_cond_broadcast(void) {
    pthread_t th[WAITERS];

    for (int i = 0; i < WAITERS; i++) {
        pthread_create(&th[i], NULL, cv_waiter, NULL);
    }

    /* Let every waiter reach the wait before broadcasting */
    struct timespec ts = { 0, 100 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    pthread_mutex_lock(&cv_lock);
    cv_go = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&cv_lock);

    for (int i = 0; i < WAITERS; i++) {
        pthread_join(th[i], NULL);
    }

    check("broadcast wakes every waiter", cv_woken == WAITERS);
}

/* --- thread local storage isolation --- */

static __thread long tls_slot = 0;
static long tls_seen[3];

static void* tls_worker(void* arg) {
    long id = (long)arg;
    tls_slot = id + 100;

    /* Every worker must still see its own value after yielding */
    struct timespec ts = { 0, 10 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    tls_seen[id] = tls_slot;

    return NULL;
}

static void test_tls_isolation(void) {
    pthread_t th[3];
    tls_slot = 55;

    for (long i = 0; i < 3; i++) {
        pthread_create(&th[i], NULL, tls_worker, (void*)i);
    }

    for (int i = 0; i < 3; i++) {
        pthread_join(th[i], NULL);
    }

    int isolated = (tls_seen[0] == 100 && tls_seen[1] == 101 &&
                    tls_seen[2] == 102 && tls_slot == 55);

    check("thread local slots stay isolated", isolated);
}

/* --- shared file table, the libuv pattern --- */

static int shared_fd = -1;

static void* fd_opener(void* arg) {
    (void)arg;

    int fd = open("/pthreadtest_shared", O_CREAT | O_RDWR, 0644);

    if (fd >= 0) {
        write(fd, "shared", 6);
        lseek(fd, 0, SEEK_SET);
    }

    shared_fd = fd;

    return NULL;
}

static void test_shared_fd_table(void) {
    pthread_t th;

    pthread_create(&th, NULL, fd_opener, NULL);
    pthread_join(th, NULL);

    if (shared_fd < 0) {
        printf("  SKIP: writable fs unavailable\n");
        return;
    }

    /* The fd opened by the worker must be readable from the main
     * thread, this is what libuv's threadpool depends on */
    char buf[8] = {0};
    ssize_t n = read(shared_fd, buf, 6);

    check("fd opened in a worker reads in main",
          n == 6 && strcmp(buf, "shared") == 0);

    close(shared_fd);
}

/* --- detached thread runs to completion --- */

static volatile int detached_ran = 0;

static void* detached_worker(void* arg) {
    (void)arg;
    detached_ran = 1;

    return NULL;
}

static void test_detached(void) {
    pthread_t th;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    pthread_create(&th, &attr, detached_worker, NULL);
    pthread_attr_destroy(&attr);

    for (int i = 0; i < 100 && !detached_ran; i++) {
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    check("detached thread ran", detached_ran == 1);
}

/* --- pthread_exit passes a value --- */

static void* exiting_worker(void* arg) {
    (void)arg;

    pthread_exit((void*)0x5AFE);

    return NULL; /* not reached */
}

static void test_pthread_exit(void) {
    pthread_t th;
    void* ret = NULL;

    pthread_create(&th, NULL, exiting_worker, NULL);
    pthread_join(th, &ret);

    check("pthread_exit value reaches join", (long)ret == 0x5AFE);
}

/* --- FUTEX_WAKE_OP across threads --- */

#define FUTEX_OP_WAIT      0
#define FUTEX_OP_WAKE      1
#define FUTEX_OP_REQUEUE   3
#define FUTEX_OP_WAKE_OP   5
#define FUTEX_PRIVATE_FLAG 128

/* A FUTEX_WAKE_OP update, from the high bits: operation:4 comparison:4 operand:12 comparand:12 */
#define OPERATION_SHIFT         28
#define COMPARISON_SHIFT        24
#define OPERAND_SHIFT           12
#define ARGUMENT_MASK           0xFFFu
#define SHIFT_PAST_WORD         32
#define UPDATE_SET              0
#define UPDATE_ADD              1
#define UPDATE_OR               2
#define UPDATE_UNKNOWN          5
#define UPDATE_OPERAND_IS_SHIFT 8
#define COMPARE_EQUAL           0
#define COMPARE_NOT_EQUAL       1
#define COMPARE_UNKNOWN         6

#define WAIT_TIMEOUT_SEC 5
#define QUEUE_POLL_LIMIT 2000
#define QUEUE_POLL_NSEC  1000000
#define ADD_THREADS      4
#define ADD_ITERS        2000

struct waiter {
    pthread_t thread;
    uint32_t* word;
    uint32_t  expected;
    long      result;
};

static uint32_t add_first = 0;
static uint32_t add_counter = 0;

static long futex(uint32_t* word, int op, uint32_t val, uintptr_t val2, uint32_t* word2, uint32_t val3) {
    return syscall(SYS_futex, word, op | FUTEX_PRIVATE_FLAG, val, val2, word2, val3);
}

static uint32_t encode_update(uint32_t operation, uint32_t operand, uint32_t comparison, uint32_t comparand) {
    return (operation << OPERATION_SHIFT) | (comparison << COMPARISON_SHIFT) |
           ((operand & ARGUMENT_MASK) << OPERAND_SHIFT) | (comparand & ARGUMENT_MASK);
}

static long wake_op(uint32_t* first, uint32_t* second, int nr_wake, int nr_wake2, uint32_t update) {
    return futex(first, FUTEX_OP_WAKE_OP, (uint32_t)nr_wake, (uintptr_t)nr_wake2, second, update);
}

/* Waits while the word holds the expected value, with a timeout so a lost wake fails instead of hanging */
static void* wait_worker(void* arg) {
    struct waiter* w = arg;
    struct timespec timeout = { WAIT_TIMEOUT_SEC, 0 };

    w->result = futex(w->word, FUTEX_OP_WAIT, w->expected, (uintptr_t)&timeout, NULL, 0);

    return NULL;
}

static void start_waiter(struct waiter* w, uint32_t* word, uint32_t expected) {
    w->word = word;
    w->expected = expected;
    w->result = -1;
    pthread_create(&w->thread, NULL, wait_worker, w);
}

/* Requeues a word's waiters onto the word itself, which counts them without waking any */
static int waiters_queued(uint32_t* word, int count) {
    struct timespec pause = { 0, QUEUE_POLL_NSEC };

    for (int poll = 0; poll < QUEUE_POLL_LIMIT; poll++) {
        if (futex(word, FUTEX_OP_REQUEUE, 0, (uintptr_t)count, word, 0) == count) {
            return 1;
        }

        nanosleep(&pause, NULL);
    }

    return 0;
}

static int refused_as_invalid(long rc) {
    return rc == -1 && errno == EINVAL;
}

static void test_wake_op_wakes_both_words(void) {
    uint32_t first = 0;
    uint32_t second = 0;
    struct waiter first_waiter;
    struct waiter second_waiter;

    start_waiter(&first_waiter, &first, 0);
    start_waiter(&second_waiter, &second, 0);
    int queued = waiters_queued(&first, 1) && waiters_queued(&second, 1);

    long woken = wake_op(&first, &second, 1, 1, encode_update(UPDATE_ADD, 1, COMPARE_EQUAL, 0));
    pthread_join(first_waiter.thread, NULL);
    pthread_join(second_waiter.thread, NULL);

    check("wake_op wakes both words when the comparison holds",
          queued && woken == 2 && second == 1 && first_waiter.result == 0 && second_waiter.result == 0);
}

static void test_wake_op_keeps_waiters_when_the_comparison_fails(void) {
    uint32_t first = 0;
    uint32_t second = 5;
    struct waiter second_waiter;

    start_waiter(&second_waiter, &second, 5);
    int queued = waiters_queued(&second, 1);

    long woken = wake_op(&first, &second, 1, 1, encode_update(UPDATE_SET, 7, COMPARE_EQUAL, 0));
    int still_queued = waiters_queued(&second, 1);
    long released = futex(&second, FUTEX_OP_WAKE, 1, 0, NULL, 0);
    pthread_join(second_waiter.thread, NULL);

    check("wake_op leaves the second word's waiters when the comparison fails",
          queued && woken == 0 && second == 7 && still_queued && released == 1 && second_waiter.result == 0);
}

static void test_wake_op_updates_an_untouched_page(void) {
    long page_size = sysconf(_SC_PAGESIZE);
    void* page = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (page == MAP_FAILED) {
        check("wake_op updates a word on a page nothing has touched", 0);
        return;
    }

    uint32_t first = 0;
    uint32_t* untouched = page;
    long woken = wake_op(&first, untouched, 1, 1, encode_update(UPDATE_SET, 9, COMPARE_EQUAL, 0));

    check("wake_op updates a word on a page nothing has touched", woken == 0 && *untouched == 9);
    munmap(page, page_size);
}

static void test_wake_op_refuses_a_read_only_word(void) {
    long page_size = sysconf(_SC_PAGESIZE);
    void* page = mmap(NULL, page_size, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (page == MAP_FAILED) {
        check("wake_op refuses a read-only word and wakes nobody", 0);
        return;
    }

    uint32_t first = 0;
    uint32_t* read_only = page;
    struct waiter first_waiter;

    start_waiter(&first_waiter, &first, 0);
    int queued = waiters_queued(&first, 1);

    long rc = wake_op(&first, read_only, 1, 1, encode_update(UPDATE_SET, 9, COMPARE_EQUAL, 0));
    int error = errno;
    int still_queued = waiters_queued(&first, 1);
    long released = futex(&first, FUTEX_OP_WAKE, 1, 0, NULL, 0);
    pthread_join(first_waiter.thread, NULL);

    check("wake_op refuses a read-only word and wakes nobody",
          queued && rc == -1 && error == EFAULT && *read_only == 0 && still_queued && released == 1 &&
          first_waiter.result == 0);
    munmap(page, page_size);
}

static void test_wake_op_refuses_malformed_arguments(void) {
    uint32_t malformed[] = {
        encode_update(UPDATE_UNKNOWN, 0, COMPARE_EQUAL, 0),
        encode_update(UPDATE_SET, 0, COMPARE_UNKNOWN, 0),
        encode_update(UPDATE_SET | UPDATE_OPERAND_IS_SHIFT, SHIFT_PAST_WORD, COMPARE_EQUAL, 0),
    };

    uint32_t valid = encode_update(UPDATE_SET, 9, COMPARE_EQUAL, 0);
    uint32_t first = 0;
    uint32_t second = 3;
    int refused = 1;

    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        refused = refused && refused_as_invalid(wake_op(&first, &second, 1, 1, malformed[i]));
    }

    refused = refused && refused_as_invalid(wake_op(&first, &second, -1, 1, valid));
    refused = refused && refused_as_invalid(wake_op(&first, &second, 1, -1, valid));

    check("wake_op refuses malformed updates and counts", refused && second == 3);
}

static void* add_worker(void* arg) {
    (void)arg;

    uint32_t add_one = encode_update(UPDATE_ADD, 1, COMPARE_EQUAL, 0);

    for (int i = 0; i < ADD_ITERS; i++) {
        wake_op(&add_first, &add_counter, 1, 1, add_one);
    }

    return NULL;
}

static void test_wake_op_adds_atomically(void) {
    pthread_t th[ADD_THREADS];

    for (int i = 0; i < ADD_THREADS; i++) {
        pthread_create(&th[i], NULL, add_worker, NULL);
    }

    for (int i = 0; i < ADD_THREADS; i++) {
        pthread_join(th[i], NULL);
    }

    check("wake_op adds atomically across threads", add_counter == (uint32_t)ADD_THREADS * ADD_ITERS);
}

static void test_wake_op_wakes_both_halves_of_a_semaphore_word(void) {
    uint32_t halves[2] = { 0, 1 };
    struct waiter low_waiter;
    struct waiter high_waiters[2];

    start_waiter(&low_waiter, &halves[0], 0);
    start_waiter(&high_waiters[0], &halves[1], 1);
    start_waiter(&high_waiters[1], &halves[1], 1);
    int queued = waiters_queued(&halves[0], 1) && waiters_queued(&halves[1], 2);

    long woken = wake_op(&halves[0], &halves[1], 1, INT32_MAX, encode_update(UPDATE_OR, 0, COMPARE_NOT_EQUAL, 0));
    pthread_join(low_waiter.thread, NULL);
    pthread_join(high_waiters[0].thread, NULL);
    pthread_join(high_waiters[1].thread, NULL);

    check("wake_op wakes both halves of a 64-bit semaphore word",
          queued && woken == 3 && halves[1] == 1 && low_waiter.result == 0 && high_waiters[0].result == 0 &&
          high_waiters[1].result == 0);
}

/* --- fork stays unimplemented by design --- */

static void test_fork_contract(void) {
    errno = 0;

    pid_t pid = fork();

    if (pid == 0) {
        /* A working fork would be a design regression */
        _exit(42);
    }

    check("fork fails with ENOSYS", pid == -1 && errno == ENOSYS);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("pthreadtest: running thread ABI tests\n");

    test_create_join();
    test_mutex_contention();
    test_cond_broadcast();
    test_tls_isolation();
    test_shared_fd_table();
    test_detached();
    test_pthread_exit();
    test_wake_op_wakes_both_words();
    test_wake_op_keeps_waiters_when_the_comparison_fails();
    test_wake_op_updates_an_untouched_page();
    test_wake_op_refuses_a_read_only_word();
    test_wake_op_refuses_malformed_arguments();
    test_wake_op_adds_atomically();
    test_wake_op_wakes_both_halves_of_a_semaphore_word();
    test_fork_contract();

    printf("pthreadtest: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
