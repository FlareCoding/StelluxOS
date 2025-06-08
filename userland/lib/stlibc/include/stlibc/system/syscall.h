#ifndef STELLUX_SYSCALL_H
#define STELLUX_SYSCALL_H

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
    SYS_ELEVATE     = 90
};

// System call function
long syscall(
    uint64_t syscall_number,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
);

#ifdef __cplusplus
}
#endif

#endif // STELLUX_SYSCALL_H 