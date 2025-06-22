#include <stlibc/stellux_syscalls.h>

long syscall(
    uint64_t syscall_number,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
) {
    long ret;

    // Use inline assembly to perform the syscall
    // x86-64 syscall calling convention:
    // - syscall number in RAX
    // - arguments in RDI, RSI, RDX, R10, R8, R9
    // - return value in RAX
    asm volatile(
        "mov %1, %%rax\n"  // syscall number
        "mov %2, %%rdi\n"  // arg1
        "mov %3, %%rsi\n"  // arg2
        "mov %4, %%rdx\n"  // arg3
        "mov %5, %%r10\n"  // arg4 (note: r10 instead of rcx for syscalls)
        "mov %6, %%r8\n"   // arg5
        "mov %7, %%r9\n"   // arg6
        "syscall\n"        // invoke system call
        "mov %%rax, %0\n"  // capture return value
        : "=r"(ret)        // output: ret variable
        : "r"(syscall_number), "r"(arg1), "r"(arg2), "r"(arg3), 
          "r"(arg4), "r"(arg5), "r"(arg6)  // inputs
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9", "memory"  // clobbered registers
    );

    return ret;
}
