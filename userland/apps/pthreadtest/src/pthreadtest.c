/*
 * pthreadtest - verifies the musl pthread ABI over thread only clone.
 *
 * Covers create and join with return values, contended mutexes,
 * condvar broadcast, thread local storage, the shared file table
 * required by POSIX threads, detached threads, and the design
 * contract that fork style clone stays unimplemented.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>

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
    test_fork_contract();

    printf("pthreadtest: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
