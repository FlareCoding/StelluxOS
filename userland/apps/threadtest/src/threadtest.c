#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <stlx/proc.h>

#define STACK_SIZE (64 * 1024)

#define STRESS_THREADS 16
#define STRESS_ITERS   10000

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

static void* alloc_stack(void) {
    void* mem = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (mem == MAP_FAILED) return NULL;
    return mem;
}

static void* stack_top(void* base) {
    return (char*)base + STACK_SIZE;
}

static void free_stack(void* base) {
    munmap(base, STACK_SIZE);
}

/* ---------- test 1: create_and_join ---------- */

static volatile int g_magic = 0;

static void thread_write_magic(void* arg) {
    (void)arg;
    g_magic = 0xCAFE;
    _exit(0);
}

static void test_create_and_join(void) {
    g_magic = 0;
    void* stk = alloc_stack();
    check("stack allocated", stk != NULL);
    if (!stk) return;

    int h = proc_create_thread(thread_write_magic, NULL, stack_top(stk), "t_magic");
    check("create returns valid handle", h >= 0);
    if (h < 0) { free_stack(stk); return; }

    proc_thread_start(h);
    int status = 0;
    proc_thread_join(h, &status);
    check("magic value written by thread", g_magic == 0xCAFE);
    check("exit status is 0", STLX_WIFEXITED(status) && STLX_WEXITSTATUS(status) == 0);
    free_stack(stk);
}

/* ---------- test 2: create_start_join_sequence ---------- */

static void thread_exit_7(void* arg) {
    (void)arg;
    _exit(7);
}

static void test_create_start_join_sequence(void) {
    void* stk = alloc_stack();
    if (!stk) { check("stack allocated", 0); return; }

    int h = proc_create_thread(thread_exit_7, NULL, stack_top(stk), "t_seq");
    check("create handle >= 0", h >= 0);
    if (h < 0) { free_stack(stk); return; }

    int rc = proc_thread_start(h);
    check("start returns 0", rc == 0);

    int status = 0;
    rc = proc_thread_join(h, &status);
    check("join returns 0", rc == 0);
    check("exit code is 7", STLX_WIFEXITED(status) && STLX_WEXITSTATUS(status) == 7);
    free_stack(stk);
}

/* ---------- test 3: thread_exit_code ---------- */

static void thread_exit_42(void* arg) {
    (void)arg;
    _exit(42);
}

static void test_thread_exit_code(void) {
    void* stk = alloc_stack();
    if (!stk) { check("stack allocated", 0); return; }

    int h = proc_create_thread(thread_exit_42, NULL, stack_top(stk), "t_42");
    if (h < 0) { check("create ok", 0); free_stack(stk); return; }

    proc_thread_start(h);
    int status = 0;
    proc_thread_join(h, &status);
    check("WIFEXITED", STLX_WIFEXITED(status));
    check("WEXITSTATUS == 42", STLX_WEXITSTATUS(status) == 42);
    free_stack(stk);
}

/* ---------- test 4: multiple_threads_join_all ---------- */

#define NUM_THREADS 8
static volatile int g_counter = 0;

static void thread_increment(void* arg) {
    (void)arg;
    __atomic_fetch_add(&g_counter, 1, __ATOMIC_SEQ_CST);
    _exit(0);
}

