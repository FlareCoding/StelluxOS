#ifndef STLX_PTY_H
#define STLX_PTY_H

/**
 * Create a PTY master/slave pair. On success, *master_fd and *slave_fd
 * are set to the new file descriptors. Returns 0 on success, -1 on
 * failure with errno set.
 */
int pty_create(int* master_fd, int* slave_fd);

#endif /* STLX_PTY_H */
