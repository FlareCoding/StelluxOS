#include "io/serial.h"
#include "hw/mmio.h"
#include "mm/early_mmu.h"

namespace serial {

// PL011 UART physical address (QEMU virt machine)
constexpr uintptr_t PL011_PHYS = 0x09000000;

// Register offsets from base
constexpr uintptr_t REG_DR = 0x00;    // Data register
constexpr uintptr_t REG_FR = 0x18;    // Flag register
constexpr uintptr_t REG_IBRD = 0x24;  // Integer baud rate divisor
constexpr uintptr_t REG_FBRD = 0x28;  // Fractional baud rate divisor
constexpr uintptr_t REG_LCR = 0x2C;   // Line control register
constexpr uintptr_t REG_CR = 0x30;    // Control register
constexpr uintptr_t REG_IMSC = 0x38;  // Interrupt mask set/clear

// Flag register bits
constexpr uint32_t FR_RXFE = (1 << 4); // Receive FIFO empty
constexpr uint32_t FR_TXFF = (1 << 5); // Transmit FIFO full

// Control register bits
constexpr uint32_t CR_UARTEN = (1 << 0); // UART enable
constexpr uint32_t CR_TXE = (1 << 8);    // Transmit enable
constexpr uint32_t CR_RXE = (1 << 9);    // Receive enable

// Line control register bits
constexpr uint32_t LCR_FEN = (1 << 4);    // Enable FIFOs
constexpr uint32_t LCR_WLEN_8 = (3 << 5); // 8-bit word length

// UART base virtual address (set during init)
static uintptr_t uart_base = 0;

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

    // Set baud rate to 115200 (assuming 24MHz UART clock on QEMU virt)
    // Divisor = 24000000 / (16 * 115200) = 13.0208...
    // Integer part = 13, Fractional part = 0.0208 * 64 = 1.33 ≈ 1
    mmio::write32(uart_base + REG_IBRD, 13);
    mmio::write32(uart_base + REG_FBRD, 1);

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

} // namespace serial
