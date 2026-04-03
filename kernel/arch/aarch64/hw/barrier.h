#ifndef STELLUX_ARCH_AARCH64_HW_BARRIER_H
#define STELLUX_ARCH_AARCH64_HW_BARRIER_H

namespace barrier {

/**
 * Compiler barrier - prevents compiler reordering only.
 * Emits no CPU instructions. On AArch64, rarely sufficient alone due to weak ordering.
 */
inline void compiler() {
    asm volatile("" ::: "memory");
}

/**
 * SMP read barrier - ensures load-load ordering between CPUs.
 * On AArch64: dmb ishld (Inner Shareable, load barrier).
 */
inline void smp_read() {
    asm volatile("dmb ishld" ::: "memory");
}

/**
 * SMP write barrier - ensures store-store ordering between CPUs.
 * On AArch64: dmb ishst (Inner Shareable, store barrier).
 */
inline void smp_write() {
    asm volatile("dmb ishst" ::: "memory");
}

/**
 * SMP full barrier - ensures complete ordering between CPUs.
 * On AArch64: dmb ish (Inner Shareable, full barrier).
 */
inline void smp_full() {
    asm volatile("dmb ish" ::: "memory");
}

/**
 * IO read barrier - ensures MMIO read completion.
 * On AArch64: dsb ld (completion guarantee, not just ordering).
 */
inline void io_read() {
    asm volatile("dsb ld" ::: "memory");
}

/**
 * IO write barrier - ensures MMIO write completion.
 * On AArch64: dsb st (ensures write reaches device).
 */
inline void io_write() {
    asm volatile("dsb st" ::: "memory");
}

/**
 * IO full barrier - ensures complete device synchronization.
 * On AArch64: dsb sy (full system completion).
 */
inline void io_full() {
    asm volatile("dsb sy" ::: "memory");
}

/**
 * DMA read barrier - ensures CPU sees device DMA writes.
 * On AArch64: dsb oshld (Outer Shareable, load completion).
 */
inline void dma_read() {
    asm volatile("dsb oshld" ::: "memory");
}

/**
 * DMA write barrier - ensures device sees CPU writes.
 * On AArch64: dsb oshst (Outer Shareable, store completion).
 */
inline void dma_write() {
    asm volatile("dsb oshst" ::: "memory");
}

/**
 * DMA full barrier - complete CPU/device memory synchronization.
 * On AArch64: dsb osh (Outer Shareable, full completion).
 */
inline void dma_full() {
    asm volatile("dsb osh" ::: "memory");
}

/**
 * Instruction barrier - ensures instruction stream synchronization.
 * On AArch64: isb (required after page table changes, system register writes).
 */
inline void instruction() {
    asm volatile("isb" ::: "memory");
}

} // namespace barrier

#endif // STELLUX_ARCH_AARCH64_HW_BARRIER_H
