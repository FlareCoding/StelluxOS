#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_FILES 16

static int write_all(int fd, const char* buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, buf + written, len - written);
        if (w <= 0) return -1;
        written += (size_t)w;
    }
    return 0;
}

int main(int argc, char* argv[]) {
    int append = 0;
    int file_fds[MAX_FILES];
    int nfiles = 0;
    int rc = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            append = 1;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            for (; i < argc && nfiles < MAX_FILES; i++) {
                int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
                int fd = open(argv[i], flags, 0644);
                if (fd < 0) {
                    fprintf(stderr, "tee: %s: cannot open\n", argv[i]);
                    rc = 1;
                } else {
                    file_fds[nfiles++] = fd;
                }
            }
            break;
        } else {
            if (nfiles >= MAX_FILES) {
                fprintf(stderr, "tee: too many files\n");
                rc = 1;
                continue;
            }
            int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
            int fd = open(argv[i], flags, 0644);
            if (fd < 0) {
                fprintf(stderr, "tee: %s: cannot open\n", argv[i]);
                rc = 1;
            } else {
                file_fds[nfiles++] = fd;
            }
        }
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        if (write_all(1, buf, (size_t)n) != 0)
            rc = 1;
        for (int i = 0; i < nfiles; i++) {
            if (write_all(file_fds[i], buf, (size_t)n) != 0)
                rc = 1;
        }
    }
    if (n < 0)
        rc = 1;

    for (int i = 0; i < nfiles; i++)
        close(file_fds[i]);

    return rc;
}
