#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <stlx/proc.h>

#define NS_PER_US             1000ULL
#define NS_PER_MS             1000000ULL
#define NS_PER_SEC            1000000000ULL
#define NS_PER_CLOCK_TICK     (NS_PER_SEC / CLOCKS_PER_SEC)
#define LONG_SPIN_NS          (200 * NS_PER_MS)
#define SHORT_SPIN_NS         (3 * NS_PER_MS)
#define SLEEP_US              (100 * 1000)
#define SLEEP_CHARGE_LIMIT_NS (5 * NS_PER_MS)
#define TIMEVAL_SLACK_NS      NS_PER_US
#define TEARDOWN_WAIT_US      (100 * 1000)
#define POLL_US               1000
#define HELPER_STACK_SIZE     (64 * 1024)

// The thread clock id encoding pthread_getcpuclockid uses, for threads of other processes
#define THREAD_CPU_CLOCK_ID(tid) ((clockid_t)((-(tid) - 1) * 8 + 6))

struct measurement {
    uint64_t cpu_ns;
    uint64_t wall_ns;
};

static int passed = 0;
static int failed = 0;
static atomic_int sibling_spun;
static atomic_int sibling_released;

static void check(const char* name, int cond) {
    if (cond) {
        printf("  PASS: %s\n", name);
        passed++;
    } else {
        printf("  FAIL: %s\n", name);
        failed++;
    }
}

