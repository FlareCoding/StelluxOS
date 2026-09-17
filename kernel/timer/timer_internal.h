#ifndef STELLUX_TIMER_TIMER_INTERNAL_H
#define STELLUX_TIMER_TIMER_INTERNAL_H

#include "common/types.h"

namespace timer {

// The deadline reported when a CPU has no timer scheduled
constexpr uint64_t NO_DEADLINE = ~0ULL;

/**
 * Prepares this CPU's deadline timers. Called once per CPU from the arch
 * timer initialization, before its interrupt is enabled.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void deadline_init_this_cpu();

/**
 * Arch-specific: makes this CPU's one-shot timer fire no later than
 * `deadline_ns` when that is sooner than what is already programmed.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void arch_request_deadline(uint64_t deadline_ns);

/**
 * Called by the arch interrupt handler with interrupts off. Wakes this CPU's
 * worker when a timer is due at or before `now_ns`.
 * @return The earliest deadline still in the future, NO_DEADLINE when none.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint64_t deadline_interrupt(uint64_t now_ns);

} // namespace timer

#endif // STELLUX_TIMER_TIMER_INTERNAL_H
