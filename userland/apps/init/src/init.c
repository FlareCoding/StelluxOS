#define _GNU_SOURCE
#define _POSIX_C_SOURCE 199309L
#include <stlx/proc.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

int main(void) {
    int fd0 = open("/dev/console", O_RDWR);
    if (fd0 < 0) {
        return 99;
    }
    open("/dev/console", O_RDWR); // fd 1
    open("/dev/console", O_RDWR); // fd 2

    setvbuf(stdout, NULL, _IONBF, 0);

    // Test networking: run ping before starting DM
    printf("init: testing ping...\r\n");
    {
        const char* ping_args[] = {"10.0.2.2", "2", NULL};
        int ping_handle = proc_create("/initrd/bin/ping", ping_args);
        if (ping_handle >= 0) {
            int ping_err = proc_start(ping_handle);
            if (ping_err >= 0) {
                int ping_status = 0;
                proc_wait(ping_handle, &ping_status);
                printf("init: ping exited with status %d\r\n", STLX_WEXITSTATUS(ping_status));
            } else {
                printf("init: ping start failed\r\n");
            }
        } else {
            printf("init: ping create failed\r\n");
        }
    }

    int dm_handle = proc_exec("/initrd/bin/stlxdm", NULL);
    if (dm_handle >= 0) {
        proc_detach(dm_handle);
        printf("init: stlxdm started\r\n");

        struct timespec dm_delay = { .tv_sec = 0, .tv_nsec = 200000000L };
        nanosleep(&dm_delay, NULL);
    } else {
        printf("init: stlxdm not available, continuing without graphics\r\n");
    }

    struct timespec delay = { .tv_sec = 0, .tv_nsec = 600000000L }; // 600ms

    while (1) {
        int shell_handle = proc_exec("/initrd/bin/shell", NULL);
        if (shell_handle < 0) {
            printf("init: failed to create shell (errno=%d)\r\n", errno);
            nanosleep(&delay, NULL);
            continue;
        }

        int shell_status = 0;
        proc_wait(shell_handle, &shell_status);
        if (STLX_WIFSIGNALED(shell_status)) {
            printf("init: shell killed (signal %d), restarting...\r\n",
                   STLX_WTERMSIG(shell_status));
        } else {
            printf("init: shell exited with code %d, restarting...\r\n",
                   STLX_WEXITSTATUS(shell_status));
        }
        nanosleep(&delay, NULL);
    }

    return 0;
}
