#ifndef SYSCALLS_H
#define SYSCALLS_H
#include <ktypes.h>

#define ENOSYS 1

#define SYSCALL_SYS_WRITE       0
#define SYSCALL_SYS_READ        1
#define SYSCALL_SYS_EXIT        60

#define SYSCALL_SYS_ELEVATE     91
#define SYSCALL_SYS_LOWER       92

EXTERN_C long __syscall(
    uint64_t syscallNumber,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
);

#endif
