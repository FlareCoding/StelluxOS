#ifndef SYSCALL_H
#define SYSCALL_H
#include <types.h>

#define EPERM           1   // Operation not permitted
#define ENOENT          2   // No such file or directory
#define EIO             5   // I/O error
#define EBADF           9   // Bad file descriptor
#define EAGAIN          11  // Try again (resource temporarily unavailable)
#define EWOULDBLOCK     EAGAIN  // Operation would block (same as EAGAIN)
#define ENOMEM          12  // Out of memory
#define EACCES          13  // Invalid access
#define EFAULT          14  // Bad address
#define EEXIST          17  // File exists
#define ENODEV          19  // No such device
#define EISDIR          21  // Is a directory
#define EINVAL          22  // Invalid argument
#define EMFILE          24  // Too many open files
#define ENOTTY	        25  // Invalid tty
#define ESPIPE          29  // Illegal seek
#define EPIPE           32  // Broken pipe
#define ENOSYS          38  // Invalid system call number
#define EOPNOTSUPP      95  // Operation not supported on transport endpoint
#define EADDRINUSE      98  // Address already in use
#define EAFNOSUPPORT    102 // Address family not supported
#define ENOTCONN        107 // Transport endpoint is not connected
#define ECONNREFUSED    111 // Connection refused
#define EINPROGRESS     115 // Operation now in progress
#define EPROTONOSUPPORT 135 // Protocol not supported
#define ENOPRIV         720 // Invalid privilege permissions

#define SYSCALL_SYS_READ                0
#define SYSCALL_SYS_WRITE               1
#define SYSCALL_SYS_OPEN                2
#define SYSCALL_SYS_CLOSE               3
#define SYSCALL_SYS_LSEEK               8
#define SYSCALL_SYS_MMAP                9
#define SYSCALL_SYS_MUNMAP              11
#define SYSCALL_SYS_BRK                 12
#define SYSCALL_SYS_IOCTL               16
#define SYSCALL_SYS_WRITEV              20
#define SYSCALL_SYS_NANOSLEEP           35
#define SYSCALL_SYS_GETPID              39
#define SYSCALL_SYS_SOCKET              41
#define SYSCALL_SYS_CONNECT             42
#define SYSCALL_SYS_ACCEPT              43
#define SYSCALL_SYS_SENDTO              44
#define SYSCALL_SYS_RECVFROM            45
#define SYSCALL_SYS_BIND                49
#define SYSCALL_SYS_LISTEN              50
#define SYSCALL_SYS_EXIT                60
#define SYSCALL_SYS_FCNTL               72
#define SYSCALL_SYS_SET_THREAD_AREA     158
#define SYSCALL_SYS_SET_TID_ADDRESS     218
#define SYSCALL_SYS_EXIT_GROUP          231

#define SYSCALL_SYS_PROC_CREATE         706
#define SYSCALL_SYS_PROC_WAIT           707
#define SYSCALL_SYS_PROC_CLOSE          708
#define SYSCALL_SYS_SHM_CREATE          709
#define SYSCALL_SYS_SHM_OPEN            710
#define SYSCALL_SYS_SHM_DESTROY         711
#define SYSCALL_SYS_SHM_MAP             712
#define SYSCALL_SYS_SHM_UNMAP           713

#define SYSCALL_SYS_ELEVATE             790

#define SYSCALL_SYS_GRAPHICS_FRAMEBUFFER_OP     800
#define SYSCALL_SYS_READ_INPUT_EVENT            801

// Uncomment this if you want to see strace-style logs for every issued syscall
// #define STELLUX_STRACE_ENABLED

/**
 * @brief Initializes the syscall handler table.
 * 
 * This function sets up the global syscall table with all registered system call handlers.
 * It should be called during kernel initialization to ensure proper syscall handling.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init_syscall_table();

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

