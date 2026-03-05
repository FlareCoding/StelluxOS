#ifndef STELLUX_SYSCALL_SYSCALL_H
#define STELLUX_SYSCALL_SYSCALL_H

#include "common/types.h"

namespace syscall {

constexpr int32_t OK = 0;
constexpr int32_t ERR_INIT = -1;

// Syscall numbers
constexpr uint64_t SYS_YIELD   = 1000;
constexpr uint64_t SYS_ELEVATE = 1001;

// Process management
constexpr uint64_t SYS_PROC_CREATE = 1010;
constexpr uint64_t SYS_PROC_START  = 1011;
constexpr uint64_t SYS_PROC_WAIT   = 1012;
constexpr uint64_t SYS_PROC_DETACH = 1013;
constexpr uint64_t SYS_PROC_INFO       = 1014;
constexpr uint64_t SYS_PROC_SET_HANDLE = 1015;
constexpr uint64_t SYS_PROC_KILL       = 1016;

// PTY
constexpr uint64_t SYS_PTY_CREATE = 1020;

/**
 * Architecture-specific syscall initialization (MSRs on x86, etc.)
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init_arch_syscalls();

} // namespace syscall

/**
 * Generic C-style syscall handler (called by arch-specific assembly/dispatch)
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE extern "C" int64_t stlx_syscall_handler(
    uint64_t syscall_num,
    uint64_t arg1,
    uint64_t arg2,
    uint64_t arg3,
    uint64_t arg4,
    uint64_t arg5,
    uint64_t arg6
);

#endif // STELLUX_SYSCALL_SYSCALL_H
