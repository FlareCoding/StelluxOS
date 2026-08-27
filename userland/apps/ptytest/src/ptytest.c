#define _GNU_SOURCE
#include <stlx/pty.h>
#include <stlx/proc.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>

#define STLX_TCSETS_RAW 0x7301

static void catch_int(int sig) {
    (void)sig;
    char marker = 'C';
    write(STDOUT_FILENO, &marker, 1);
    _exit(42);
}

/* Child mode for the ISIG test: report readiness, then block on stdin
 * until the terminal's ^C delivers SIGINT into the handler. */
static int catch_int_child(void) {
    signal(SIGINT, catch_int);
    write(STDOUT_FILENO, "R", 1);
    char b;
    read(STDIN_FILENO, &b, 1);
    return 1;
}

/* Spawn victim on the slave as its foreground group and press ^C */
static int isig_run_victim(const char* path, const char** args,
                           int wait_ready, int* status) {
    int master_fd, slave_fd;
    if (pty_create(&master_fd, &slave_fd) < 0) {
        printf("ptytest: isig pty_create failed\n");
        return -1;
    }

    int proc = proc_create(path, args);
    if (proc < 0) {
        printf("ptytest: isig proc_create failed\n");
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    proc_set_handle(proc, 0, slave_fd);
    proc_set_handle(proc, 1, slave_fd);
    proc_set_handle(proc, 2, slave_fd);

    process_info info;
    if (proc_info(proc, &info) != 0 ||
        setpgid(info.pid, info.pid) != 0 ||
        tcsetpgrp(master_fd, info.pid) != 0) {
        printf("ptytest: isig foreground setup failed\n");
        proc_detach(proc);
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    proc_start(proc);

    /* Wait for the readiness marker, or give the victim time to block */
    if (wait_ready) {
        char b = 0;
        while (read(master_fd, &b, 1) == 1 && b != 'R') {}
    } else {
        usleep(200 * 1000);
    }

    char intr = 0x03;
    write(master_fd, &intr, 1);

    proc_wait(proc, status);
    close(master_fd);
    close(slave_fd);
    return 0;
}

static int isig_test(void) {
    /* Default disposition: ^C must kill the foreground child */
    static const char* sleep_args[] = { "60", NULL };
    int status = 0;
    if (isig_run_victim("/bin/sleep", sleep_args, 0, &status) != 0) {
        return -1;
    }

    if (!STLX_WIFSIGNALED(status) || STLX_WTERMSIG(status) != SIGINT) {
        printf("ptytest: ISIG kill FAILED (status=%d)\n", status);
        return -1;
    }

    printf("ptytest: ISIG ^C killed foreground child\n");

    /* Installed handler: ^C must run it instead of killing */
    static const char* catch_args[] = { "--catch-int", NULL };
    status = 0;
    if (isig_run_victim("/bin/ptytest", catch_args, 1, &status) != 0) {
        return -1;
    }

    if (!STLX_WIFEXITED(status) || STLX_WEXITSTATUS(status) != 42) {
        printf("ptytest: ISIG handler FAILED (status=%d)\n", status);
        return -1;
    }

    printf("ptytest: ISIG ^C ran the child's handler\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "--catch-int") == 0) {
        return catch_int_child();
    }

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

    if (isig_test() != 0) {
        return 1;
    }

    printf("ptytest: all tests passed\n");
    return 0;
}
