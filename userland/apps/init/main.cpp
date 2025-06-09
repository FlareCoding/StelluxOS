// #include <sched/sched.h>
// #include <time/time.h>
// #include <dynpriv/dynpriv.h>
// #include <ipc/shm.h>
// #include <core/klog.h>

#include <stlibc/stlibc.h>

// Simple sprintf implementation for hex numbers
int sprintf_hex(char* buf, const char* format, uint64_t value) {
    const char* hex_chars = "0123456789abcdef";
    int i = 0;
    
    // Write "0x" prefix
    buf[i++] = '0';
    buf[i++] = 'x';
    
    // Convert number to hex (16 digits)
    for (int j = 15; j >= 0; j--) {
        buf[i++] = hex_chars[(value >> (j * 4)) & 0xF];
    }
    
    // Add newline
    buf[i++] = '\n';
    buf[i] = '\0';
    
    return i;
}

int main() {
    // const auto proc_flags =
    //     process_creation_flags::IS_USERLAND     |
    //     process_creation_flags::SCHEDULE_NOW    |
    //     process_creation_flags::ALLOW_ELEVATE;

    // if (!create_process("/initrd/bin/shell", proc_flags)) {
    //     return -1;
    // }

    syscall(SYS_WRITE, 0, (uint64_t)"Hello, World!\n", 13, 0, 0, 0);

    void* ptr = mmap(nullptr, 0x1000 * 10, PROT_READ | PROT_WRITE, 0);
    syscall(SYS_WRITE, 0, (uint64_t)"First mmap returned: ", 20, 0, 0, 0);
    char buf[32];
    int len = sprintf_hex(buf, "0x%llx\n", reinterpret_cast<uintptr_t>(ptr));
    syscall(SYS_WRITE, 0, (uint64_t)buf, len, 0, 0, 0);

    void* ptr2 = mmap(nullptr, 0x1000 * 5, PROT_READ | PROT_WRITE, 0);
    syscall(SYS_WRITE, 0, (uint64_t)"Second mmap returned: ", 21, 0, 0, 0);
    len = sprintf_hex(buf, "0x%llx\n", reinterpret_cast<uintptr_t>(ptr2));
    syscall(SYS_WRITE, 0, (uint64_t)buf, len, 0, 0, 0);

    syscall(SYS_WRITE, 0, (uint64_t)"Unmapping first region...\n", 25, 0, 0, 0);
    munmap(ptr, 0x1000 * 10);

    // Test reallocation of the same size
    void* ptr3 = mmap(nullptr, 0x1000 * 10, PROT_READ | PROT_WRITE, 0);
    syscall(SYS_WRITE, 0, (uint64_t)"Third mmap (reallocation test) returned: ", 38, 0, 0, 0);
    len = sprintf_hex(buf, "0x%llx\n", reinterpret_cast<uintptr_t>(ptr3));
    syscall(SYS_WRITE, 0, (uint64_t)buf, len, 0, 0, 0);

    syscall(SYS_WRITE, 0, (uint64_t)"Unmapping second region...\n", 26, 0, 0, 0);
    munmap(ptr2, 0x1000 * 5);
    syscall(SYS_WRITE, 0, (uint64_t)"Unmapping third region...\n", 26, 0, 0, 0);
    munmap(ptr3, 0x1000 * 10);

    return 0;
}
