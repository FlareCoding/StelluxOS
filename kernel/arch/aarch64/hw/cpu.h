#ifndef STELLUX_ARCH_AARCH64_HW_CPU_H
#define STELLUX_ARCH_AARCH64_HW_CPU_H

#include "common/types.h"

namespace cpu {

// Affinity fields of MPIDR_EL1: Aff0 to Aff2 in bits 0 to 23, Aff3 in bits 32 to 39
constexpr uint64_t MPIDR_AFFINITY_MASK = 0xFF00FFFFFFULL;
constexpr uint32_t MPIDR_AFF_LEVELS    = 4;
inline constexpr uint32_t MPIDR_AFF_SHIFT[MPIDR_AFF_LEVELS] = {0, 8, 16, 32};

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void halt() {
    asm volatile("wfi");
}

/**
 * @brief Put the cpu to sleep until the next interrupt and return with interrupts enabled.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void halt_until_interrupt() {
    asm volatile("dsb sy; wfi; msr daifclr, #0xf" ::: "memory");
}

inline void relax() {
    asm volatile("wfe");
}

inline void send_event() {
    asm volatile("sev" ::: "memory");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void irq_disable() {
    asm volatile("msr daifset, #0xf" ::: "memory");
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void irq_enable() {
    asm volatile("msr daifclr, #0xf" ::: "memory");
}

constexpr uint64_t DAIF_IRQ_MASKED = 1ULL << 7;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline bool irqs_enabled() {
    uint64_t daif;
    asm volatile("mrs %0, daif" : "=r"(daif));
    return (daif & DAIF_IRQ_MASKED) == 0;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline uint64_t irq_save() {
    uint64_t daif;
    asm volatile("mrs %0, daif; msr daifset, #0xf" : "=r"(daif) :: "memory");
    return daif;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline void irq_restore(uint64_t daif) {
    asm volatile("msr daif, %0" :: "r"(daif) : "memory");
}

/**
 * @brief Read this CPU's MPIDR_EL1, which carries its affinity address.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE inline uint64_t read_mpidr() {
    uint64_t mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr;
}

/**
 * @brief Affinity level `level`, 0 to 3, of an MPIDR value.
 */
constexpr uint8_t mpidr_affinity(uint64_t mpidr, uint32_t level) {
    return static_cast<uint8_t>(mpidr >> MPIDR_AFF_SHIFT[level]);
}

inline uint64_t read_tls_base() {
    uint64_t base;
    asm volatile("mrs %0, tpidr_el0" : "=r"(base));
    return base;
}

inline void write_tls_base(uint64_t base) {
    asm volatile("msr tpidr_el0, %0" :: "r"(base) : "memory");
}

} // namespace cpu

#endif // STELLUX_ARCH_AARCH64_HW_CPU_H
