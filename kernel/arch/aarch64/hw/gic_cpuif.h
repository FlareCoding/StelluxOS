#ifndef STELLUX_ARCH_AARCH64_HW_GIC_CPUIF_H
#define STELLUX_ARCH_AARCH64_HW_GIC_CPUIF_H

#include "common/types.h"

/**
 * Wrappers for the GICv3 CPU interface system registers (ICC_*_EL1). GICv2
 * reaches its CPU interface through MMIO instead, so only the GICv3 backend
 * uses these. The registers are named by encoding so the assembler needs no
 * GIC extension enabled.
 */
namespace gic_cpuif {

constexpr uint64_t SRE_ENABLE             = 1ULL << 0;  // System register interface
constexpr uint64_t SRE_DISABLE_FIQ_BYPASS = 1ULL << 1;
constexpr uint64_t SRE_DISABLE_IRQ_BYPASS = 1ULL << 2;
constexpr uint64_t CTLR_EOI_MODE          = 1ULL << 1;  // Split priority drop and deactivation
constexpr uint64_t CTLR_RSS               = 1ULL << 18; // SGI targets may use Aff0 16 to 255
constexpr uint64_t IGRPEN_ENABLE          = 1ULL << 0;
constexpr uint64_t PMR_ALLOW_ALL          = 0xFF;
constexpr uint64_t BPR_MOST_PREEMPTION    = 0;          // Smallest binary point the GIC accepts

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline uint64_t read_sre() {
    uint64_t sre;
    asm volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre));
    return sre;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void write_sre(uint64_t sre) {
    asm volatile("msr S3_0_C12_C12_5, %0; isb" :: "r"(sre) : "memory");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void write_pmr(uint64_t pmr) {
    asm volatile("msr S3_0_C4_C6_0, %0" :: "r"(pmr) : "memory");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void write_bpr1(uint64_t bpr) {
    asm volatile("msr S3_0_C12_C12_3, %0" :: "r"(bpr) : "memory");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline uint64_t read_ctlr() {
    uint64_t ctlr;
    asm volatile("mrs %0, S3_0_C12_C12_4" : "=r"(ctlr));
    return ctlr;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void write_ctlr(uint64_t ctlr) {
    asm volatile("msr S3_0_C12_C12_4, %0" :: "r"(ctlr) : "memory");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void write_igrpen1(uint64_t enable) {
    asm volatile("msr S3_0_C12_C12_7, %0; isb" :: "r"(enable) : "memory");
}

/**
 * @brief Acknowledge the highest priority pending group 1 interrupt.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline uint64_t read_iar1() {
    uint64_t ack;
    asm volatile("mrs %0, S3_0_C12_C12_0" : "=r"(ack) :: "memory");
    return ack;
}

/**
 * @brief Drop priority and deactivate an acknowledged group 1 interrupt.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void write_eoir1(uint64_t ack) {
    asm volatile("msr S3_0_C12_C12_1, %0; isb" :: "r"(ack) : "memory");
}

/**
 * @brief Raise a group 1 SGI. Stores made before the call are visible to the
 * target CPU by the time it takes the interrupt.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void write_sgi1r(uint64_t sgi1r) {
    asm volatile("dsb ishst; msr S3_0_C12_C11_5, %0; isb" :: "r"(sgi1r) : "memory");
}

} // namespace gic_cpuif

#endif // STELLUX_ARCH_AARCH64_HW_GIC_CPUIF_H
