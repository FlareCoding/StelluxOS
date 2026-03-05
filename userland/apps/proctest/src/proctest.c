#define _POSIX_C_SOURCE 199309L

#include <stlx/proc.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void sleep_ms(int ms) {
    if (ms <= 0) {
        return;
    }

    uint64_t start = monotonic_ms();
    if (start == 0) {
        volatile uint64_t spins = (uint64_t)ms * 500000ULL;
        while (spins-- > 0) {
            __asm__ volatile("" ::: "memory");
        }
        return;
    }

    while ((monotonic_ms() - start) < (uint64_t)ms) {
        __asm__ volatile("" ::: "memory");
    }
}

static int wait_for_code(int handle, int expected_code) {
    int exit_code = -1;
    if (proc_wait(handle, &exit_code) < 0) {
        printf("proctest: proc_wait failed (errno=%d)\r\n", errno);
        return 1;
    }
    if (exit_code != expected_code) {
        printf("proctest: expected exit %d, got %d\r\n", expected_code, exit_code);
        return 1;
    }
    return 0;
}

static int run_kill_sleep(void) {
    const char* argv[] = { "5", NULL };
    int handle = proc_exec("/initrd/bin/sleep", argv);
    if (handle < 0) {
        printf("proctest: kill-sleep spawn failed (errno=%d)\r\n", errno);
        return 1;
    }
    if (proc_kill(handle) < 0) {
        printf("proctest: kill-sleep proc_kill failed (errno=%d)\r\n", errno);
        return 1;
    }
    return wait_for_code(handle, 137);
}

static int run_kill_created(void) {
    const char* argv[] = { "5", NULL };
    int handle = proc_create("/initrd/bin/sleep", argv);
    if (handle < 0) {
        printf("proctest: kill-created spawn failed (errno=%d)\r\n", errno);
        return 1;
    }
    if (proc_kill(handle) < 0) {
        printf("proctest: kill-created proc_kill failed (errno=%d)\r\n", errno);
        return 1;
    }
    return wait_for_code(handle, 137);
}

static int run_reject_transfer(void) {
    int target = proc_create("/initrd/bin/true", NULL);
    int resource = proc_create("/initrd/bin/true", NULL);
    if (target < 0 || resource < 0) {
        printf("proctest: reject-transfer spawn failed (errno=%d)\r\n", errno);
        return 1;
    }

    int rc = proc_set_handle(target, 3, resource);
    int failed_as_expected = (rc < 0);
    if (!failed_as_expected) {
        printf("proctest: proc_set_handle unexpectedly accepted PROCESS handle\r\n");
    }

    if (proc_kill(target) < 0 || wait_for_code(target, 137) != 0) {
        return 1;
    }
    if (proc_kill(resource) < 0 || wait_for_code(resource, 137) != 0) {
        return 1;
    }

    return failed_as_expected ? 0 : 1;
}

static int run_recursive_kill(void) {
    const char* argv[] = { "middle", NULL };
    int handle = proc_exec("/initrd/bin/proctest", argv);
    if (handle < 0) {
        printf("proctest: recursive-kill spawn failed (errno=%d)\r\n", errno);
        return 1;
    }

    printf("proctest: recursive-kill spawned middle\r\n");
    sleep_ms(200);
    printf("proctest: recursive-kill issuing kill\r\n");
    if (proc_kill(handle) < 0) {
        printf("proctest: recursive-kill proc_kill failed (errno=%d)\r\n", errno);
        return 1;
    }
    printf("proctest: recursive-kill waiting for middle\r\n");
    if (wait_for_code(handle, 137) != 0) {
        return 1;
    }
    printf("proctest: recursive-kill middle exited\r\n");

    sleep_ms(2500);
    printf("proctest: recursive-kill window complete\r\n");
    return 0;
}

static int run_detach(void) {
    const char* argv[] = { "detached-child", NULL };
    int handle = proc_exec("/initrd/bin/proctest", argv);
    if (handle < 0) {
        printf("proctest: detach spawn failed (errno=%d)\r\n", errno);
        return 1;
    }

    if (proc_detach(handle) < 0) {
        printf("proctest: detach failed (errno=%d)\r\n", errno);
        return 1;
    }

    sleep_ms(2500);
    printf("proctest: detach window complete\r\n");
    return 0;
}

