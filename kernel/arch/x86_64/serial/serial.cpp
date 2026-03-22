#include "io/serial.h"
#include "hw/portio.h"
#include "irq/ioapic.h"
#include "defs/vectors.h"

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

// IER bits
constexpr uint8_t IER_RX_AVAIL = 0x01;    // Received data available

// Baud rate divisors
constexpr uint16_t BAUD_115200 = 0x01;
constexpr uint16_t BAUD_9600 = 0x0C;

constexpr uint8_t COM1_LEGACY_IRQ = 4;

// Active UART port base -- defaults to COM1, can be redirected to a PCI serial adapter
static uint16_t g_port_base = COM1_BASE;

__PRIVILEGED_BSS static rx_callback_t g_rx_callback;

static void init_uart(uint16_t port, uint16_t baud_divisor) {
    portio::out8(port + REG_IER, 0x00);
    portio::out8(port + REG_LCR, LCR_DLAB);
    portio::out8(port + REG_DATA, static_cast<uint8_t>(baud_divisor & 0xFF));
    portio::out8(port + REG_IER, static_cast<uint8_t>((baud_divisor >> 8) & 0xFF));
    portio::out8(port + REG_LCR, LCR_8N1);
    portio::out8(port + REG_FCR, FCR_ENABLE | FCR_CLEAR_RX | FCR_CLEAR_TX | FCR_TRIGGER_14);
    portio::out8(port + REG_MCR, MCR_DTR | MCR_RTS | MCR_OUT2);
}

int32_t init() {
    init_uart(COM1_BASE, BAUD_115200);

    uint8_t lsr = portio::in8(COM1_BASE + REG_LSR);
    if (lsr == 0xFF) {
        return ERR_NO_DEVICE;
    }

    return OK;
}

void set_port(uint16_t port) {
    init_uart(port, BAUD_9600);
    g_port_base = port;
}

void write_char(char c) {
    while ((portio::in8(g_port_base + REG_LSR) & LSR_TX_EMPTY) == 0) {
        asm volatile ("pause");
    }
    portio::out8(g_port_base + REG_DATA, static_cast<uint8_t>(c));
}

void write(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        write_char(data[i]);
    }
}

int32_t read_char() {
    if ((portio::in8(g_port_base + REG_LSR) & LSR_DATA_READY) == 0) {
        return ERR_NO_DATA;
    }
    return portio::in8(g_port_base + REG_DATA);
}

int32_t remap() {
    return OK;
}

__PRIVILEGED_CODE void set_rx_callback(rx_callback_t cb) {
    g_rx_callback = cb;
}

__PRIVILEGED_CODE int32_t enable_rx_interrupt() {
    int32_t rc = ioapic::route_irq(COM1_LEGACY_IRQ, x86::VEC_SERIAL, 0);
    if (rc != ioapic::OK) {
        return rc;
    }
    portio::out8(g_port_base + REG_IER, IER_RX_AVAIL);
    return OK;
}

__PRIVILEGED_CODE void on_rx_irq() {
    while ((portio::in8(g_port_base + REG_LSR) & LSR_DATA_READY) != 0) {
        char c = static_cast<char>(portio::in8(g_port_base + REG_DATA));
        if (g_rx_callback) {
            g_rx_callback(c);
        }
    }
}

__PRIVILEGED_CODE uint32_t irq_id() {
    return x86::VEC_SERIAL;
}

} // namespace serial
