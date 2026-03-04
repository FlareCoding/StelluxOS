#define _GNU_SOURCE
#include <stlx/proc.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>

#ifndef SYS_getdents64
#ifdef __NR_getdents64
#define SYS_getdents64 __NR_getdents64
#else
#error "SYS_getdents64 is not available for this toolchain"
#endif
#endif

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
} __attribute__((packed));

enum {
    DT_UNKNOWN = 0,
    DT_DIR = 4
};

static void format_mode_string(mode_t mode, char out[11]) {
    out[0] = S_ISDIR(mode)  ? 'd' :
             S_ISREG(mode)  ? '-' :
             S_ISSOCK(mode) ? 's' :
             S_ISLNK(mode)  ? 'l' :
             S_ISCHR(mode)  ? 'c' :
             S_ISBLK(mode)  ? 'b' :
             S_ISFIFO(mode) ? 'p' : '?';
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static int list_directory(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("init: open(%s) failed (errno=%d)\r\n", path, errno);
        return -1;
    }

    // Edge-case validation: tiny buffer should fail with EINVAL and not panic.
    char tiny_buf[8];
    long tiny_rc = syscall(SYS_getdents64, fd, tiny_buf, sizeof(tiny_buf));
    if (tiny_rc != -1 || errno != EINVAL) {
        printf("init: expected getdents64 EINVAL for tiny buffer, rc=%ld errno=%d\r\n",
               tiny_rc, errno);
        close(fd);
        return -1;
    }

    close(fd);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("init: reopen(%s) failed (errno=%d)\r\n", path, errno);
        return -1;
    }

    printf("init: listing %s via getdents64\r\n", path);

    char buf[1024];
    for (;;) {
        long nread = syscall(SYS_getdents64, fd, buf, sizeof(buf));
        if (nread == -1) {
            printf("init: getdents64 failed (errno=%d)\r\n", errno);
            close(fd);
            return -1;
        }
        if (nread == 0) {
            break;
        }

        long bpos = 0;
        while (bpos < nread) {
            struct linux_dirent64* d =
                (struct linux_dirent64*)(buf + bpos);

            if (d->d_reclen < (unsigned short)(offsetof(struct linux_dirent64, d_name) + 1) ||
                d->d_reclen > (unsigned short)(nread - bpos)) {
                printf("init: malformed dirent (reclen=%u, remain=%ld)\r\n",
                       d->d_reclen, nread - bpos);
                close(fd);
                return -1;
            }

            char full_path[512];
            int path_len = snprintf(full_path, sizeof(full_path), "%s/%s", path, d->d_name);
            if (path_len < 0 || path_len >= (int)sizeof(full_path)) {
                printf("  ? ? %s (path too long)\r\n", d->d_name);
                bpos += d->d_reclen;
                continue;
            }

            struct stat st;
            char mode_str[11] = "??????????";
            long long size = -1;
            if (stat(full_path, &st) == 0) {
                size = (long long)st.st_size;
                format_mode_string(st.st_mode, mode_str);
            }

            const char* suffix = (d->d_type == DT_DIR) ? "/" : "";
            printf("  %s %8lld %s%s\r\n", mode_str, size, d->d_name, suffix);
            bpos += d->d_reclen;
        }
    }

    close(fd);
    return 0;
}

int main(void) {
    int fd0 = open("/dev/console", O_RDWR);
    if (fd0 < 0) {
        return 99;
    }
    open("/dev/console", O_RDWR); // fd 1
    open("/dev/console", O_RDWR); // fd 2

    setvbuf(stdout, NULL, _IONBF, 0);

    const char* argv[] = { "4", "1000", NULL };
    int handle = proc_create("/initrd/bin/hello", argv);
    if (handle < 0) {
        printf("init: proc_create failed (errno=%d)\r\n", errno);
        return 1;
    }
    printf("init: proc_create returned handle %d\r\n", handle);

    int err = proc_start(handle);
    if (err < 0) {
        printf("init: proc_start failed (errno=%d)\r\n", errno);
        return 2;
    }
    printf("init: proc_start ok\r\n");

    int exit_code = -1;
    err = proc_wait(handle, &exit_code);
    if (err < 0) {
        printf("init: proc_wait failed (errno=%d)\r\n", errno);
        return 3;
    }
    printf("init: child exited with code %d\r\n", exit_code);

    if (list_directory("/") != 0) {
        printf("init: directory listing failed\r\n");
        return 4;
    }
    if (list_directory("/initrd") != 0) {
        printf("init: directory listing failed\r\n");
        return 4;
    }
    if (list_directory("/initrd/bin") != 0) {
        printf("init: directory listing failed\r\n");
        return 4;
    }

    printf("init: directory listing complete\r\n");

    if (list_directory("/dev") != 0) {
        printf("init: /dev listing failed\r\n");
        return 4;
    }

    printf("init: type something and press Enter: ");
    char input[128];
    ssize_t n = read(0, input, sizeof(input) - 1);
    if (n > 0) {
        input[n] = '\0';
        printf("init: you typed: %s", input);
    } else {
        printf("init: read failed (n=%ld, errno=%d)\r\n", (long)n, errno);
    }

    return 0;
}
