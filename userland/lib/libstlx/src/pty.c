#define _GNU_SOURCE
#include <stlx/pty.h>
#include <stlx/syscall_nums.h>
#include <unistd.h>

int pty_create(int* master_fd, int* slave_fd) {
    int fds[2];
    int rc = (int)syscall(SYS_PTY_CREATE, fds);
    if (rc < 0) return rc;
    *master_fd = fds[0];
    *slave_fd = fds[1];
    return 0;
}
