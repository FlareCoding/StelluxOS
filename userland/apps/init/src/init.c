#include <stdio.h>
#include <sys/mman.h>

static int run_vma_syscall_demo(void) {
    const size_t page_size = 4096;
    const size_t map_len = page_size * 2;

    unsigned char* region = (unsigned char*)mmap(
        NULL, map_len,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0
    );
    if (region == MAP_FAILED) {
        printf("mmap failed\n");
        return 1;
    }

    region[0] = 0x2A;
    region[page_size] = 0x55;
    if (region[0] != 0x2A || region[page_size] != 0x55) {
        printf("mmap value check failed\n");
        munmap(region, map_len);
        return 1;
    }
    printf("mmap values: first=0x2a second=0x55\n");

    if (mprotect(region, page_size, PROT_READ) != 0) {
        printf("mprotect RO failed\n");
        munmap(region, map_len);
        return 1;
    }
    if (region[0] != 0x2A) {
        printf("mprotect RO value check failed\n");
        munmap(region, map_len);
        return 1;
    }
    printf("mprotect RO value: 0x2a\n");

    if (mprotect(region, page_size, PROT_READ | PROT_WRITE) != 0) {
        printf("mprotect RW restore failed\n");
        munmap(region, map_len);
        return 1;
    }
    region[1] = 0x33;
    if (region[1] != 0x33) {
        printf("mprotect RW value check failed\n");
        munmap(region, map_len);
        return 1;
    }
    printf("mprotect RW value: 0x33\n");

    if (munmap(region + page_size, page_size) != 0) {
        printf("munmap upper page failed\n");
        munmap(region, page_size);
        return 1;
    }
    printf("munmap upper page ok\n");

    if (munmap(region + page_size, page_size) != 0) {
        printf("munmap upper page second call failed\n");
        munmap(region, page_size);
        return 1;
    }
    printf("munmap upper page second call ok (idempotent)\n");

    if (munmap(region, page_size) != 0) {
        printf("munmap first page failed\n");
        return 1;
    }
    printf("munmap first page ok\n");

    return 0;
}

int main(void) {
    printf("hello from userspace!\n");

    int rc = run_vma_syscall_demo();
    if (rc == 0) {
        printf("VMA syscall demo passed\n");
    } else {
        printf("VMA syscall demo failed\n");
    }
    return rc;
}
