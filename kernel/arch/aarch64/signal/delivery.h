#ifndef STELLUX_ARCH_AARCH64_SIGNAL_DELIVERY_H
#define STELLUX_ARCH_AARCH64_SIGNAL_DELIVERY_H

#include "signal/sigframe.h"
#include "trap/trap_frame.h"
#include "sched/fpu_state.h"
#include "signals/signal_types.h"

namespace sched { struct task; }

namespace aarch64 {

/**
 * @brief Fill a zeroed kernel-local signal frame from interrupted state.
 * Pure marshaling with no user access, so it is unit-testable. The FP block
 * is written in the kernel ABI field order, converting from fpu_state.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void pack_sigframe(rt_sigframe* frame, const trap_frame* tf,
                                     int64_t saved_result, uint32_t sig,
                                     signals::sig_set_t old_blocked,
                                     const sched::fpu_state* fp);

/**
 * @brief Apply a restored frame onto interrupted state (rt_sigreturn core).
 * Forces PSTATE back to EL0 with unmasked interrupts so a forged frame can
 * never return to EL1, and recovers the saved mask and FP. Returns false and
 * leaves tf untouched on a corrupt FP record. Pure, unit-testable.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool unpack_sigframe(const rt_sigframe* frame, trap_frame* tf,
                                       sched::fpu_state* fp,
                                       signals::sig_set_t* mask);

/**
 * @brief Build a signal frame on the user stack and redirect tf to the
 * handler. Returns 0 on success, negative when the user stack is unwritable.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t build_signal_frame(trap_frame* tf, uint32_t sig,
                                             const signals::k_sigaction* act,
                                             signals::sig_set_t old_blocked,
                                             int64_t saved_result);

/**
 * @brief Restore interrupted state from the user signal frame and return
 * the value to resume in x0. Kills the task with SIGSEGV on a bad frame.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int64_t restore_signal_frame(trap_frame* tf);

/**
 * @brief Deliver one pending handler-bound signal to a task interrupted
 * in user mode, redirecting the trap frame to the handler. The frame
 * write never blocks or pages in: when the user stack is not resident
 * the signal is returned to the pending set for a later boundary.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void deliver_async_signal(sched::task* self,
                                            trap_frame* tf);

} // namespace aarch64

#endif // STELLUX_ARCH_AARCH64_SIGNAL_DELIVERY_H