static void test_multiple_threads_join_all(void) {
    g_counter = 0;
    int handles[NUM_THREADS];
    void* stacks[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        stacks[i] = alloc_stack();
        if (!stacks[i]) { check("stack alloc", 0); return; }
        handles[i] = proc_create_thread(thread_increment, NULL,
                                         stack_top(stacks[i]), "t_inc");
        if (handles[i] < 0) { check("create ok", 0); return; }
        proc_thread_start(handles[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        proc_thread_join(handles[i], NULL);
        free_stack(stacks[i]);
    }

    check("counter == NUM_THREADS", g_counter == NUM_THREADS);
}

/* ---------- test 5: thread_detach ---------- */

static volatile int g_detach_flag = 0;

static void thread_set_flag(void* arg) {
    (void)arg;
    __atomic_store_n(&g_detach_flag, 1, __ATOMIC_SEQ_CST);
    _exit(0);
}

static void test_thread_detach(void) {
    g_detach_flag = 0;
    void* stk = alloc_stack();
    if (!stk) { check("stack allocated", 0); return; }

    int h = proc_create_thread(thread_set_flag, NULL, stack_top(stk), "t_detach");
    if (h < 0) { check("create ok", 0); free_stack(stk); return; }

    proc_thread_start(h);
    proc_thread_detach(h);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000 }; // 200ms
    nanosleep(&ts, NULL);

    check("detached thread set flag", __atomic_load_n(&g_detach_flag, __ATOMIC_SEQ_CST) == 1);
    // stack not freed: detached thread may still be using it; reclaimed at process exit
}

/* ---------- test 6: shared_memory_write ---------- */

static volatile int g_shared_val = 0;

static void thread_write_shared(void* arg) {
    volatile int* ptr = (volatile int*)arg;
    *ptr = 12345;
    _exit(0);
}

static void test_shared_memory_write(void) {
    g_shared_val = 0;
    void* stk = alloc_stack();
    if (!stk) { check("stack allocated", 0); return; }

    int h = proc_create_thread(thread_write_shared, (void*)&g_shared_val,
                                stack_top(stk), "t_shm");
    if (h < 0) { check("create ok", 0); free_stack(stk); return; }

    proc_thread_start(h);
    proc_thread_join(h, NULL);
    check("shared memory value == 12345", g_shared_val == 12345);
    free_stack(stk);
}

/* ---------- test 7: shared_mmap_region ---------- */

static void thread_write_mmap(void* arg) {
    int* page = (int*)arg;
    page[0] = 0xBEEF;
    _exit(0);
}

static void test_shared_mmap_region(void) {
    int* page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) { check("mmap page", 0); return; }
    page[0] = 0;

    void* stk = alloc_stack();
    if (!stk) { check("stack allocated", 0); munmap(page, 4096); return; }

    int h = proc_create_thread(thread_write_mmap, page, stack_top(stk), "t_mmap");
    if (h < 0) { check("create ok", 0); free_stack(stk); munmap(page, 4096); return; }

    proc_thread_start(h);
    proc_thread_join(h, NULL);
    check("mmap page[0] == 0xBEEF", page[0] == 0xBEEF);
    free_stack(stk);
    munmap(page, 4096);
}

/* ---------- test 8: atomic_counter_stress ---------- */

static volatile int g_stress_counter = 0;

static void thread_stress_inc(void* arg) {
    (void)arg;
    for (int i = 0; i < STRESS_ITERS; i++) {
        __atomic_fetch_add(&g_stress_counter, 1, __ATOMIC_SEQ_CST);
    }
    _exit(0);
}

static void test_atomic_counter_stress(void) {
    g_stress_counter = 0;
    int handles[STRESS_THREADS];
    void* stacks[STRESS_THREADS];

    for (int i = 0; i < STRESS_THREADS; i++) {
        stacks[i] = alloc_stack();
        if (!stacks[i]) { check("stack alloc", 0); return; }
        handles[i] = proc_create_thread(thread_stress_inc, NULL,
                                         stack_top(stacks[i]), "t_stress");
        if (handles[i] < 0) { check("create ok", 0); return; }
        proc_thread_start(handles[i]);
    }

    for (int i = 0; i < STRESS_THREADS; i++) {
        proc_thread_join(handles[i], NULL);
        free_stack(stacks[i]);
    }

    int expected = STRESS_THREADS * STRESS_ITERS;
    check("stress counter correct", g_stress_counter == expected);
}

/* ---------- test 9: handle_table_isolation ---------- */

static volatile int g_iso_write_ok = 0;

static void thread_close_stdout(void* arg) {
    (void)arg;
    close(STDOUT_FILENO);
    __atomic_store_n(&g_iso_write_ok, 1, __ATOMIC_SEQ_CST);
    _exit(0);
}

