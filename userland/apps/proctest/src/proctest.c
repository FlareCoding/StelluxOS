#define _POSIX_C_SOURCE 199309L
#include <stlx/proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define PROC_KILL_EXPECTED_EXIT 137

static int parse_i32(const char* s, int* out) {
    if (!s || !out) return -1;
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || (end && *end != '\0')) return -1;
    if (v < -2147483648L || v > 2147483647L) return -1;
    *out = (int)v;
    return 0;
}

static void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L
    };
    nanosleep(&ts, NULL);
}

static int run_chain(int depth, int tick_ms) {
    if (depth > 0) {
        char depth_buf[16];
        char tick_buf[16];
        snprintf(depth_buf, sizeof(depth_buf), "%d", depth - 1);
        snprintf(tick_buf, sizeof(tick_buf), "%d", tick_ms);

        const char* child_argv[] = { "chain", depth_buf, tick_buf, NULL };
        int child = proc_create("/initrd/bin/proctest", child_argv);
        if (child < 0) {
            printf("proctest(chain): proc_create failed errno=%d\r\n", errno);
            return 20;
        }
        if (proc_start(child) < 0) {
            printf("proctest(chain): proc_start failed errno=%d\r\n", errno);
            return 21;
        }
        printf("proctest(chain): spawned depth=%d handle=%d\r\n", depth - 1, child);
    }

    while (1) {
        sleep_ms(tick_ms);
    }
}

static int run_kill_recursive_test(void) {
    const char* child_argv[] = { "chain", "2", "50", NULL };
    int child = proc_create("/initrd/bin/proctest", child_argv);
    if (child < 0) {
        printf("proctest(kill): proc_create failed errno=%d\r\n", errno);
        return 30;
    }
    if (proc_start(child) < 0) {
        printf("proctest(kill): proc_start failed errno=%d\r\n", errno);
        return 31;
    }

    sleep_ms(150);

    if (proc_kill(child) < 0) {
        printf("proctest(kill): proc_kill failed errno=%d\r\n", errno);
        return 32;
    }

    int exit_code = -1;
    if (proc_wait(child, &exit_code) < 0) {
        printf("proctest(kill): proc_wait failed errno=%d\r\n", errno);
        return 33;
    }

    printf("proctest(kill): child exit=%d expected=%d\r\n",
           exit_code, PROC_KILL_EXPECTED_EXIT);
    if (exit_code != PROC_KILL_EXPECTED_EXIT) {
        printf("proctest(kill): FAIL\r\n");
        return 34;
    }

    printf("proctest(kill): PASS\r\n");
    return 0;
}

static int run_orphan_exit_test(void) {
    const char* child_argv[] = { "chain", "1", "50", NULL };
    int child = proc_create("/initrd/bin/proctest", child_argv);
    if (child < 0) {
        printf("proctest(orphan-exit): proc_create failed errno=%d\r\n", errno);
        return 40;
    }
    if (proc_start(child) < 0) {
        printf("proctest(orphan-exit): proc_start failed errno=%d\r\n", errno);
        return 41;
    }

    printf("proctest(orphan-exit): started child handle=%d and exiting without wait\r\n", child);
    return 0;
}

static int run_detach_test(void) {
    const char* child_argv[] = { "sleep", "800", NULL };
    int child = proc_create("/initrd/bin/proctest", child_argv);
    if (child < 0) {
        printf("proctest(detach): proc_create failed errno=%d\r\n", errno);
        return 50;
    }
    if (proc_start(child) < 0) {
        printf("proctest(detach): proc_start failed errno=%d\r\n", errno);
        return 51;
    }
    if (proc_detach(child) < 0) {
        printf("proctest(detach): proc_detach failed errno=%d\r\n", errno);
        return 52;
    }

    printf("proctest(detach): detached child successfully\r\n");
    sleep_ms(50);
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc <= 1) {
        return run_kill_recursive_test();
    }

    if (strcmp(argv[1], "chain") == 0) {
        if (argc < 4) {
            printf("usage: proctest chain <depth> <tick_ms>\r\n");
            return 2;
        }
        int depth = 0;
        int tick_ms = 0;
        if (parse_i32(argv[2], &depth) < 0 || parse_i32(argv[3], &tick_ms) < 0) {
            printf("proctest(chain): invalid numeric args\r\n");
            return 3;
        }
        if (depth < 0) depth = 0;
        if (tick_ms <= 0) tick_ms = 50;
        return run_chain(depth, tick_ms);
    }

    if (strcmp(argv[1], "sleep") == 0) {
        if (argc < 3) return 4;
        int ms = 0;
        if (parse_i32(argv[2], &ms) < 0) return 5;
        sleep_ms(ms);
        return 0;
    }

    if (strcmp(argv[1], "kill-recursive") == 0) {
        return run_kill_recursive_test();
    }

    if (strcmp(argv[1], "orphan-exit") == 0) {
        return run_orphan_exit_test();
    }

    if (strcmp(argv[1], "detach") == 0) {
        return run_detach_test();
    }

    printf("usage: proctest [kill-recursive|orphan-exit|detach|chain <d> <ms>|sleep <ms>]\r\n");
    return 1;
}
