/*
 * mmstress - races kernel copies against mapping changes by sibling threads.
 *
 * Copier threads make the kernel copy into and out of shared regions through
 * syscalls, and wait on futex words inside them, while mutator threads unmap,
 * remap, and reprotect the same regions underneath. No thread touches a
 * region from user mode, so every access is a kernel access, and each one
 * must either complete or fail with EFAULT while the kernel keeps running.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#define FUTEX_WAIT 0

#define REGION_COUNT    4
#define REGION_SIZE     (4 * 4096)
#define COPIER_THREADS  4
#define MUTATOR_THREADS 2
#define COPIER_ROUNDS   200

static unsigned char* regions[REGION_COUNT];
static int sink_fd = -1;
static int source_fd = -1;

static atomic_int copies_completed;
static atomic_int copies_faulted;
static atomic_int waits_done;
static atomic_int mutations;
static atomic_int unexpected;
static atomic_int copiers_running;

static void account_copy(const char* what, long rc) {
    if (rc >= 0) {
        atomic_fetch_add(&copies_completed, 1);
        return;
    }

    if (errno == EFAULT) {
        atomic_fetch_add(&copies_faulted, 1);
        return;
    }

    printf("mmstress: unexpected %s result rc=%ld errno=%d\n", what, rc, errno);
    atomic_fetch_add(&unexpected, 1);
}

static void account_wait(long rc) {
    if (rc == 0 || errno == EAGAIN || errno == ETIMEDOUT || errno == EFAULT || errno == EINTR) {
        atomic_fetch_add(&waits_done, 1);
        return;
    }

    printf("mmstress: unexpected futex result rc=%ld errno=%d\n", rc, errno);
    atomic_fetch_add(&unexpected, 1);
}

static void* copier_main(void* arg) {
    unsigned next = (unsigned)(uintptr_t)arg;

    for (int round = 0; round < COPIER_ROUNDS; round++) {
        unsigned char* region = regions[next++ % REGION_COUNT];

        /* kernel writes into the region */
        errno = 0;
        account_copy("getrandom", syscall(SYS_getrandom, region + 64, 256, 0));

        if (source_fd >= 0) {
            errno = 0;
            account_copy("read", read(source_fd, region, REGION_SIZE));
        }

        /* kernel reads out of the region */
        lseek(sink_fd, 0, SEEK_SET);
        errno = 0;
        account_copy("write", write(sink_fd, region, REGION_SIZE));

        /* the futex word is read twice by the kernel, once under a lock */
        struct timespec brief = { 0, 200000 };
        errno = 0;
        account_wait(syscall(SYS_futex, region, FUTEX_WAIT, 0, &brief));
    }

    atomic_fetch_sub(&copiers_running, 1);
    return NULL;
}

static long elapsed_ms(const struct timespec* since) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - since->tv_sec) * 1000 + (now.tv_nsec - since->tv_nsec) / 1000000;
}

static void expect_ok(const char* what, int ok) {
    if (!ok) {
        printf("mmstress: unexpected %s failure errno=%d\n", what, errno);
        atomic_fetch_add(&unexpected, 1);
    }
}

/* Each mutator owns a disjoint set of regions so its calls never collide */
static void* mutator_main(void* arg) {
    unsigned owner = (unsigned)(uintptr_t)arg;
    unsigned next = owner;

    while (atomic_load(&copiers_running) > 0) {
        unsigned char* region = regions[next % REGION_COUNT];
        next += MUTATOR_THREADS;

        expect_ok("munmap", munmap(region, REGION_SIZE) == 0);
        void* again = mmap(region, REGION_SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        expect_ok("mmap fixed", again == region);

        expect_ok("mprotect read", mprotect(region, REGION_SIZE, PROT_READ) == 0);
        sched_yield();
        expect_ok("mprotect none", mprotect(region, REGION_SIZE, PROT_NONE) == 0);
        sched_yield();
        expect_ok("mprotect rw", mprotect(region, REGION_SIZE, PROT_READ | PROT_WRITE) == 0);

        atomic_fetch_add(&mutations, 1);

        /* a short pause keeps the copiers progressing under the mutation storm */
        struct timespec pause = { 0, 1000000 };
        nanosleep(&pause, NULL);
    }

    return NULL;
}

int main(void) {
    printf("mmstress: racing kernel copies against sibling mapping changes\n");

    sink_fd = open("/tmp/mmstress.sink", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sink_fd < 0) {
        sink_fd = open("/mmstress.sink", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (sink_fd < 0) {
        printf("mmstress: FAIL: no writable sink file, errno=%d\n", errno);
        printf("mmstress: done\n");
        return 1;
    }

    source_fd = open("/dev/urandom", O_RDONLY);

    for (int i = 0; i < REGION_COUNT; i++) {
        regions[i] = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (regions[i] == MAP_FAILED) {
            printf("mmstress: FAIL: region mmap failed, errno=%d\n", errno);
            printf("mmstress: done\n");
            return 1;
        }
    }

    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);

    pthread_t copiers[COPIER_THREADS];
    pthread_t mutators[MUTATOR_THREADS];
    atomic_store(&copiers_running, COPIER_THREADS);

    for (uintptr_t i = 0; i < COPIER_THREADS; i++) {
        pthread_create(&copiers[i], NULL, copier_main, (void*)i);
    }
    for (uintptr_t i = 0; i < MUTATOR_THREADS; i++) {
        pthread_create(&mutators[i], NULL, mutator_main, (void*)i);
    }

    for (int i = 0; i < COPIER_THREADS; i++) {
        pthread_join(copiers[i], NULL);
    }
    for (int i = 0; i < MUTATOR_THREADS; i++) {
        pthread_join(mutators[i], NULL);
    }

    int bad = atomic_load(&unexpected);
    printf("mmstress: %d copies completed, %d faulted, %d waits, %d mutations, %d unexpected in %ld ms\n",
           atomic_load(&copies_completed), atomic_load(&copies_faulted),
           atomic_load(&waits_done), atomic_load(&mutations), bad, elapsed_ms(&started));
    printf("mmstress: %s\n", bad == 0 ? "PASS" : "FAIL");
    printf("mmstress: done\n");
    return bad == 0 ? 0 : 1;
}
