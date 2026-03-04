#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

static int cat_fd(int fd) {
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(1, buf + written, (size_t)(n - written));
            if (w <= 0) return 1;
            written += w;
        }
    }
    return (n < 0) ? 1 : 0;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        return cat_fd(0);
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            printf("cat: %s: no such file\r\n", argv[i]);
            rc = 1;
            continue;
        }
        if (cat_fd(fd) != 0) rc = 1;
        close(fd);
    }
    return rc;
}
