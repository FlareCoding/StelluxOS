#ifndef STELLUX_ARCH_ARCH_SIGNAL_H
#define STELLUX_ARCH_ARCH_SIGNAL_H

#include "common/types.h"

namespace sched {
struct task;
}

namespace arch {

/**
 * @brief Deliver one pending handler-bound signal at the syscall-return
 * boundary by redirecting the saved user context to the handler.
 * An action without a restorer or an unwritable user stack kills the task
 * with SIGSEGV. Returns the value for the user's result register, so a
 * built frame keeps the handler's first argument intact.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int64_t deliver_pending_signal(sched::task* self,
                                                 int64_t result);

/**
 * @brief rt_sigreturn core: restore the interrupted context from the user
 * signal frame and return the value to resume in the result register.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int64_t restore_signal_context();

} // namespace arch

#endif // STELLUX_ARCH_ARCH_SIGNAL_H
