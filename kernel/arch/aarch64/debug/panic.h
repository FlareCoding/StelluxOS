#ifndef STELLUX_ARCH_AARCH64_DEBUG_PANIC_H
#define STELLUX_ARCH_AARCH64_DEBUG_PANIC_H

#include "trap/trap_frame.h"

namespace panic {

/**
 * @note Privilege: **required**
 */
[[noreturn]] __PRIVILEGED_CODE void on_trap(aarch64::trap_frame* tf, const char* kind);

} // namespace panic

#endif // STELLUX_ARCH_AARCH64_DEBUG_PANIC_H