static uint64_t clock_ns(clockid_t clock) {
    struct timespec ts;
    if (clock_gettime(clock, &ts) != 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
}

static uint64_t monotonic_ns(void) {
    return clock_ns(CLOCK_MONOTONIC);
}

static uint64_t timeval_ns(struct timeval tv) {
    return (uint64_t)tv.tv_sec * NS_PER_SEC + (uint64_t)tv.tv_usec * NS_PER_US;
}

static uint64_t rusage_cpu_ns(int who) {
    struct rusage usage;
    if (getrusage(who, &usage) != 0) {
        return 0;
    }

    return timeval_ns(usage.ru_utime) + timeval_ns(usage.ru_stime);
}

static uint64_t thread_rusage_ns(void) {
    return rusage_cpu_ns(RUSAGE_THREAD);
}

static uint64_t thread_clock_ns(void) {
    return clock_ns(CLOCK_THREAD_CPUTIME_ID);
}

static void spin_for_ns(uint64_t duration_ns) {
    uint64_t deadline = monotonic_ns() + duration_ns;
    while (monotonic_ns() < deadline) {
    }
}

static void sleep_for_us(uint64_t duration_us) {
    usleep((useconds_t)duration_us);
}

// The wall window encloses the CPU window, so exact accounting never charges more than it
static struct measurement measure_thread(uint64_t (*read_cpu_ns)(void), void (*work)(uint64_t), uint64_t amount) {
    uint64_t wall_start = monotonic_ns();
    uint64_t cpu_start = read_cpu_ns();

    work(amount);

    uint64_t cpu_end = read_cpu_ns();
    uint64_t wall_end = monotonic_ns();

    struct measurement m = { cpu_end - cpu_start, wall_end - wall_start };
    return m;
}

static void test_spinning_thread(void) {
    struct measurement m = measure_thread(thread_rusage_ns, spin_for_ns, LONG_SPIN_NS);

    check("a spinning thread is charged at most its wall time", m.cpu_ns <= m.wall_ns + TIMEVAL_SLACK_NS);
    check("a spinning thread is charged most of its wall time", m.cpu_ns >= m.wall_ns / 2);
}

static void test_short_run(void) {
    struct measurement m = measure_thread(thread_rusage_ns, spin_for_ns, SHORT_SPIN_NS);

    check("a run shorter than a tick is charged at most its wall time", m.cpu_ns <= m.wall_ns + TIMEVAL_SLACK_NS);
    check("a run shorter than a tick is charged most of its wall time", m.cpu_ns >= m.wall_ns / 2);
}

static void test_sleeping_thread(void) {
    struct measurement m = measure_thread(thread_rusage_ns, sleep_for_us, SLEEP_US);

    check("a sleeping thread is not charged", m.cpu_ns < SLEEP_CHARGE_LIMIT_NS);
}

static void test_thread_clock(void) {
    struct measurement run = measure_thread(thread_clock_ns, spin_for_ns, SHORT_SPIN_NS);
    struct measurement sleep = measure_thread(thread_clock_ns, sleep_for_us, SLEEP_US);

    check("the thread clock charges a short run at most its wall time", run.cpu_ns <= run.wall_ns);
    check("the thread clock charges a short run most of its wall time", run.cpu_ns >= run.wall_ns / 2);
    check("the thread clock stands still while the thread sleeps", sleep.cpu_ns < SLEEP_CHARGE_LIMIT_NS);
}

static void test_process_total_covers_the_thread(void) {
    uint64_t thread_ns = rusage_cpu_ns(RUSAGE_THREAD);
    uint64_t process_ns = rusage_cpu_ns(RUSAGE_SELF);
    uint64_t thread_clock = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    uint64_t process_clock = clock_ns(CLOCK_PROCESS_CPUTIME_ID);

    check("the process total covers the calling thread", process_ns + TIMEVAL_SLACK_NS >= thread_ns);
    check("the process clock covers the calling thread", process_clock >= thread_clock);
}

static void spin_then_exit(void* unused) {
    (void)unused;
    spin_for_ns(LONG_SPIN_NS);
    _exit(0);
}

static void test_exited_thread_stays_in_process_total(void) {
    void* stack = mmap(NULL, HELPER_STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) {
        printf("  SKIP: helper stack unavailable\n");
        return;
    }

    uint64_t process_before = rusage_cpu_ns(RUSAGE_SELF);
    uint64_t clock_before = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    int spinner = proc_create_thread(spin_then_exit, NULL, (char*)stack + HELPER_STACK_SIZE, "cpu_spinner");
    if (spinner < 0) {
        printf("  SKIP: thread creation unavailable\n");
        munmap(stack, HELPER_STACK_SIZE);
        return;
    }

    proc_thread_start(spinner);
    proc_thread_join(spinner, NULL);

    // Waiting past the thread's teardown checks that its time outlives its task
    usleep(TEARDOWN_WAIT_US);
    uint64_t process_after = rusage_cpu_ns(RUSAGE_SELF);
    uint64_t clock_after = clock_ns(CLOCK_PROCESS_CPUTIME_ID);

    check("the process total keeps an exited thread's time", process_after - process_before >= LONG_SPIN_NS / 2);
    check("the process clock keeps an exited thread's time", clock_after - clock_before >= LONG_SPIN_NS / 2);

    munmap(stack, HELPER_STACK_SIZE);
}

static void* spin_until_released(void* unused) {
    (void)unused;
    spin_for_ns(LONG_SPIN_NS);
    atomic_store(&sibling_spun, 1);

    while (!atomic_load(&sibling_released)) {
        usleep(POLL_US);
    }

    return NULL;
}

static void test_sibling_thread_clock(void) {
    pthread_t sibling;
    if (pthread_create(&sibling, NULL, spin_until_released, NULL) != 0) {
        printf("  SKIP: thread creation unavailable\n");
        return;
    }

    while (!atomic_load(&sibling_spun)) {
        usleep(POLL_US);
    }

    clockid_t sibling_clock;
    int has_clock = pthread_getcpuclockid(sibling, &sibling_clock) == 0;
    uint64_t sibling_ns = has_clock ? clock_ns(sibling_clock) : 0;

    atomic_store(&sibling_released, 1);
    pthread_join(sibling, NULL);

    check("a sibling thread's clock reads its CPU time", has_clock && sibling_ns >= LONG_SPIN_NS / 2);
}

static void test_process_clock_ids(void) {
    clockid_t own_clock;
    int has_own_clock = clock_getcpuclockid(getpid(), &own_clock) == 0;
    uint64_t own_ns = has_own_clock ? clock_ns(own_clock) : 0;
    uint64_t process_ns = clock_ns(CLOCK_PROCESS_CPUTIME_ID);

    check("this process's clock id reads the process clock", has_own_clock && own_ns > 0 && process_ns >= own_ns);

    process_info child_info;
    int child = proc_create("/bin/true", NULL);
    if (child < 0 || proc_info(child, &child_info) != 0) {
        printf("  SKIP: child process unavailable\n");
        return;
    }

    clockid_t child_clock;
    int has_child_clock = clock_getcpuclockid(child_info.pid, &child_clock) == 0;
    struct timespec child_ts = { 1, 0 };
    int child_read = has_child_clock && clock_gettime(child_clock, &child_ts) == 0;

    struct timespec unused;
    int thread_refused = clock_gettime(THREAD_CPU_CLOCK_ID(child_info.pid), &unused) != 0 && errno == EINVAL;

    proc_start(child);
    proc_wait(child, NULL);

    check("another process's clock id reads its CPU time",
          child_read && child_ts.tv_sec == 0 && child_ts.tv_nsec == 0);
    check("another process's thread clock is refused", thread_refused);
}

static void test_clock_excludes_sleep(void) {
    clock_t before = clock();
    usleep(SLEEP_US);
    clock_t after = clock();

    uint64_t charged_ns = (uint64_t)(after - before) * NS_PER_CLOCK_TICK;

    check("clock() leaves out time spent asleep", charged_ns < SLEEP_CHARGE_LIMIT_NS);
}

static void test_resolution(void) {
    struct timespec thread_res;
    struct timespec process_res;
    int read = clock_getres(CLOCK_THREAD_CPUTIME_ID, &thread_res) == 0 &&
               clock_getres(CLOCK_PROCESS_CPUTIME_ID, &process_res) == 0;

    check("CPU clocks report nanosecond resolution",
          read && thread_res.tv_sec == 0 && thread_res.tv_nsec == 1 &&
          process_res.tv_sec == 0 && process_res.tv_nsec == 1);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("cputimetest: running CPU time tests\n");

    test_spinning_thread();
    test_short_run();
    test_sleeping_thread();
    test_thread_clock();
    test_process_total_covers_the_thread();
    test_exited_thread_stays_in_process_total();
    test_sibling_thread_clock();
    test_process_clock_ids();
    test_clock_excludes_sleep();
    test_resolution();

    printf("cputimetest: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
