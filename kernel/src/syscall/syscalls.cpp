#include "syscalls.h"
#include <process/process.h>
#include <kprint.h>

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
        kprint("SYSCALL_SYS_WRITE called!!\n");
        break;
    }
    case SYSCALL_SYS_READ: {
        // Handle read syscall
        break;
    }
    case SYSCALL_SYS_EXIT: {
        // Handle exit syscall
        break;
    }
    default: {
        kprintError("Unknown syscall number %llu\n", syscallnum);
        returnVal = ENOSYS;
        break;
    }
    }

    return returnVal;
}
