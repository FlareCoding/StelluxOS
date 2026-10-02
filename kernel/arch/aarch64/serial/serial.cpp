#include "serial/serial.h"
#include "hw/mmio.h"
#include "irq/irq.h"
#include "irq/irq_arch.h"
#include "mm/early_mmu.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"

namespace serial {

#if defined(STLX_PLATFORM_JETSON_NANO)

// ============================================================================
// Jetson Nano (Tegra210): 16550-compatible UART backend
//
// Tegra has no PL011. UARTA at 0x70006000 is the debug-header console
// (ttyS0 under L4T, J44 on the A02 carrier / J50 on B01). Registers follow
// the classic 16550 layout but are spaced 4 bytes apart (reg-shift=2) and
// accessed as 32-bit words.
// ============================================================================

constexpr uintptr_t UART_PHYS = 0x70006000; // UARTA

// Register offsets (16550 offset << 2)
constexpr uintptr_t REG_RBR_THR = 0x00; // Receive buffer (r) / transmit holding (w)
constexpr uintptr_t REG_IER = 0x04;     // Interrupt enable register
constexpr uintptr_t REG_IIR_FCR = 0x08; // Interrupt ident (r) / FIFO control (w)
constexpr uintptr_t REG_LSR = 0x14;     // Line status register

// Line status register bits
constexpr uint32_t LSR_DR = (1 << 0);   // Receive data ready
constexpr uint32_t LSR_THRE = (1 << 5); // Transmit holding register empty

// Interrupt enable register bits
constexpr uint32_t IER_RDA = (1 << 0);  // Received data available

// FIFO control register bits
constexpr uint32_t FCR_FIFO_EN = (1 << 0);
constexpr uint32_t FCR_RX_CLR = (1 << 1);
constexpr uint32_t FCR_TX_CLR = (1 << 2);

// Tegra210 UARTA interrupt: SPI 36 → GIC INTID 68
constexpr uint32_t UART_GIC_INTID = 68;

// UART base virtual address (set during init)
// Feeds elevated MMIO writes, so a stray write here would turn every log
// call into an arbitrary Ring 0 memory write.
__PRIVILEGED_BSS static uintptr_t uart_base = 0;

__PRIVILEGED_BSS static rx_callback_t g_rx_callback;

int32_t init() {
    // Map UART if not already mapped
    if (uart_base == 0) {
        if (early_mmu::init() != early_mmu::OK) {
            return ERR_NO_DEVICE;
        }

        uart_base = early_mmu::map_device(UART_PHYS, 0x1000);
        if (uart_base == 0) {
            return ERR_NO_DEVICE;
        }
    }

    // The boot chain (TegraBoot/cboot/U-Boot) leaves UARTA configured at
    // 115200 8N1, and reprogramming the divisors would require knowing the
    // PLLP-derived UART clock rate. Keep the firmware line settings and only
    // take over interrupt and FIFO state.
    mmio::write32(uart_base + REG_IER, 0);
    mmio::write32(uart_base + REG_IIR_FCR, FCR_FIFO_EN | FCR_RX_CLR | FCR_TX_CLR);

    return OK;
}

void write_char(char c) {
    // Wait for the transmit holding register to drain
    while ((mmio::read32(uart_base + REG_LSR) & LSR_THRE) == 0) {
        asm volatile ("yield");
    }
    mmio::write32(uart_base + REG_RBR_THR, static_cast<uint32_t>(c));
}

void write(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        write_char(data[i]);
    }
}

int32_t read_char() {
    if ((mmio::read32(uart_base + REG_LSR) & LSR_DR) == 0) {
        return ERR_NO_DATA;
    }

    return mmio::read32(uart_base + REG_RBR_THR) & 0xFF;
}

int32_t remap() {
    uintptr_t kva_base = 0;
    uintptr_t kva_va = 0;
    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(UART_PHYS),
        0x1000,
        paging::PAGE_KERNEL_RW,
        kva_base, kva_va);
    if (rc != vmm::OK) {
        return ERR_NO_DEVICE;
    }

    uart_base = kva_va;

    return OK;
}

