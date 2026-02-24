#ifndef STELLUX_ARCH_X86_64_DEBUG_PANIC_H
#define STELLUX_ARCH_X86_64_DEBUG_PANIC_H

#include "trap/trap_frame.h"

namespace panic {

/**
 * @note Privilege: **required**
 */
[[noreturn]] __PRIVILEGED_CODE void on_trap(x86::trap_frame* tf);

} // namespace panic

#endif // STELLUX_ARCH_X86_64_DEBUG_PANIC_H
