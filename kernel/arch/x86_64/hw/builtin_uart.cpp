#include "arch/arch_uart.h"
#include "drivers/serial/uart/ns16550.h"
#include "irq/ioapic.h"
#include "defs/vectors.h"

namespace arch {

// COM1 port base address
constexpr uint16_t COM1_BASE = 0x3F8;

constexpr uint8_t COM1_LEGACY_IRQ = 4;

__PRIVILEGED_CODE static int32_t route_com1_interrupt();

// Feeds elevated port I/O, so a stray write here would redirect Ring 0
// accesses to arbitrary ports such as the PCI config window.
__PRIVILEGED_DATA constinit static serial::uart::ns16550 g_com1(
    COM1_BASE,
    serial::uart::NS16550_STANDARD_CLOCK_HZ,
    route_com1_interrupt
);

// The trap handler hands VEC_SERIAL to the built-in UART
__PRIVILEGED_CODE static int32_t route_com1_interrupt() {
    if (ioapic::route_irq(COM1_LEGACY_IRQ, x86::VEC_SERIAL, 0) != ioapic::OK) {
        return serial::uart::ERR_UNSUPPORTED;
    }

    return serial::uart::OK;
}

serial::uart::device* builtin_uart() {
    return &g_com1;
}

} // namespace arch