__PRIVILEGED_CODE void set_rx_callback(rx_callback_t cb) {
    g_rx_callback = cb;
}

__PRIVILEGED_CODE int32_t enable_rx_interrupt() {
    irq::set_spi_target(UART_GIC_INTID, 0x01);
    // TF-A at EL3 configures the GIC-400 with security extensions; the SPI
    // must be moved to group 1 for non-secure delivery (same as RPi4).
    irq::set_group1(UART_GIC_INTID);
    irq::set_level_triggered(UART_GIC_INTID);
    irq::unmask(UART_GIC_INTID);
    mmio::write32(uart_base + REG_IER, IER_RDA);
    return OK;
}

__PRIVILEGED_CODE void on_rx_irq() {
    // Drain the RX FIFO; the level-triggered SPI deasserts once empty
    while ((mmio::read32(uart_base + REG_LSR) & LSR_DR) != 0) {
        char c = static_cast<char>(mmio::read32(uart_base + REG_RBR_THR) & 0xFF);
        if (g_rx_callback) {
            g_rx_callback(c);
        }
    }
}

uint32_t irq_id() {
    return UART_GIC_INTID;
}

void set_port(uint16_t) {
    // No-op: AArch64 uses MMIO UARTs, not x86 port I/O
}

#else // PL011 platforms (QEMU virt, RPi4)

// Both QEMU virt and RPi4 use the same PL011 IP block with identical
// registers, only the base address and clock rate differ.
#if defined(STLX_PLATFORM_RPI4)
// Raspberry Pi 4 (BCM2711): PL011 on GPIO header, 48 MHz UART clock
constexpr uintptr_t PL011_PHYS = 0xFE201000;
constexpr uint32_t BAUD_IBRD = 26;   // 48000000 / (16 * 115200) = 26.042
constexpr uint32_t BAUD_FBRD = 3;    // 0.042 * 64 = 2.67 ~ 3
#else
// QEMU virt machine: PL011 at fixed address, 24 MHz UART clock
constexpr uintptr_t PL011_PHYS = 0x09000000;
constexpr uint32_t BAUD_IBRD = 13;   // 24000000 / (16 * 115200) = 13.021
constexpr uint32_t BAUD_FBRD = 1;    // 0.021 * 64 = 1.33 ~ 1
#endif

// Register offsets from base (same for all PL011 implementations)
constexpr uintptr_t REG_DR = 0x00;    // Data register
constexpr uintptr_t REG_FR = 0x18;    // Flag register
constexpr uintptr_t REG_IBRD = 0x24;  // Integer baud rate divisor
constexpr uintptr_t REG_FBRD = 0x28;  // Fractional baud rate divisor
constexpr uintptr_t REG_LCR = 0x2C;   // Line control register
constexpr uintptr_t REG_CR = 0x30;    // Control register
constexpr uintptr_t REG_IMSC = 0x38;  // Interrupt mask set/clear
constexpr uintptr_t REG_MIS = 0x40;   // Masked interrupt status
constexpr uintptr_t REG_ICR = 0x44;   // Interrupt clear register

// Flag register bits
constexpr uint32_t FR_RXFE = (1 << 4); // Receive FIFO empty
constexpr uint32_t FR_TXFF = (1 << 5); // Transmit FIFO full

// IMSC / MIS / ICR bits
constexpr uint32_t INT_RX      = (1 << 4); // Receive interrupt
constexpr uint32_t INT_RT      = (1 << 6); // Receive timeout interrupt

// Platform GIC interrupt ID for PL011 UART
#if defined(STLX_PLATFORM_RPI4)
constexpr uint32_t PL011_GIC_INTID = 153; // GIC SPI 121 (VC IRQ 57 + 64 offset + 32 INTID base)
#else
constexpr uint32_t PL011_GIC_INTID = 33; // QEMU virt: SPI 1
#endif

// Control register bits
constexpr uint32_t CR_UARTEN = (1 << 0); // UART enable
constexpr uint32_t CR_TXE = (1 << 8);    // Transmit enable
constexpr uint32_t CR_RXE = (1 << 9);    // Receive enable

