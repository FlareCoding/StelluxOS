#include "syscall/syscall.h"
#include "trap/trap_frame.h"
#include "defs/exception.h"
#include "common/types.h"
#include "common/logging.h"

namespace syscall {

__PRIVILEGED_CODE int32_t init_arch_syscalls() {
    // AArch64 doesn't need MSR setup like x86 for SVC.
    // SVC is dispatched through the exception vector table (VBAR_EL1).
    log::info("syscall: aarch64 initialized (via SVC dispatch)");
    return OK;
}

} // namespace syscall

// Called from trap handler for SVC exceptions
extern "C" __PRIVILEGED_CODE 
void stlx_aarch64_syscall_dispatch(aarch64::trap_frame* tf) {
    // SVC immediate is in ESR_EL1[15:0] on AArch64
    uint64_t syscall_num = tf->esr & 0xFFFF;

    // According to AArch64 calling convention:
    // x0-x7 = arguments
    // Map to generic handler: stlx_syscall_handler(num, arg1..arg6)
    int64_t result = stlx_syscall_handler(
        syscall_num,
        tf->x[0], tf->x[1], tf->x[2],
        tf->x[3], tf->x[4], tf->x[5]
    );

    // Set return value in x0 (standard AArch64 ABI)
    tf->x[0] = static_cast<uint64_t>(result);

    // For SYS_ELEVATE success: modify SPSR to return to EL1, but only if we
    // actually came from EL0 (we're truly elevating). If we came from
    // EL1 (already elevated, just got a warning), don't modify SPSR.
    if (syscall_num == syscall::SYS_ELEVATE && result == 0) {
        uint8_t current_mode = tf->spsr & aarch64::SPSR_MODE_MASK;
        if (current_mode == aarch64::SPSR_EL0T) {
            // Came from EL0, elevate to EL1
            tf->spsr = (tf->spsr & ~aarch64::SPSR_MODE_MASK) | aarch64::SPSR_EL1T;
        }
    }
}
