#include <stlibc/system/syscall.h>

extern "C" {

long syscall(
    uint64_t syscall_number,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5
) {    
    // x86_64 syscall convention:
    // rax = syscall number
    // rdi = arg1
    // rsi = arg2
    // rdx = arg3
    // r10 = arg4
    // r8  = arg5
    // syscall instruction
    // rax = return value
    long ret;

    asm volatile(
        "mov %1, %%rax\n"  // syscall number
        "mov %2, %%rdi\n"  // arg1
        "mov %3, %%rsi\n"  // arg2
        "mov %4, %%rdx\n"  // arg3
        "mov %5, %%r10\n"  // arg4
        "mov %6, %%r8\n"   // arg5
        "syscall\n"
        "mov %%rax, %0\n"  // Capture return value
        : "=r"(ret)
        : "r"(syscall_number), "r"(arg1), "r"(arg2), "r"(arg3), "r"(arg4), "r"(arg5)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8"
    );

    return ret;
}

} // extern "C" 
