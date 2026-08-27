#include "syscall/syscall.h"
#include "trap/trap_frame.h"
#include "defs/exception.h"
#include "common/types.h"
#include "common/logging.h"

namespace sched {
__PRIVILEGED_CODE void on_yield(aarch64::trap_frame* tf);
} // namespace sched

namespace syscall {

__PRIVILEGED_CODE int32_t init_arch_syscalls() {
    log::info("syscall: aarch64 initialized (via SVC dispatch)");
    return OK;
}

} // namespace syscall

extern "C" __PRIVILEGED_CODE
void stlx_aarch64_syscall_dispatch(aarch64::trap_frame* tf) {
    uint64_t syscall_num = tf->x[8];

    if (syscall_num == syscall::SYS_YIELD) {
        sched::on_yield(tf);
        return;
    }

    int64_t result = stlx_syscall_handler(
        syscall_num,
        tf->x[0], tf->x[1], tf->x[2],
        tf->x[3], tf->x[4], tf->x[5]
    );

    // Set return value in x0 (standard AArch64 ABI)
    tf->x[0] = static_cast<uint64_t>(result);

    // On SYS_ELEVATE success, rewrite SPSR so the return lands in EL1. A
    // caller already in EL1 also reports success, its SPSR must stay untouched.
    if (syscall_num == syscall::SYS_ELEVATE && result == 0) {
        uint8_t current_mode = tf->spsr & aarch64::SPSR_MODE_MASK;
        if (current_mode == aarch64::SPSR_EL0T) {
            tf->spsr = (tf->spsr & ~aarch64::SPSR_MODE_MASK) | aarch64::SPSR_EL1T;
        }
    }
}
