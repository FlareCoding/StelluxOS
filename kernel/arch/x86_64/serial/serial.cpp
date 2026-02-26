#include "io/serial.h"
#include "hw/portio.h"

namespace serial {

// COM1 port base address
constexpr uint16_t COM1_BASE = 0x3F8;

// Register offsets from base
constexpr uint16_t REG_DATA = 0;        // Data register (read/write)
constexpr uint16_t REG_IER = 1;         // Interrupt enable register
constexpr uint16_t REG_FCR = 2;         // FIFO control register
constexpr uint16_t REG_LCR = 3;         // Line control register
constexpr uint16_t REG_MCR = 4;         // Modem control register
constexpr uint16_t REG_LSR = 5;         // Line status register

// Line Status Register bits
constexpr uint8_t LSR_DATA_READY = 0x01;  // Data available to read
constexpr uint8_t LSR_TX_EMPTY = 0x20;    // Transmit buffer empty

// Line Control Register bits
constexpr uint8_t LCR_DLAB = 0x80;        // Divisor latch access bit
constexpr uint8_t LCR_8N1 = 0x03;         // 8 data bits, no parity, 1 stop bit

// FIFO Control Register bits
constexpr uint8_t FCR_ENABLE = 0x01;      // Enable FIFOs
constexpr uint8_t FCR_CLEAR_RX = 0x02;    // Clear receive FIFO
constexpr uint8_t FCR_CLEAR_TX = 0x04;    // Clear transmit FIFO
constexpr uint8_t FCR_TRIGGER_14 = 0xC0;  // 14-byte trigger level

// Modem Control Register bits
constexpr uint8_t MCR_DTR = 0x01;         // Data terminal ready
constexpr uint8_t MCR_RTS = 0x02;         // Request to send
constexpr uint8_t MCR_OUT2 = 0x08;        // OUT2 (enables IRQs)

int32_t init() {
    // Disable all interrupts
    portio::out8(COM1_BASE + REG_IER, 0x00);

    // Enable DLAB to set baud rate divisor
    portio::out8(COM1_BASE + REG_LCR, LCR_DLAB);

    // Set divisor to 1 (115200 baud with 1.8432 MHz clock)
    portio::out8(COM1_BASE + REG_DATA, 0x01); // Divisor low byte
    portio::out8(COM1_BASE + REG_IER, 0x00);  // Divisor high byte

    // Configure 8N1, disable DLAB
    portio::out8(COM1_BASE + REG_LCR, LCR_8N1);

    // Enable and clear FIFOs, set 14-byte trigger
    portio::out8(COM1_BASE + REG_FCR, FCR_ENABLE | FCR_CLEAR_RX | FCR_CLEAR_TX | FCR_TRIGGER_14);

    // Enable DTR, RTS, and OUT2 (for interrupts, though we use polling)
    portio::out8(COM1_BASE + REG_MCR, MCR_DTR | MCR_RTS | MCR_OUT2);

    // Verify the UART is working by checking if we can read LSR
    uint8_t lsr = portio::in8(COM1_BASE + REG_LSR);
    if (lsr == 0xFF) {
        // No UART present (all bits high typically means no device)
        return ERR_NO_DEVICE;
    }

    return OK;
}

void write_char(char c) {
    // Wait for transmit buffer to be empty
    while ((portio::in8(COM1_BASE + REG_LSR) & LSR_TX_EMPTY) == 0) {
        asm volatile ("pause");
    }
    portio::out8(COM1_BASE + REG_DATA, static_cast<uint8_t>(c));
}

void write(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        write_char(data[i]);
    }
}

int32_t read_char() {
    // Check if data is available
    if ((portio::in8(COM1_BASE + REG_LSR) & LSR_DATA_READY) == 0) {
        return ERR_NO_DATA;
    }
    return portio::in8(COM1_BASE + REG_DATA);
}

int32_t remap() {
    return OK;
}

} // namespace serial