static void test_handle_table_isolation(void) {
    g_iso_write_ok = 0;
    void* stk = alloc_stack();
    if (!stk) { check("stack allocated", 0); return; }

    int h = proc_create_thread(thread_close_stdout, NULL, stack_top(stk), "t_iso");
    if (h < 0) { check("create ok", 0); free_stack(stk); return; }

    proc_thread_start(h);
    proc_thread_join(h, NULL);

    check("thread closed its stdout", __atomic_load_n(&g_iso_write_ok, __ATOMIC_SEQ_CST) == 1);
    int rc = write(STDOUT_FILENO, "", 0);
    check("main stdout still valid", rc >= 0);
    free_stack(stk);
}

/* ---------- test 10: inherited_handles ---------- */

static volatile int g_inherited_ok = 0;

static void thread_read_fd(void* arg) {
    int fd = (int)(long)arg;
    char buf[16];
    int n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        __atomic_store_n(&g_inherited_ok, 1, __ATOMIC_SEQ_CST);
    }
    _exit(0);
}

static void test_inherited_handles(void) {
    g_inherited_ok = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        printf("  SKIP: /dev/urandom not available\n");
        return;
    }

    void* stk = alloc_stack();
    if (!stk) { check("stack allocated", 0); close(fd); return; }

    int h = proc_create_thread(thread_read_fd, (void*)(long)fd,
                                stack_top(stk), "t_inherit");
    if (h < 0) { check("create ok", 0); free_stack(stk); close(fd); return; }

    proc_thread_start(h);
    proc_thread_join(h, NULL);
    check("thread read from inherited fd", __atomic_load_n(&g_inherited_ok, __ATOMIC_SEQ_CST) == 1);
    close(fd);
    free_stack(stk);
}

/* ---------- test 11: nested_thread_create ---------- */

static volatile int g_nested_b_done = 0;
static volatile int g_nested_a_done = 0;

static void thread_nested_b(void* arg) {
    (void)arg;
    __atomic_store_n(&g_nested_b_done, 1, __ATOMIC_SEQ_CST);
    _exit(0);
}

static void thread_nested_a(void* arg) {
    (void)arg;
    void* stk = alloc_stack();
    if (!stk) { _exit(1); }

    int h = proc_create_thread(thread_nested_b, NULL, stack_top(stk), "t_nest_b");
    if (h < 0) { free_stack(stk); _exit(2); }
    proc_thread_start(h);
    proc_thread_join(h, NULL);
    free_stack(stk);

    __atomic_store_n(&g_nested_a_done, 1, __ATOMIC_SEQ_CST);
    _exit(0);
}

static void test_nested_thread_create(void) {
    g_nested_a_done = 0;
    g_nested_b_done = 0;
    void* stk = alloc_stack();
    if (!stk) { check("stack allocated", 0); return; }

    int h = proc_create_thread(thread_nested_a, NULL, stack_top(stk), "t_nest_a");
    if (h < 0) { check("create ok", 0); free_stack(stk); return; }

    proc_thread_start(h);
    int status = 0;
    proc_thread_join(h, &status);
    check("thread A exited cleanly", STLX_WIFEXITED(status) && STLX_WEXITSTATUS(status) == 0);
    check("thread B completed", __atomic_load_n(&g_nested_b_done, __ATOMIC_SEQ_CST) == 1);
    check("thread A completed", __atomic_load_n(&g_nested_a_done, __ATOMIC_SEQ_CST) == 1);
    free_stack(stk);
}

/* ---------- test 12: thread_kill ---------- */

static void thread_spin_forever(void* arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile("" ::: "memory");
    }
}

static void test_thread_kill(void) {
    void* stk = alloc_stack();
    if (!stk) { check("stack allocated", 0); return; }

    int h = proc_create_thread(thread_spin_forever, NULL, stack_top(stk), "t_spin");
    if (h < 0) { check("create ok", 0); free_stack(stk); return; }

    proc_thread_start(h);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; // 50ms
    nanosleep(&ts, NULL);

    proc_thread_kill(h);
    int status = 0;
    proc_thread_join(h, &status);
    check("killed thread is signaled", STLX_WIFSIGNALED(status));
    free_stack(stk);
}

