#ifndef STELLUX_ARCH_X86_64_SIGNAL_DELIVERY_H
#define STELLUX_ARCH_X86_64_SIGNAL_DELIVERY_H

#include "signal/sigframe.h"
#include "syscall/syscall_frame.h"
#include "signals/signal_types.h"

namespace x86 {

/**
 * @brief Fill a zeroed kernel-local signal frame from interrupted state.
 * Pure register marshaling with no user access, so it is unit-testable.
 * user_fpstate is the user address the FXSAVE image will occupy, and the
 * caller must have zeroed the frame so no kernel stack data leaks out.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void pack_sigframe(rt_sigframe* frame,
                                     const syscall_frame* ctx,
                                     int64_t saved_result, uint32_t sig,
                                     signals::sig_set_t old_blocked,
                                     uint64_t user_fpstate);

/**
 * @brief Apply a restored frame onto interrupted state (rt_sigreturn core).
 * Sanitizes user-controlled RFLAGS and rejects a non-canonical return RIP,
 * since SYSRET would otherwise fault in Ring 0. Returns false and leaves
 * ctx untouched on a bad frame. Pure, unit-testable.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool unpack_sigframe(const rt_sigframe* frame,
                                       syscall_frame* ctx,
                                       signals::sig_set_t* mask);

/**
 * @brief Build a signal frame on the user stack and redirect ctx to the
 * handler. Returns 0 on success, negative when the user stack is unwritable.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t build_signal_frame(syscall_frame* ctx, uint32_t sig,
                                             const signals::k_sigaction* act,
                                             signals::sig_set_t old_blocked,
                                             int64_t saved_result);

/**
 * @brief Restore interrupted state from the user signal frame and return
 * the value to resume in RAX. Kills the task with SIGSEGV on a bad frame.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int64_t restore_signal_frame(syscall_frame* ctx);

} // namespace x86

#endif // STELLUX_ARCH_X86_64_SIGNAL_DELIVERY_H
