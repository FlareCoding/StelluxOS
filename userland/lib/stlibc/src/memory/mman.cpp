#include <stlibc/memory/mman.h>
#include <stlibc/system/syscall.h>

extern "C" {

void* mmap(void* addr, size_t length, int prot_flags, int flags, long offset) {
    uint64_t result = syscall(
        SYS_MMAP,
        reinterpret_cast<uint64_t>(addr),
        length,
        prot_flags,
        flags,
        offset
    );
    
    // Convert the 64-bit result to a pointer
    return reinterpret_cast<void*>(result);
}

int munmap(void* addr, size_t length) {
    return syscall(
        SYS_MUNMAP,
        reinterpret_cast<uint64_t>(addr),
        length,
        0,
        0,
        0
    );
}

} // extern "C" 