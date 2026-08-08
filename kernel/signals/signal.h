#ifndef STELLUX_SIGNALS_SIGNAL_H
#define STELLUX_SIGNALS_SIGNAL_H

#include "signals/signal_types.h"

namespace sched {
struct task;
struct thread_group;
}

namespace signals {

constexpr int32_t OK        = 0;
constexpr int32_t ERR_INVAL = -1;

// rt_sigprocmask how values (musl ABI)
constexpr uint32_t SIG_BLOCK   = 0;
constexpr uint32_t SIG_UNBLOCK = 1;
constexpr uint32_t SIG_SETMASK = 2;

/**
 * @brief Install the action for sig (handler, SIG_IGN, or SIG_DFL),
 * returning the previous one in old. Rejects invalid signals and any
 * attempt to change SIGKILL/SIGSTOP (querying them is still allowed).
 * Installing SIG_IGN discards pending instances of sig (POSIX).
 * @return OK or ERR_INVAL.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t set_action(sched::thread_group* tg, uint32_t sig,
                                     const k_sigaction* act, k_sigaction* old);

/**
 * @brief Apply a sigprocmask-style update to a task's blocked mask.
 * If old is non-null the previous mask is returned. If set is null the
 * mask is only queried and how is ignored.
 * SIGKILL/SIGSTOP are silently kept unblockable per POSIX.
 * @return OK or ERR_INVAL for a bad how.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t set_blocked(sched::task* t, uint32_t how,
                                      const sig_set_t* set, sig_set_t* old);

/**
 * @brief Signals pending for the task (thread + process sets) that are
 * currently blocked. Matches rt_sigpending semantics.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE sig_set_t pending_blocked_set(sched::task* t);

} // namespace signals

#endif // STELLUX_SIGNALS_SIGNAL_H
