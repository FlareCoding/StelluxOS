#ifndef STELLUX_ARCH_INIT_H
#define STELLUX_ARCH_INIT_H

#include "common/types.h"

namespace arch {

constexpr int32_t OK = 0;
constexpr int32_t ERR_GDT_INIT = -1;
constexpr int32_t ERR_TRAP_INIT = -2;
constexpr int32_t ERR_PERCPU_INIT = -3;
constexpr int32_t ERR_CPU_INIT = -4;
constexpr int32_t ERR_SCHED_INIT = -5;
constexpr int32_t ERR_SYSCALL_INIT = -6;

/**
 * @brief Architecture-specific initialization early on in the boot process.
 * Called before memory management is setup.
 * @return OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t early_init();

} // namespace arch

#endif // STELLUX_ARCH_INIT_H
