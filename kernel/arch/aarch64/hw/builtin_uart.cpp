#include "arch/arch_uart.h"
#include "drivers/serial/uart/pl011.h"
#include "irq/irq_arch.h"

namespace arch {

// Both QEMU virt and RPi4 use the same PL011 IP block with identical
// registers, only the base address and clock rate differ.
#if defined(STLX_PLATFORM_RPI4)
// Raspberry Pi 4 (BCM2711): PL011 on GPIO header, 48 MHz UART clock
constexpr uintptr_t PL011_PHYS = 0xFE201000;
constexpr uint32_t PL011_CLOCK_HZ = 48000000;
constexpr uint32_t PL011_GIC_INTID = 153; // GIC SPI 121 (VC IRQ 57 + 64 offset + 32 INTID base)
#else
// QEMU virt machine: PL011 at fixed address, 24 MHz UART clock
constexpr uintptr_t PL011_PHYS = 0x09000000;
constexpr uint32_t PL011_CLOCK_HZ = 24000000;
constexpr uint32_t PL011_GIC_INTID = 33; // QEMU virt: SPI 1
#endif

__PRIVILEGED_CODE static void prepare_pl011_interrupt(uint32_t irq);

// Its register base feeds elevated MMIO writes, so a stray write here would
// turn every log call into an arbitrary Ring 0 memory write.
__PRIVILEGED_DATA constinit static serial::uart::pl011 g_pl011(
    PL011_PHYS,
    PL011_CLOCK_HZ,
    PL011_GIC_INTID,
    prepare_pl011_interrupt
);

__PRIVILEGED_CODE static void prepare_pl011_interrupt(uint32_t irq) {
#if defined(STLX_PLATFORM_RPI4)
    irq::set_group1(irq);
    irq::set_level_triggered(irq);
#else
    (void)irq;
#endif
}

serial::uart::device* builtin_uart() {
    return &g_pl011;
}

} // namespace arch
