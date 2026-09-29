#ifndef STELLUX_SYSCALL_HANDLERS_SYS_DUP_H
#define STELLUX_SYSCALL_HANDLERS_SYS_DUP_H

#include "syscall/syscall_table.h"
#include "resource/resource_types.h"

namespace sched { struct task; }

DECLARE_SYSCALL(dup);
DECLARE_SYSCALL(dup2);
DECLARE_SYSCALL(dup3);

namespace syscall {

/**
 * @brief Duplicate `old_h` into the lowest free handle at or above `min_handle`, with
 * close-on-exec set only when `cloexec` is.
 * @return The duplicate handle, EBADF for a bad `old_h`, EINVAL when `min_handle` is at or
 * past the handle limit, or EMFILE when every handle from there up is taken.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int64_t duplicate_handle(
    sched::task* task,
    resource::handle_t old_h,
    uint64_t min_handle,
    bool cloexec
);

} // namespace syscall

#endif // STELLUX_SYSCALL_HANDLERS_SYS_DUP_H
