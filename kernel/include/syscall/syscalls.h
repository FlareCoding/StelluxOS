#ifndef SYSCALL_H
#define SYSCALL_H
#include <types.h>

#define ENOSYS  1   // Invalid system call number
#define ENOPRIV 2   // Invalid privilege permissions
#define EINVAL  22  // Invalid argument
#define EFAULT  14  // Bad address
#define ENOMEM  12  // Out of memory
#define EACCES  13  // Invalid access
#define ENOTTY	25  // Invalid tty

#define SYSCALL_SYS_READ                0
#define SYSCALL_SYS_WRITE               1
#define SYSCALL_SYS_MMAP                9
#define SYSCALL_SYS_MUNMAP              11
#define SYSCALL_SYS_BRK                 12
#define SYSCALL_SYS_IOCTL               16
#define SYSCALL_SYS_WRITEV              20
#define SYSCALL_SYS_GETPID              39
#define SYSCALL_SYS_EXIT                60
#define SYSCALL_SYS_SET_THREAD_AREA     158
#define SYSCALL_SYS_SET_TID_ADDRESS     218
#define SYSCALL_SYS_EXIT_GROUP          231

#define SYSCALL_SYS_PROC_CREATE 706
#define SYSCALL_SYS_PROC_WAIT   707
#define SYSCALL_SYS_PROC_CLOSE  708

#define SYSCALL_SYS_ELEVATE     790

/**
 * @brief Executes a system call with the specified number and arguments.
 * 
 * @param syscall_number The unique identifier for the system call to be executed.
 * @param arg1 The first argument to pass to the system call.
 * @param arg2 The second argument to pass to the system call.
 * @param arg3 The third argument to pass to the system call.
 * @param arg4 The fourth argument to pass to the system call.
 * @param arg5 The fifth argument to pass to the system call.
 * @param arg6 The sixth argument to pass to the system call.
 * @return int The result of the system call. A non-negative value typically indicates
 *             success or a valid return value, while a negative value signifies an error.
 */
EXTERN_C long syscall(
    uint64_t syscall_number,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
);

// Architecture-specific code for enabling the syscall interface
namespace arch {
/**
 * @brief Enables the architecture-specific syscall interface.
 * 
 * Configures the necessary hardware and system state to allow the use of system calls.
 * This function performs architecture-specific setup, such as configuring Model Specific Registers (MSRs),
 * enabling appropriate CPU flags, and ensuring the syscall handler is properly registered.
 * 
 * @note This function must be called during system initialization to enable user-space processes
 *       to make system calls.
 * 
 * @namespace arch
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void enable_syscall_interface();
} // namespace arch

#endif // SYSCALL_H