// Line control register bits
constexpr uint32_t LCR_FEN = (1 << 4); // Enable FIFOs
constexpr uint32_t LCR_WLEN_8 = (3 << 5); // 8-bit word length

// UART base virtual address (set during init)
// Feeds elevated MMIO writes, so a stray write here would turn every log
// call into an arbitrary Ring 0 memory write.
__PRIVILEGED_BSS static uintptr_t uart_base = 0;

__PRIVILEGED_BSS static rx_callback_t g_rx_callback;

int32_t init() {
    // Map UART if not already mapped
    if (uart_base == 0) {
        if (early_mmu::init() != early_mmu::OK) {
            return ERR_NO_DEVICE;
        }

        uart_base = early_mmu::map_device(PL011_PHYS, 0x1000);
        if (uart_base == 0) {
            return ERR_NO_DEVICE;
        }
    }

    // Disable UART while configuring
    mmio::write32(uart_base + REG_CR, 0);

    // Disable all interrupts
    mmio::write32(uart_base + REG_IMSC, 0);

    // Set baud rate to 115200 using platform-specific divisors
    mmio::write32(uart_base + REG_IBRD, BAUD_IBRD);
    mmio::write32(uart_base + REG_FBRD, BAUD_FBRD);

    // Configure line control: 8 bits, no parity, 1 stop bit, enable FIFOs
    mmio::write32(uart_base + REG_LCR, LCR_WLEN_8 | LCR_FEN);

    // Enable UART, TX, and RX
    mmio::write32(uart_base + REG_CR, CR_UARTEN | CR_TXE | CR_RXE);

    return OK;
}

void write_char(char c) {
    // Wait for transmit FIFO to have space
    while ((mmio::read32(uart_base + REG_FR) & FR_TXFF) != 0) {
        asm volatile ("yield");
    }
    mmio::write32(uart_base + REG_DR, static_cast<uint32_t>(c));
}

void write(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        write_char(data[i]);
    }
}

int32_t read_char() {
    // Check if receive FIFO is empty
    if ((mmio::read32(uart_base + REG_FR) & FR_RXFE) != 0) {
        return ERR_NO_DATA;
    }

    return mmio::read32(uart_base + REG_DR) & 0xFF;
}

int32_t remap() {
    uintptr_t kva_base = 0;
    uintptr_t kva_va = 0;
    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(PL011_PHYS),
        0x1000,
        paging::PAGE_KERNEL_RW,
        kva_base, kva_va);
    if (rc != vmm::OK) {
        return ERR_NO_DEVICE;
    }

    uart_base = kva_va;

    return OK;
}

__PRIVILEGED_CODE void set_rx_callback(rx_callback_t cb) {
    g_rx_callback = cb;
}

__PRIVILEGED_CODE int32_t enable_rx_interrupt() {
#if defined(STLX_PLATFORM_RPI4)
    irq::set_group1(PL011_GIC_INTID);
    irq::set_level_triggered(PL011_GIC_INTID);
#endif
    irq::unmask(PL011_GIC_INTID);
    mmio::write32(uart_base + REG_IMSC, INT_RX | INT_RT);
    return OK;
}

__PRIVILEGED_CODE void on_rx_irq() {
    uint32_t mis = mmio::read32(uart_base + REG_MIS);

    if (mis & (INT_RX | INT_RT)) {
        while ((mmio::read32(uart_base + REG_FR) & FR_RXFE) == 0) {
            char c = static_cast<char>(mmio::read32(uart_base + REG_DR) & 0xFF);
            if (g_rx_callback) {
                g_rx_callback(c);
            }
        }
        if (mis & INT_RT) {
            mmio::write32(uart_base + REG_ICR, INT_RT);
        }
    }
}

uint32_t irq_id() {
    return PL011_GIC_INTID;
}

void set_port(uint16_t) {
    // No-op: AArch64 uses PL011 MMIO, not x86 port I/O
}

#endif // STLX_PLATFORM_JETSON_NANO

} // namespace serial
