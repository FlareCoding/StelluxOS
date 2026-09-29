#ifndef STELLUX_SYSCALL_HANDLERS_SYS_SIGNAL_H
#define STELLUX_SYSCALL_HANDLERS_SYS_SIGNAL_H

#include "syscall/syscall_table.h"

namespace sched { struct task; }

DECLARE_SYSCALL(rt_sigaction);
DECLARE_SYSCALL(rt_sigprocmask);
DECLARE_SYSCALL(rt_sigpending);
DECLARE_SYSCALL(rt_sigreturn);
DECLARE_SYSCALL(kill);
DECLARE_SYSCALL(tkill);
DECLARE_SYSCALL(tgkill);

namespace syscall {

/**
 * @brief Blocks the signal set at `u_set` until the calling syscall returns, for the waits
 * that take a signal mask. A null `u_set` leaves the mask unchanged.
 * @return 0, EINVAL when `size` is not the size of a signal set, or EFAULT.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int64_t set_temporary_sigmask(sched::task* task, uint64_t u_set, uint64_t size);

} // namespace syscall

#endif // STELLUX_SYSCALL_HANDLERS_SYS_SIGNAL_H