/* ---------- test 13: leader_exit_kills_threads ---------- */

static void test_leader_exit_kills_threads(void) {
    int h = proc_exec("/bin/threadtest", (const char*[]){ "--child-leader-exit", NULL });
    if (h < 0) {
        printf("  SKIP: could not spawn child process\n");
        return;
    }

    int status = 0;
    proc_wait(h, &status);
    check("child process exited", STLX_WIFEXITED(status));
}

/* ---------- test 14: kill_sleeping_thread ---------- */

static void thread_sleep_long(void* arg) {
    (void)arg;
    struct timespec ts = { .tv_sec = 10, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
    _exit(0);
}

static void test_kill_sleeping_thread(void) {
    void* stk = alloc_stack();
    if (!stk) { check("stack allocated", 0); return; }

    int h = proc_create_thread(thread_sleep_long, NULL, stack_top(stk), "t_sleeper");
    if (h < 0) { check("create ok", 0); free_stack(stk); return; }

    proc_thread_start(h);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; // 100ms
    nanosleep(&ts, NULL);

    proc_thread_kill(h);
    int status = 0;
    proc_thread_join(h, &status);
    check("sleeping thread was killed", STLX_WIFSIGNALED(status));
    free_stack(stk);
}

/* ---------- test 15: create_null_name ---------- */

static void test_create_null_name(void) {
    void* stk = alloc_stack();
    if (!stk) { check("stack allocated", 0); return; }

    int h = proc_create_thread(thread_exit_7, NULL, stack_top(stk), NULL);
    check("null name still succeeds (empty name)", h >= 0);
    if (h >= 0) {
        proc_thread_start(h);
        proc_thread_join(h, NULL);
    }
    free_stack(stk);
}

/* ---------- test 16: create_zero_stack ---------- */

static void test_create_zero_stack(void) {
    int h = proc_create_thread(thread_exit_7, NULL, NULL, "t_nostack");
    check("zero stack returns error", h < 0);
}

/* ---------- test 17: join_after_detach ---------- */

static void test_join_after_detach(void) {
    void* stk = alloc_stack();
    if (!stk) { check("stack allocated", 0); return; }

    int h = proc_create_thread(thread_set_flag, NULL, stack_top(stk), "t_jad");
    if (h < 0) { check("create ok", 0); free_stack(stk); return; }

    proc_thread_start(h);
    proc_thread_detach(h);

    int status = 0;
    int rc = proc_thread_join(h, &status);
    check("join after detach returns error", rc < 0);

    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000 };
    nanosleep(&ts, NULL);
    // stack not freed: detached thread may still be using it; reclaimed at process exit
}

/* ---------- child process mode for leader_exit_kills_threads ---------- */

static void run_child_leader_exit(void) {
    void* stk = alloc_stack();
    if (!stk) _exit(99);

    int h = proc_create_thread(thread_spin_forever, NULL, stack_top(stk), "t_orphan");
    if (h < 0) _exit(98);
    proc_thread_start(h);
    proc_thread_detach(h);
    _exit(0);
}

/* ---------- main ---------- */

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc > 1 && strcmp(argv[1], "--child-leader-exit") == 0) {
        run_child_leader_exit();
        return 0;
    }

    printf("threadtest: Stellux thread model test suite\n\n");

    printf("[basic lifecycle]\n");
    test_create_and_join();
    test_create_start_join_sequence();
    test_thread_exit_code();
    test_multiple_threads_join_all();
    test_thread_detach();

    printf("\n[shared address space]\n");
    test_shared_memory_write();
    test_shared_mmap_region();
    test_atomic_counter_stress();

    printf("\n[handle table isolation]\n");
    test_handle_table_isolation();
    test_inherited_handles();

    printf("\n[thread-spawns-thread]\n");
    test_nested_thread_create();

    printf("\n[kill and termination]\n");
    test_thread_kill();
    test_leader_exit_kills_threads();
    test_kill_sleeping_thread();

    printf("\n[error cases]\n");
    test_create_null_name();
    test_create_zero_stack();
    test_join_after_detach();

    printf("\n--- Results: %d passed, %d failed ---\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
