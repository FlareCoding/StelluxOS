#define _GNU_SOURCE
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
#define LONG_SPIN_NS          (200 * NS_PER_MS)
#define SHORT_SPIN_NS         (3 * NS_PER_MS)
#define SLEEP_US              (100 * 1000)
#define SLEEP_CHARGE_LIMIT_NS (5 * NS_PER_MS)
#define TIMEVAL_SLACK_NS      NS_PER_US
#define TEARDOWN_WAIT_US      (100 * 1000)
#define HELPER_STACK_SIZE     (64 * 1024)

struct measurement {
    uint64_t cpu_ns;
    uint64_t wall_ns;
};

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

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_SEC + (uint64_t)ts.tv_nsec;
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

static void spin_for_ns(uint64_t duration_ns) {
    uint64_t deadline = monotonic_ns() + duration_ns;
    while (monotonic_ns() < deadline) {
    }
}

static void sleep_for_us(uint64_t duration_us) {
    usleep((useconds_t)duration_us);
}

// The wall window encloses the CPU window, so exact accounting never charges more than it
static struct measurement measure_thread(void (*work)(uint64_t), uint64_t amount) {
    uint64_t wall_start = monotonic_ns();
    uint64_t cpu_start = rusage_cpu_ns(RUSAGE_THREAD);

    work(amount);

    uint64_t cpu_end = rusage_cpu_ns(RUSAGE_THREAD);
    uint64_t wall_end = monotonic_ns();

    struct measurement m = { cpu_end - cpu_start, wall_end - wall_start };
    return m;
}

static void test_spinning_thread(void) {
    struct measurement m = measure_thread(spin_for_ns, LONG_SPIN_NS);

    check("a spinning thread is charged at most its wall time", m.cpu_ns <= m.wall_ns + TIMEVAL_SLACK_NS);
    check("a spinning thread is charged most of its wall time", m.cpu_ns >= m.wall_ns / 2);
}

static void test_short_run(void) {
    struct measurement m = measure_thread(spin_for_ns, SHORT_SPIN_NS);

    check("a run shorter than a tick is charged at most its wall time", m.cpu_ns <= m.wall_ns + TIMEVAL_SLACK_NS);
    check("a run shorter than a tick is charged most of its wall time", m.cpu_ns >= m.wall_ns / 2);
}

static void test_sleeping_thread(void) {
    struct measurement m = measure_thread(sleep_for_us, SLEEP_US);

    check("a sleeping thread is not charged", m.cpu_ns < SLEEP_CHARGE_LIMIT_NS);
}

static void test_process_total_covers_the_thread(void) {
    uint64_t thread_ns = rusage_cpu_ns(RUSAGE_THREAD);
    uint64_t process_ns = rusage_cpu_ns(RUSAGE_SELF);

    check("the process total covers the calling thread", process_ns + TIMEVAL_SLACK_NS >= thread_ns);
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

    check("the process total keeps an exited thread's time", process_after - process_before >= LONG_SPIN_NS / 2);

    munmap(stack, HELPER_STACK_SIZE);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("cputimetest: running CPU time tests\n");

    test_spinning_thread();
    test_short_run();
    test_sleeping_thread();
    test_process_total_covers_the_thread();
    test_exited_thread_stays_in_process_total();

    printf("cputimetest: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
