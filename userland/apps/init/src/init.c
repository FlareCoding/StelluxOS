#define _GNU_SOURCE
#include <stlx/proc.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>

int main(void) {
    int fd0 = open("/dev/console", O_RDWR);
    if (fd0 < 0) {
        return 99;
    }
    open("/dev/console", O_RDWR); // fd 1
    open("/dev/console", O_RDWR); // fd 2

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("init: launching shell...\r\n");
    int shell_handle = proc_exec("/initrd/bin/shell", NULL);
    if (shell_handle < 0) {
        printf("init: failed to create shell (errno=%d)\r\n", errno);
        return 5;
    }

    int shell_exit = -1;
    proc_wait(shell_handle, &shell_exit);
    printf("init: shell exited with code %d\r\n", shell_exit);

    return 0;
}
