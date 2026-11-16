#include "drivers/serial/uart/ns16550.h"
#include "hw/cpu.h"
#include "hw/ioport.h"

namespace serial::uart {

constexpr uint32_t DIVISOR_MAX = 0xFFFF;

// Register offsets from base
constexpr uint16_t REG_DATA = 0;        // Data register (read/write)
constexpr uint16_t REG_IER = 1;         // Interrupt enable register
constexpr uint16_t REG_FCR = 2;         // FIFO control register
constexpr uint16_t REG_LCR = 3;         // Line control register
constexpr uint16_t REG_MCR = 4;         // Modem control register
constexpr uint16_t REG_LSR = 5;         // Line status register
constexpr uint16_t REG_DLL = 0;         // Divisor low byte, while DLAB is set
constexpr uint16_t REG_DLM = 1;         // Divisor high byte, while DLAB is set

// Line Status Register bits
constexpr uint8_t LSR_DATA_READY = 0x01;  // Data available to read
constexpr uint8_t LSR_TX_EMPTY = 0x20;    // Transmit buffer empty
constexpr uint8_t LSR_NO_DEVICE = 0xFF;   // What an I/O port with no chip behind it reads

// Line Control Register bits
constexpr uint8_t LCR_STOP_BITS_2 = 0x04; // Two stop bits
constexpr uint8_t LCR_PARITY = 0x08;      // Parity enable
constexpr uint8_t LCR_PARITY_EVEN = 0x10; // Even parity
constexpr uint8_t LCR_DLAB = 0x80;        // Divisor latch access bit

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
constexpr uint8_t IER_RX_AVAIL = 0x01; // Received data available

int32_t ns16550::divisor_for(uint32_t clock_hz, uint32_t baud) {
    if (baud == 0) {
        return ERR_UNSUPPORTED;
    }

    uint64_t divisor = baud_divisor(clock_hz, baud, 0);
    bool fits_latch = divisor >= 1 && divisor <= DIVISOR_MAX;
    if (!fits_latch || !is_within_tolerance(clock_hz, baud, static_cast<uint32_t>(divisor))) {
        return ERR_UNSUPPORTED;
    }

    return static_cast<int32_t>(divisor);
}

int32_t ns16550::line_control_for(const settings& line) {
    bool has_data_bits = line.data_bits >= DATA_BITS_MIN && line.data_bits <= DATA_BITS_MAX;
    bool has_stop_bits = line.stop_bits == 1 || (line.stop_bits == 2 && line.data_bits > DATA_BITS_MIN);
    if (!has_data_bits || !has_stop_bits) {
        return ERR_UNSUPPORTED;
    }

    uint8_t word_length = static_cast<uint8_t>(line.data_bits - DATA_BITS_MIN);
    uint8_t stop_bits = line.stop_bits == 2 ? LCR_STOP_BITS_2 : 0;
    uint8_t parity_enable = line.parity_mode != parity::none ? LCR_PARITY : 0;
    uint8_t parity_even = line.parity_mode == parity::even ? LCR_PARITY_EVEN : 0;

    return word_length | stop_bits | parity_enable | parity_even;
}

__PRIVILEGED_CODE int32_t ns16550::init(const settings& line) {
    int32_t divisor = divisor_for(m_clock_hz, line.baud);
    int32_t line_control = line_control_for(line);
    if (divisor < 0 || line_control < 0) {
        return ERR_UNSUPPORTED;
    }

    ioport::write8(m_port + REG_IER, 0);

    ioport::write8(m_port + REG_LCR, LCR_DLAB);
    ioport::write8(m_port + REG_DLL, static_cast<uint8_t>(divisor));
    ioport::write8(m_port + REG_DLM, static_cast<uint8_t>(divisor >> 8));

    ioport::write8(m_port + REG_LCR, static_cast<uint8_t>(line_control));
    ioport::write8(m_port + REG_FCR, FCR_ENABLE | FCR_CLEAR_RX | FCR_CLEAR_TX | FCR_TRIGGER_14);
    ioport::write8(m_port + REG_MCR, MCR_DTR | MCR_RTS | MCR_OUT2);

    if (ioport::read8(m_port + REG_LSR) == LSR_NO_DEVICE) {
        return ERR_NO_DEVICE;
    }

    return OK;
}

// I/O ports need no mapping
__PRIVILEGED_CODE int32_t ns16550::remap_registers() {
    return OK;
}

__PRIVILEGED_CODE int32_t ns16550::enable_receive_interrupt(receive_callback on_receive, void* context) {
    if (!m_route_receive_interrupt || m_route_receive_interrupt() != OK) {
        return ERR_UNSUPPORTED;
    }

    m_on_receive = on_receive;
    m_receive_context = context;
    ioport::write8(m_port + REG_IER, IER_RX_AVAIL);

    return OK;
}

__PRIVILEGED_CODE void ns16550::handle_receive_interrupt() {
    while ((ioport::read8(m_port + REG_LSR) & LSR_DATA_READY) != 0) {
        uint8_t byte = ioport::read8(m_port + REG_DATA);
        if (m_on_receive) {
            m_on_receive(m_receive_context, byte);
        }
    }
}

__PRIVILEGED_CODE void ns16550::write(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        while ((ioport::read8(m_port + REG_LSR) & LSR_TX_EMPTY) == 0) {
            cpu::pause();
        }

        ioport::write8(m_port + REG_DATA, static_cast<uint8_t>(data[i]));
    }
}

} // namespace serial::uart
