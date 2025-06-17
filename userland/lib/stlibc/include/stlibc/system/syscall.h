#ifndef STLIBC_SYSCALL_H
#define STLIBC_SYSCALL_H

#include <stlibc/stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// System call numbers
enum {
    SYS_WRITE       = 0,
    SYS_READ        = 1,
    SYS_EXIT        = 2,
    SYS_MMAP        = 3,
    SYS_MUNMAP      = 4,
    SYS_GETPID      = 5,
    SYS_PROC_CREATE = 6,
    SYS_PROC_WAIT   = 7,
    SYS_PROC_CLOSE  = 8,
    SYS_ELEVATE     = 90
};

// System call function
long syscall(
    uint64_t syscall_number,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5
);

#ifdef __cplusplus
}
#endif

#endif // STLIBC_SYSCALL_H 