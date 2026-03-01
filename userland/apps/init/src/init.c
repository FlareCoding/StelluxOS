#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int run_vma_syscall_demo(void) {
    const size_t page_size = 4096;
    const size_t map_len = page_size * 2;

    uint8_t* region = (uint8_t*)mmap(
        NULL, map_len,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0
    );
    if (region == MAP_FAILED) {
        printf("mmap failed: errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }

    region[0] = 0x2A;
    region[page_size] = 0x55;
    printf("mmap ok: region=%p first=0x%x second=0x%x\n",
           (void*)region, region[0], region[page_size]);

    if (mprotect(region, page_size, PROT_READ) != 0) {
        printf("mprotect RO failed: errno=%d (%s)\n", errno, strerror(errno));
        munmap(region, map_len);
        return 1;
    }
    printf("mprotect RO ok: first page is now read-only (read value=0x%x)\n", region[0]);

    if (mprotect(region, page_size, PROT_READ | PROT_WRITE) != 0) {
        printf("mprotect RW restore failed: errno=%d (%s)\n", errno, strerror(errno));
        munmap(region, map_len);
        return 1;
    }
    region[1] = 0x33;
    printf("mprotect RW restore ok: first page write value=0x%x\n", region[1]);

    if (munmap(region + page_size, page_size) != 0) {
        printf("munmap upper page failed: errno=%d (%s)\n", errno, strerror(errno));
        munmap(region, page_size);
        return 1;
    }
    printf("munmap upper page ok\n");

    if (munmap(region + page_size, page_size) != 0) {
        printf("munmap upper page (second call) failed: errno=%d (%s)\n", errno, strerror(errno));
        munmap(region, page_size);
        return 1;
    }
    printf("munmap upper page second call ok (idempotent)\n");

    if (munmap(region, page_size) != 0) {
        printf("munmap first page failed: errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    printf("munmap first page ok\n");

    return 0;
}

static int run_resource_fd_demo(void) {
    const char* path = "/resource_demo_file";
    const char* msg = "hello from resource fd demo";

    // Example 1: open/write/close + reopen/read/close
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        printf("open(O_CREAT|O_RDWR) failed: errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }

    ssize_t wr = write(fd, msg, strlen(msg));
    if (wr != (ssize_t)strlen(msg)) {
        printf("write failed: wrote=%ld errno=%d (%s)\n",
               (long)wr, errno, strerror(errno));
        close(fd);
        return 1;
    }

    if (close(fd) != 0) {
        printf("close after write failed: errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }

    fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("reopen(O_RDONLY) failed: errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }

    char buf[128] = {};
    ssize_t rd = read(fd, buf, sizeof(buf) - 1);
    if (rd < 0) {
        printf("read failed: errno=%d (%s)\n", errno, strerror(errno));
        close(fd);
        return 1;
    }
    buf[rd] = '\0';
    printf("resource fd example 1 ok: read back \"%s\" (%ld bytes)\n", buf, (long)rd);

    if (close(fd) != 0) {
        printf("close after read failed: errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }

    // Example 2: invalid handle should fail with EBADF
    errno = 0;
    rd = read(-1, buf, 1);
    if (rd != -1 || errno != EBADF) {
        printf("resource fd example 2 failed: read(-1) => %ld errno=%d\n",
               (long)rd, errno);
        return 1;
    }
    printf("resource fd example 2 ok: read(-1) -> EBADF\n");

    return 0;
}

int main(void) {
    printf("hello from userspace!\n");

    int rc_vma = run_vma_syscall_demo();
    printf("VMA syscall demo %s\n", rc_vma == 0 ? "passed" : "failed");

    int rc_fd = run_resource_fd_demo();
    printf("Resource FD demo %s\n", rc_fd == 0 ? "passed" : "failed");

    return (rc_vma == 0 && rc_fd == 0) ? 0 : 1;
}
