#ifndef STELLUX_ARCH_X86_64_HW_BARRIER_H
#define STELLUX_ARCH_X86_64_HW_BARRIER_H

namespace barrier {

/**
 * Compiler barrier - prevents compiler reordering only.
 * Emits no CPU instructions.
 */
inline void compiler() {
    asm volatile("" ::: "memory");
}

/**
 * SMP read barrier - ensures load-load ordering between CPUs.
 * On x86_64: Compiler barrier only (TSO guarantees load-load ordering).
 */
inline void smp_read() {
    asm volatile("" ::: "memory");
}

/**
 * SMP write barrier - ensures store-store ordering between CPUs.
 * On x86_64: Compiler barrier only (TSO guarantees store-store ordering).
 */
inline void smp_write() {
    asm volatile("" ::: "memory");
}

/**
 * SMP full barrier - ensures complete ordering between CPUs.
 * On x86_64: lock prefix provides full barrier; faster than mfence.
 */
inline void smp_full() {
    asm volatile("lock; addl $0, (%%rsp)" ::: "memory", "cc");
}

/**
 * IO read barrier - ensures MMIO read completion.
 * On x86_64: Compiler barrier only (UC memory is strongly ordered).
 */
inline void io_read() {
    asm volatile("" ::: "memory");
}

/**
 * IO write barrier - ensures MMIO write ordering.
 * On x86_64: Compiler barrier only (UC memory is strongly ordered).
 */
inline void io_write() {
    asm volatile("" ::: "memory");
}

/**
 * IO full barrier - ensures complete device synchronization.
 * On x86_64: mfence for maximum safety with devices.
 */
inline void io_full() {
    asm volatile("mfence" ::: "memory");
}

/**
 * DMA read barrier - ensures CPU sees device DMA writes.
 * On x86_64: lfence to serialize loads.
 */
inline void dma_read() {
    asm volatile("lfence" ::: "memory");
}

/**
 * DMA write barrier - ensures device sees CPU writes.
 * On x86_64: sfence to flush store buffers.
 */
inline void dma_write() {
    asm volatile("sfence" ::: "memory");
}

/**
 * DMA full barrier - complete CPU/device memory synchronization.
 * On x86_64: mfence for complete fence semantics.
 */
inline void dma_full() {
    asm volatile("mfence" ::: "memory");
}

/**
 * Instruction barrier - ensures instruction stream synchronization.
 * On x86_64: No-op (x86 is self-synchronizing; TLB flush handles page tables).
 */
inline void instruction() {
    asm volatile("" ::: "memory");
}

} // namespace barrier

#endif // STELLUX_ARCH_X86_64_HW_BARRIER_H
