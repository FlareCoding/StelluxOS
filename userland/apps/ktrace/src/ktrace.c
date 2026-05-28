#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#define KTRACE_DEV "/dev/ktrace"
#define KTRACE_IOCTL_SET_CATEGORIES 0x4b01 // keep in sync with kernel/trace/trace.h
#define KTRACE_IOCTL_RESET          0x4b02

static int do_dump(const char* outpath) {
    int fd = open(KTRACE_DEV, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "ktrace: open %s failed\n", KTRACE_DEV); return 1; }
    int out = outpath ? open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644) : 1;
    if (out < 0) { close(fd); fprintf(stderr, "ktrace: open %s failed\n", outpath); return 1; }
    char buf[8192];
    ssize_t n;
    int rc = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = write(out, buf + w, (size_t)(n - w));
            if (k <= 0) { rc = 1; goto done; }
            w += k;
        }
    }
    if (n < 0) rc = 1;
done:
    if (out != 1) close(out);
    close(fd);
    return rc;
}

static int do_ctl(unsigned long cmd, unsigned long arg) {
    int fd = open(KTRACE_DEV, O_RDWR);
    if (fd < 0) { fprintf(stderr, "ktrace: open %s failed\n", KTRACE_DEV); return 1; }
    int rc = ioctl(fd, cmd, arg);
    close(fd);
    return rc < 0 ? 1 : 0;
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: ktrace --dump [file] | --reset | --enable | --disable | --categories <hex>\n");
        return 1;
    }
    if (!strcmp(argv[1], "--dump"))    return do_dump(argc > 2 ? argv[2] : NULL);
    if (!strcmp(argv[1], "--reset"))   return do_ctl(KTRACE_IOCTL_RESET, 0);
    if (!strcmp(argv[1], "--enable"))  return do_ctl(KTRACE_IOCTL_SET_CATEGORIES, 0xffff);
    if (!strcmp(argv[1], "--disable")) return do_ctl(KTRACE_IOCTL_SET_CATEGORIES, 0);
    if (!strcmp(argv[1], "--categories") && argc > 2)
        return do_ctl(KTRACE_IOCTL_SET_CATEGORIES, strtoul(argv[2], NULL, 0));
    fprintf(stderr, "ktrace: unknown command\n");
    return 1;
}
