#ifndef STELLUX_ARCH_AARCH64_HW_PSCI_H
#define STELLUX_ARCH_AARCH64_HW_PSCI_H

#include "common/types.h"

namespace psci {

// PSCI function IDs (SMC64 calling convention)
constexpr uint32_t PSCI_VERSION   = 0x84000000;
constexpr uint64_t PSCI_CPU_ON_64 = 0xC4000003;

// PSCI return codes
constexpr int32_t SUCCESS          = 0;
constexpr int32_t NOT_SUPPORTED    = -1;
constexpr int32_t INVALID_PARAMS   = -2;
constexpr int32_t DENIED           = -3;
constexpr int32_t ALREADY_ON       = -4;
constexpr int32_t ON_PENDING       = -5;
constexpr int32_t INTERNAL_FAILURE = -6;

enum class conduit : uint8_t {
    HVC = 0,
    SMC = 1,
};

/**
 * Raw HVC call. Used on VMs (QEMU virt) where PSCI is handled at EL2.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline int64_t hvc_call(uint64_t fn, uint64_t a1,
                                           uint64_t a2, uint64_t a3) {
    register uint64_t x0 asm("x0") = fn;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;

    asm volatile("hvc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
        :
        : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
          "x12", "x13", "x14", "x15", "x16", "x17", "memory");

    return static_cast<int64_t>(x0);
}

/**
 * Raw SMC call. Used on real hardware (RPi4 with TF-A) where PSCI is at EL3.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline int64_t smc_call(uint64_t fn, uint64_t a1,
                                           uint64_t a2, uint64_t a3) {
    register uint64_t x0 asm("x0") = fn;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;

    asm volatile("smc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
        :
        : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
          "x12", "x13", "x14", "x15", "x16", "x17", "memory");

    return static_cast<int64_t>(x0);
}

/**
 * Detect the PSCI conduit by probing PSCI_VERSION via HVC first, then SMC.
 * @return The working conduit, or HVC as default.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline conduit detect_conduit() {
    int64_t version = hvc_call(PSCI_VERSION, 0, 0, 0);
    if (version >= 0) {
        return conduit::HVC;
    }
    version = smc_call(PSCI_VERSION, 0, 0, 0);
    if (version >= 0) {
        return conduit::SMC;
    }
    return conduit::HVC;
}

/**
 * Issue a PSCI call using the given conduit.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline int32_t call(conduit c, uint64_t fn,
                                       uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t result;
    if (c == conduit::HVC) {
        result = hvc_call(fn, a1, a2, a3);
    } else {
        result = smc_call(fn, a1, a2, a3);
    }
    return static_cast<int32_t>(result);
}

/**
 * Boot a secondary CPU via PSCI CPU_ON.
 * @param c             The detected PSCI conduit (HVC or SMC).
 * @param target_mpidr  MPIDR of the target CPU.
 * @param entry_point   Physical address where the AP begins execution.
 * @param context_id    Opaque value passed to the AP in x0 at entry.
 * @return PSCI result code (0 = SUCCESS).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline int32_t cpu_on(conduit c, uint64_t target_mpidr,
                                         uint64_t entry_point,
                                         uint64_t context_id) {
    return call(c, PSCI_CPU_ON_64, target_mpidr, entry_point, context_id);
}

} // namespace psci

#endif // STELLUX_ARCH_AARCH64_HW_PSCI_H
