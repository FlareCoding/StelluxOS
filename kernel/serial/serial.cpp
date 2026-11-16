#include "serial/serial.h"
#include "arch/arch_uart.h"
#include "drivers/serial/uart/ns16550.h"
#include "hw/ioport.h"

namespace serial {

constexpr uart::settings BOOT_LINE = {
    .baud        = 115200,
    .data_bits   = 8,
    .parity_mode = uart::parity::none,
    .stop_bits   = 1,
};

// PCI serial adapters use PCI interrupt lines, not legacy IRQ 4.
// RX interrupt routing for PCI UARTs is not supported.
__PRIVILEGED_DATA constinit static uart::ns16550 g_pci_card(0, uart::NS16550_STANDARD_CLOCK_HZ, nullptr);

// The UART output goes to, the boot UART until set_port moves it
__PRIVILEGED_BSS static uart::device* g_uart;

__PRIVILEGED_BSS static rx_callback_t g_rx_callback;

__PRIVILEGED_CODE static void forward_received_byte(void*, uint8_t byte) {
    if (g_rx_callback) {
        g_rx_callback(static_cast<char>(byte));
    }
}

int32_t init() {
    uart::device* builtin = arch::builtin_uart();
    if (builtin->init(BOOT_LINE) != uart::OK) {
        return ERR_NO_DEVICE;
    }

    g_uart = builtin;

    return OK;
}

int32_t remap() {
    if (g_uart->remap_registers() != uart::OK) {
        return ERR_NO_DEVICE;
    }

    return OK;
}

void write_char(char c) {
    write(&c, 1);
}

void write(const char* data, size_t len) {
    if (!g_uart) {
        return;
    }

    g_uart->write(data, len);
}

void set_port(uint16_t port) {
    if (!ioport::available()) {
        return;
    }

    g_pci_card = uart::ns16550(port, uart::NS16550_STANDARD_CLOCK_HZ, nullptr);
    if (g_pci_card.init(BOOT_LINE) == uart::OK) {
        g_uart = &g_pci_card;
    }
}

__PRIVILEGED_CODE void set_rx_callback(rx_callback_t cb) {
    g_rx_callback = cb;
}

__PRIVILEGED_CODE int32_t enable_rx_interrupt() {
    if (g_uart->enable_receive_interrupt(forward_received_byte, nullptr) != uart::OK) {
        return ERR_NO_DEVICE;
    }

    return OK;
}

// The receive interrupt the trap handler delivers belongs to the boot UART
__PRIVILEGED_CODE void on_rx_irq() {
    arch::builtin_uart()->handle_receive_interrupt();
}

} // namespace serial
