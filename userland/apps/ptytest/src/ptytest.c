#include <stlx/pty.h>
#include <stlx/proc.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>

#define STLX_TCSETS_RAW 0x5401

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int master_fd, slave_fd;
    if (pty_create(&master_fd, &slave_fd) < 0) {
        printf("ptytest: pty_create failed\n");
        return 1;
    }
    printf("ptytest: created PTY pair (master=%d, slave=%d)\n", master_fd, slave_fd);

    ioctl(slave_fd, STLX_TCSETS_RAW, 0);

    const char* msg = "hello from master";
    ssize_t w = write(master_fd, msg, strlen(msg));
    printf("ptytest: wrote %ld bytes to master\n", (long)w);

    char buf[64] = {};
    ssize_t r = read(slave_fd, buf, sizeof(buf) - 1);
    printf("ptytest: read %ld bytes from slave: \"%s\"\n", (long)r, buf);

    const char* reply = "hello from slave";
    w = write(slave_fd, reply, strlen(reply));
    printf("ptytest: wrote %ld bytes to slave\n", (long)w);

    memset(buf, 0, sizeof(buf));
    r = read(master_fd, buf, sizeof(buf) - 1);
    printf("ptytest: read %ld bytes from master: \"%s\"\n", (long)r, buf);

    // Test proc_set_handle: launch hello with PTY slave as stdio
    int proc = proc_create("/bin/hello", NULL);
    if (proc >= 0) {
        proc_set_handle(proc, 0, slave_fd);
        proc_set_handle(proc, 1, slave_fd);
        proc_set_handle(proc, 2, slave_fd);
        proc_start(proc);

        memset(buf, 0, sizeof(buf));
        r = read(master_fd, buf, sizeof(buf) - 1);
        printf("ptytest: child output via PTY: \"%s\"\n", buf);

        int exit_code = -1;
        proc_wait(proc, &exit_code);
        printf("ptytest: child exited with code %d\n", exit_code);
    }

    close(slave_fd);
    close(master_fd);
    printf("ptytest: all tests passed\n");
    return 0;
}
