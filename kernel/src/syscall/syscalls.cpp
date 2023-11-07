#include "syscalls.h"
#include <process/process.h>
#include <sched/sched.h>
#include <arch/x86/per_cpu_data.h>
#include <kprint.h>

EXTERN_C long __check_current_elevate_status() {
    return static_cast<long>(current->elevated);
}

EXTERN_C long __syscall_handler(
    uint64_t syscallnum,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5
) {
    uint64_t returnVal = 0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    (void)arg5;

    switch (syscallnum) {
    case SYSCALL_SYS_WRITE: {
        // Handle write syscall
        kprint((const char*)arg2);
        break;
    }
    case SYSCALL_SYS_READ: {
        // Handle read syscall
        break;
    }
    case SYSCALL_SYS_ELEVATE: {
        if (current->elevated) {
            kprint("[*] Already elevated\n");
        } else {
            current->elevated = 1;
        }
        break;
    }
    case SYSCALL_SYS_LOWER: {
        if (!current->elevated) {
            kprint("[*] Already lowered\n");
        } else {
            current->elevated = 0;
        }
        break;
    }
    default: {
        kprintError("Unknown syscall number %llu\n", syscallnum);
        returnVal = -ENOSYS;
        break;
    }
    }

    return returnVal;
}

EXTERN_C long __syscall(
    uint64_t syscallNumber,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
) {
    long ret;

    asm volatile(
        "mov %1, %%rax\n"  // syscall number
        "mov %2, %%rdi\n"  // arg1
        "mov %3, %%rsi\n"  // arg2
        "mov %4, %%rdx\n"  // arg3
        "mov %5, %%r10\n"  // arg4
        "mov %6, %%r8\n"   // arg5
        "mov %7, %%r9\n"   // arg6
        "syscall\n"
        "mov %%rax, %0\n"  // Capture return value
        : "=r"(ret)
        : "r"(syscallNumber), "r"(arg1), "r"(arg2), "r"(arg3), "r"(arg4), "r"(arg5), "r"(arg6)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9"
    );

    return ret;
}
