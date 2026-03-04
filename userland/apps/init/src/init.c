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

    struct timespec delay = { .tv_sec = 0, .tv_nsec = 600000000L }; // 600ms

    while (1) {
        int shell_handle = proc_exec("/initrd/bin/shell", NULL);
        if (shell_handle < 0) {
            printf("init: failed to create shell (errno=%d)\r\n", errno);
            nanosleep(&delay, NULL);
            continue;
        }

        int shell_exit = -1;
        proc_wait(shell_handle, &shell_exit);
        printf("init: shell exited with code %d, restarting...\r\n", shell_exit);
        nanosleep(&delay, NULL);
    }

    return 0;
}