static int run_parent_exit_helper_wait(void) {
    const char* argv[] = { "parent-exit-helper", NULL };
    int handle = proc_exec("/initrd/bin/proctest", argv);
    if (handle < 0) {
        printf("proctest: parent-exit helper spawn failed (errno=%d)\r\n", errno);
        return 1;
    }

    if (wait_for_code(handle, 0) != 0) {
        return 1;
    }

    sleep_ms(2500);
    printf("proctest: parent-exit helper window complete\r\n");
    return 0;
}

static int mode_middle(void) {
    const char* argv[] = { "leaf", NULL };
    int handle = proc_exec("/initrd/bin/proctest", argv);
    if (handle < 0) {
        printf("proctest: middle failed to spawn leaf (errno=%d)\r\n", errno);
        return 1;
    }

    printf("proctest: middle spawned leaf\r\n");
    sleep_ms(5000);
    printf("proctest: MIDDLE-FINISHED\r\n");

    int exit_code = -1;
    proc_wait(handle, &exit_code);
    return exit_code;
}

static int mode_leaf(void) {
    printf("proctest: leaf sleeping\r\n");
    sleep_ms(2000);
    printf("proctest: LEAF-FINISHED\r\n");
    return 23;
}

static int mode_detached_child(void) {
    sleep_ms(1000);
    printf("proctest: DETACHED-ALIVE\r\n");
    return 23;
}

static int mode_parent_exit_helper(void) {
    const char* argv[] = { "middle", NULL };
    int handle = proc_exec("/initrd/bin/proctest", argv);
    if (handle < 0) {
        printf("proctest: parent-exit helper failed to spawn middle (errno=%d)\r\n", errno);
        return 1;
    }
    printf("proctest: parent-exit helper spawned middle\r\n");
    (void)handle;
    return 0;
}

static int report_case(const char* name, int rc) {
    printf("proctest: %s %s\r\n", name, rc == 0 ? "PASS" : "FAIL");
    return rc;
}

static int mode_selftest(void) {
    int failures = 0;

    failures += report_case("kill-sleep", run_kill_sleep());
    failures += report_case("kill-created", run_kill_created());
    failures += report_case("reject-transfer", run_reject_transfer());
    failures += report_case("recursive-kill", run_recursive_kill());
    failures += report_case("detach", run_detach());
    failures += report_case("parent-exit-helper", run_parent_exit_helper_wait());

    if (failures == 0) {
        printf("proctest: SELFTEST PASS\r\n");
        return 0;
    }

    printf("proctest: SELFTEST FAIL (%d)\r\n", failures);
    return 1;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("proctest: missing mode\r\n");
        return 1;
    }

    if (strcmp(argv[1], "selftest") == 0) {
        return mode_selftest();
    }
    if (strcmp(argv[1], "middle") == 0) {
        return mode_middle();
    }
    if (strcmp(argv[1], "leaf") == 0) {
        return mode_leaf();
    }
    if (strcmp(argv[1], "kill-sleep") == 0) {
        return run_kill_sleep();
    }
    if (strcmp(argv[1], "kill-created") == 0) {
        return run_kill_created();
    }
    if (strcmp(argv[1], "reject-transfer") == 0) {
        return run_reject_transfer();
    }
    if (strcmp(argv[1], "recursive-kill") == 0) {
        return run_recursive_kill();
    }
    if (strcmp(argv[1], "detach") == 0) {
        return run_detach();
    }
    if (strcmp(argv[1], "parent-exit-wait") == 0) {
        return run_parent_exit_helper_wait();
    }
    if (strcmp(argv[1], "detached-child") == 0) {
        return mode_detached_child();
    }
    if (strcmp(argv[1], "parent-exit-helper") == 0) {
        return mode_parent_exit_helper();
    }

    printf("proctest: unknown mode '%s'\r\n", argv[1]);
    return 1;
}
