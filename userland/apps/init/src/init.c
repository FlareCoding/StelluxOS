#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

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

int main(void) {
    printf("hello from userspace!\n");

    int rc = run_vma_syscall_demo();
    printf("VMA syscall demo %s\n", rc == 0 ? "passed" : "failed");
    return rc;
}
