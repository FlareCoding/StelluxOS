#include <stlibc/system/syscall.h>

extern "C" {

long syscall(
    uint64_t syscall_number,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
) {
    int ret;
    
    // x86_64 syscall convention:
    // rax = syscall number
    // rdi = arg1
    // rsi = arg2
    // rdx = arg3
    // r10 = arg4
    // r8  = arg5
    // r9  = arg6
    // syscall instruction
    // rax = return value
    asm volatile(
        "syscall"
        : "=a" (ret)
        : "a" (syscall_number),
          "D" (arg1),
          "S" (arg2),
          "d" (arg3),
          "r" (arg4),
          "r" (arg5),
          "r" (arg6)
        : "rcx", "r11", "memory"
    );
    
    return ret;
}

} // extern "C" 
