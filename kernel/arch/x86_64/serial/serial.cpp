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

// PCI Configuration Mechanism 1 (0xCF8/0xCFC) for early boot device setup.
// Used before pci::init() to enable the PCI serial adapter.
constexpr uint16_t PCI_CONFIG_ADDR = 0x0CF8;
constexpr uint16_t PCI_CONFIG_DATA = 0x0CFC;

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    portio::out32(PCI_CONFIG_ADDR, 0x80000000u
        | (static_cast<uint32_t>(bus) << 16)
        | (static_cast<uint32_t>(dev) << 11)
        | (static_cast<uint32_t>(fn) << 8)
        | (reg & 0xFCu));
    return portio::in32(PCI_CONFIG_DATA);
}

static void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val) {
    portio::out32(PCI_CONFIG_ADDR, 0x80000000u
        | (static_cast<uint32_t>(bus) << 16)
        | (static_cast<uint32_t>(dev) << 11)
        | (static_cast<uint32_t>(fn) << 8)
        | (reg & 0xFCu));
    portio::out32(PCI_CONFIG_DATA, val);
}

// MCS9922 PCI location (lspci: 05:00.0) and identity
constexpr uint8_t  SERIAL_PCI_BUS = 0x05;
constexpr uint8_t  SERIAL_PCI_DEV = 0x00;
constexpr uint8_t  SERIAL_PCI_FN  = 0x00;
constexpr uint16_t MOSCHIP_VENDOR = 0x9710;

constexpr uint8_t PCI_REG_ID  = 0x00;
constexpr uint8_t PCI_REG_CMD = 0x04;
constexpr uint8_t PCI_REG_BAR0 = 0x10;
constexpr uint16_t PCI_CMD_IO = 0x0001;
constexpr uint16_t PCI_CMD_MEM = 0x0002;
constexpr uint16_t PCI_CMD_BM = 0x0004;

// Probe the MCS9922 via raw config space: verify vendor ID, enable I/O
// space in the Command register, and return BAR0's I/O port address.
// Returns 0 on failure (wrong device / not an I/O BAR).
static uint16_t enable_pci_serial() {
    uint32_t id = pci_cfg_read32(SERIAL_PCI_BUS, SERIAL_PCI_DEV, SERIAL_PCI_FN, PCI_REG_ID);
    if ((id & 0xFFFF) != MOSCHIP_VENDOR)
        return 0;

    uint32_t cmd_status = pci_cfg_read32(SERIAL_PCI_BUS, SERIAL_PCI_DEV, SERIAL_PCI_FN, PCI_REG_CMD);
    uint16_t cmd = static_cast<uint16_t>(cmd_status & 0xFFFF);
    cmd |= PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_BM;
    pci_cfg_write32(SERIAL_PCI_BUS, SERIAL_PCI_DEV, SERIAL_PCI_FN, PCI_REG_CMD,
                    (cmd_status & 0xFFFF0000u) | cmd);

    uint32_t bar0 = pci_cfg_read32(SERIAL_PCI_BUS, SERIAL_PCI_DEV, SERIAL_PCI_FN, PCI_REG_BAR0);
    if ((bar0 & 0x1) == 0)
        return 0;

    return static_cast<uint16_t>(bar0 & ~0x3u);
}

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
    uint16_t pci_port = enable_pci_serial();
    if (pci_port != 0)
        g_port_base = pci_port;

    init_uart(g_port_base, BAUD_115200);

    uint8_t lsr = portio::in8(g_port_base + REG_LSR);
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
    if (g_port_base != COM1_BASE) {
        // PCI serial adapters use PCI interrupt lines, not legacy IRQ 4.
        // RX interrupt routing for PCI UARTs is not supported.
        return ERR_NO_DEVICE;
    }
    int32_t rc = ioapic::route_irq(COM1_LEGACY_IRQ, x86::VEC_SERIAL, 0);
    if (rc != ioapic::OK) {
        return rc;
    }
    portio::out8(COM1_BASE + REG_IER, IER_RX_AVAIL);
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
