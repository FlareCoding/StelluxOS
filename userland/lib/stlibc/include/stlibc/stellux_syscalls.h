#ifndef STLIBC_STELLUX_SYSCALLS_H
#define STLIBC_STELLUX_SYSCALLS_H

#include <stlibc/stlibcdef.h>

// StelluxOS-specific syscalls
#define SYS_PROC_CREATE         706
#define SYS_PROC_WAIT           707
#define SYS_PROC_CLOSE          708
#define SYS_ELEVATE             790

// Error codes
#define ENOSYS      1   // Invalid system call number
#define ENOENT      2   // No such file or directory
#define EIO         5   // I/O error
#define EBADF       9   // Bad file descriptor
#define ENOMEM      12  // Out of memory
#define EACCES      13  // Invalid access
#define EFAULT      14  // Bad address
#define EEXIST      17  // File exists
#define EISDIR      21  // Is a directory
#define EINVAL      22  // Invalid argument
#define EMFILE      24  // Too many open files
#define ENOTTY      25  // Invalid tty
#define ESPIPE      29  // Illegal seek
#define ENOPRIV     72  // Invalid privilege permissions

/**
 * @brief Performs a system call with up to 6 arguments.
 * 
 * It uses inline assembly to perform the actual syscall
 * instruction with the x86-64 calling convention.
 * 
 * @param syscall_number The system call number to invoke
 * @param arg1 First argument to the system call
 * @param arg2 Second argument to the system call
 * @param arg3 Third argument to the system call
 * @param arg4 Fourth argument to the system call
 * @param arg5 Fifth argument to the system call
 * @param arg6 Sixth argument to the system call
 * @return long The return value from the system call (negative for errors)
 */
long syscall(
    uint64_t syscall_number,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
);

// Convenience macros for common syscall patterns
#define syscall0(num) \
    syscall((num), 0, 0, 0, 0, 0, 0)

#define syscall1(num, arg1) \
    syscall((num), (uint64_t)(arg1), 0, 0, 0, 0, 0)

#define syscall2(num, arg1, arg2) \
    syscall((num), (uint64_t)(arg1), (uint64_t)(arg2), 0, 0, 0, 0)

#define syscall3(num, arg1, arg2, arg3) \
    syscall((num), (uint64_t)(arg1), (uint64_t)(arg2), (uint64_t)(arg3), 0, 0, 0)

#define syscall4(num, arg1, arg2, arg3, arg4) \
    syscall((num), (uint64_t)(arg1), (uint64_t)(arg2), (uint64_t)(arg3), (uint64_t)(arg4), 0, 0)

#define syscall5(num, arg1, arg2, arg3, arg4, arg5) \
    syscall((num), (uint64_t)(arg1), (uint64_t)(arg2), (uint64_t)(arg3), (uint64_t)(arg4), (uint64_t)(arg5), 0)

#define syscall6(num, arg1, arg2, arg3, arg4, arg5, arg6) \
    syscall((num), (uint64_t)(arg1), (uint64_t)(arg2), (uint64_t)(arg3), (uint64_t)(arg4), (uint64_t)(arg5), (uint64_t)(arg6))

#endif // STLIBC_STELLUX_SYSCALLS_H