#include <syscall/syscalls.h>
#include <process/process.h>
#include <serial/serial.h>
#include <dynpriv/dynpriv.h>

EXTERN_C
__PRIVILEGED_CODE
int __syscall_handler(
    uint64_t syscallnum,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5
) {
    int return_val = 0;
    __unused arg1;
    __unused arg2;
    __unused arg3;
    __unused arg4;
    __unused arg5;

    switch (syscallnum) {
    case SYSCALL_SYS_WRITE: {
        // Handle write syscall
        serial::printf((const char*)arg2);
        break;
    }
    case SYSCALL_SYS_READ: {
        // Handle read syscall
        break;
    }
    case SYSCALL_SYS_EXIT: {
        sched::exit_thread();
        break;
    }
    case SYSCALL_SYS_ELEVATE: {
        // Make sure that the thread is allowed to elevate
        if (!dynpriv::is_asid_allowed()) {
            serial::printf("[*] Unauthorized elevation attempt\n");
            return_val = -ENOPRIV;
            break;
        }

        if (current->elevated) {
            serial::printf("[*] Already elevated\n");
        } else {
            current->elevated = 1;
        }
        break;
    }
    default: {
        serial::printf("Unknown syscall number %llu\n", syscallnum);
        return_val = -ENOSYS;
        break;
    }
    }

    return return_val;
}

EXTERN_C
int syscall(
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

    return static_cast<int>(ret);
}

