#include "drivers/usb/serial/ftdi.h"
#include "drivers/usb/serial/bridge.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/usb_descriptors.h"
#include "common/logging.h"
#include "terminal/tty.h"

namespace usb::serial {

static constexpr uint16_t FTDI_VENDOR_ID = 0x0403;
static constexpr uint16_t FT232_PRODUCT_ID = 0x6001;  // FT232AM, FT232BM and FT232R
static constexpr uint16_t FT2232_PRODUCT_ID = 0x6010; // FT2232C, FT2232D and FT2232H
static constexpr uint16_t FT4232H_PRODUCT_ID = 0x6011;
static constexpr uint16_t FT232H_PRODUCT_ID = 0x6014;
static constexpr uint16_t FTX_PRODUCT_ID = 0x6015;    // the FT-X series, such as FT231X

static constexpr uint8_t REQUEST_RESET = 0x00;
static constexpr uint8_t REQUEST_SET_MODEM_CTRL = 0x01;
static constexpr uint8_t REQUEST_SET_FLOW_CTRL = 0x02;
static constexpr uint8_t REQUEST_SET_BAUD_RATE = 0x03;
static constexpr uint8_t REQUEST_SET_DATA = 0x04;
static constexpr uint8_t REQUEST_SET_LATENCY_TIMER = 0x09;

// Resets the UART and empties both of its buffers
static constexpr uint16_t RESET_UART = 0x0000;

// The chip sends what it has received once this many milliseconds pass without a full packet, rather than the 16 it
// starts with, so a reply that a flashing tool waits for arrives at once
static constexpr uint16_t LATENCY_TIMER_MS = 1;

// Every packet the chip sends starts with its modem status and line status bytes
static constexpr size_t STATUS_BYTES = 2;

// SET_DATA's value: data bits in bits 0 to 7, parity in bits 8 to 10 and stop bits in bits 11 to 13
static constexpr uint16_t PARITY_NONE = 0;
static constexpr uint16_t PARITY_ODD = 1;
static constexpr uint16_t PARITY_EVEN = 2;
static constexpr uint16_t PARITY_MARK = 3;
static constexpr uint16_t PARITY_SPACE = 4;
static constexpr uint16_t PARITY_SHIFT = 8;
static constexpr uint16_t ONE_STOP_BIT = 0;
static constexpr uint16_t TWO_STOP_BITS = 2;
static constexpr uint16_t STOP_BITS_SHIFT = 11;

// SET_MODEM_CTRL's value: each line's level, and a bit that lets the request change it
static constexpr uint16_t MODEM_CTRL_DTR = 1u << 0;
static constexpr uint16_t MODEM_CTRL_RTS = 1u << 1;
static constexpr uint16_t MODEM_CTRL_WRITE_BOTH = (1u << 8) | (1u << 9);

// The modem status byte starting each packet
static constexpr uint8_t MODEM_STATUS_CTS = 1u << 4;
static constexpr uint8_t MODEM_STATUS_DSR = 1u << 5;
static constexpr uint8_t MODEM_STATUS_RI = 1u << 6;
static constexpr uint8_t MODEM_STATUS_CD = 1u << 7;

// SET_FLOW_CTRL's index holds the flow control in its high byte and the UART in its low one
static constexpr uint16_t FLOW_RTS_CTS = 0x0100;

// The chip divides a 3 MHz clock by a whole number, in bits 0 to 13 of its divisor, plus a number of eighths, coded
// in bits 14 to 16 as EIGHTHS_CODES gives them
static constexpr uint32_t BAUD_CLOCK_HZ = 3000000;
static constexpr uint32_t EIGHTHS_SHIFT = 14;
static constexpr uint32_t MAX_EIGHTHS = (0x3FFF << 3) | 7;
static constexpr uint32_t DIVISOR_TOP_SHIFT = 16;
static constexpr uint32_t EIGHTHS_CODES[] = {0, 3, 2, 4, 1, 5, 6, 7};

// Below a divisor of 2 the chip divides only by 1, coded 0, and by 1.5, coded 1
static constexpr uint32_t DIVIDE_BY_1_EIGHTHS = 8;
static constexpr uint32_t DIVIDE_BY_1_5_EIGHTHS = 12;
static constexpr uint32_t DIVIDE_BY_2_EIGHTHS = 16;
static constexpr uint32_t DIVIDE_BY_1_CODE = 0;
static constexpr uint32_t DIVIDE_BY_1_5_CODE = 1;

// The eighths the FT232AM divides by: none, one, two and four
static constexpr uint32_t COARSE_EIGHTHS = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4);

static constexpr const char* FAMILY_NAME = "ftdi";

// FTDI chips sharing a product ID differ in the device release number
struct ftdi_chip {
    uint16_t    release;
    const char* name;
    bool        names_uart_in_baud_index; // SET_BAUD_RATE's index carries the UART, and the divisor's top bit above it
    bool        has_coarse_eighths;       // divides by COARSE_EIGHTHS only, and never by 1.5
};

static constexpr ftdi_chip CHIPS[] = {
    {.release = 0x0200, .name = "ft232am", .names_uart_in_baud_index = false, .has_coarse_eighths = true},
    {.release = 0x0400, .name = "ft232bm", .names_uart_in_baud_index = false, .has_coarse_eighths = false},
    {.release = 0x0500, .name = "ft2232c", .names_uart_in_baud_index = true, .has_coarse_eighths = false},
    {.release = 0x0600, .name = "ft232r", .names_uart_in_baud_index = false, .has_coarse_eighths = false},
    {.release = 0x0700, .name = "ft2232h", .names_uart_in_baud_index = true, .has_coarse_eighths = false},
    {.release = 0x0800, .name = "ft4232h", .names_uart_in_baud_index = true, .has_coarse_eighths = false},
    {.release = 0x0900, .name = "ft232h", .names_uart_in_baud_index = true, .has_coarse_eighths = false},
    {.release = 0x1000, .name = "ftx", .names_uart_in_baud_index = false, .has_coarse_eighths = false},
};

// A release the table lacks takes the family's name, and its rates are coded as an FT232BM's
static constexpr ftdi_chip UNLISTED_CHIP = {
    .release = 0,
    .name = FAMILY_NAME,
    .names_uart_in_baud_index = false,
    .has_coarse_eighths = false,
};

static const ftdi_chip* chip_for_release(uint16_t release) {
    for (const ftdi_chip& chip : CHIPS) {
        if (chip.release == release) {
            return &chip;
        }
    }

    return &UNLISTED_CHIP;
}

// The divisor of the rate nearest `baud`, false when the chip makes none within 3 percent of it
static bool find_divisor(const ftdi_chip& chip, uint32_t baud, uint32_t* divisor) {
    if (baud == 0) {
        return false;
    }

    uint32_t eighths = (8 * BAUD_CLOCK_HZ + baud / 2) / baud;
    if (eighths < DIVIDE_BY_1_EIGHTHS) {
        eighths = DIVIDE_BY_1_EIGHTHS;
    } else if (eighths > MAX_EIGHTHS) {
        eighths = MAX_EIGHTHS;
    }

    bool is_whole_or_special = eighths >= DIVIDE_BY_2_EIGHTHS ||
                               eighths == DIVIDE_BY_1_EIGHTHS ||
                               (eighths == DIVIDE_BY_1_5_EIGHTHS && !chip.has_coarse_eighths);
    bool has_eighth = !chip.has_coarse_eighths || (COARSE_EIGHTHS & (1u << (eighths & 7))) != 0;
    uint32_t made = (8 * BAUD_CLOCK_HZ + eighths / 2) / eighths;
    if (!is_whole_or_special || !has_eighth || !::serial::is_rate_within_tolerance(made, baud)) {
        return false;
    }

    if (eighths == DIVIDE_BY_1_EIGHTHS) {
        *divisor = DIVIDE_BY_1_CODE;
    } else if (eighths == DIVIDE_BY_1_5_EIGHTHS) {
        *divisor = DIVIDE_BY_1_5_CODE;
    } else {
        *divisor = (eighths >> 3) | EIGHTHS_CODES[eighths & 7] << EIGHTHS_SHIFT;
    }

    return true;
}

static uint16_t parity_code(::serial::parity mode) {
    switch (mode) {
        case ::serial::parity::none:  return PARITY_NONE;
        case ::serial::parity::odd:   return PARITY_ODD;
        case ::serial::parity::even:  return PARITY_EVEN;
        case ::serial::parity::mark:  return PARITY_MARK;
        case ::serial::parity::space: return PARITY_SPACE;
    }

    return PARITY_NONE;
}

ftdi::ftdi(device* dev, const interface& iface) : translator(dev, iface), m_chip(&UNLISTED_CHIP) {
    m_modem_status.store_relaxed(0);
}

int32_t ftdi::configure() {
    if (send_request(REQUEST_RESET, RESET_UART, uart_number()) != 0) {
        return ::serial::ERR_IO;
    }

    m_chip = chip_for_release(m_device->bcd_device);

    return ::serial::OK;
}

const char* ftdi::chip_name() const {
    return m_chip->name;
}

const char* ftdi::driver_name() const {
    return FAMILY_NAME;
}

// A chip with several UARTs has an interface for each
int8_t ftdi::channel() const {
    return m_device->num_interfaces > 1 ? static_cast<int8_t>(m_interface->interface_number) : -1;
}

void ftdi::handle_received(::serial::port* p, const uint8_t* data, size_t length, uint16_t packet_size) {
    for (size_t offset = 0; offset < length; offset += packet_size) {
        size_t packet = length - offset < packet_size ? length - offset : packet_size;

        // The modem status byte of the read's last packet is the latest the chip has given
        m_modem_status.store_relaxed(data[offset]);
        if (packet > STATUS_BYTES) {
            ::serial::receive(p, data + offset + STATUS_BYTES, packet - STATUS_BYTES);
        }
    }
}

// The chip makes 7 or 8 data bits only. The first call, from the first open, also shortens the latency timer, since
// configure() runs on the HCD task and sends only what must come before reads are posted
int32_t ftdi::set_line(const ::serial::line_settings& settings) {
    uint32_t divisor = 0;
    bool is_rate_made = find_divisor(*m_chip, settings.baud, &divisor);
    bool is_size_made = settings.data_bits == 7 || settings.data_bits == 8;
    if (!is_rate_made || !is_size_made) {
        return ::serial::ERR_UNSUPPORTED;
    }

    if (!m_has_asked_for_latency_timer) {
        m_has_asked_for_latency_timer = true;
        ask_for_latency_timer();
    }

    uint16_t divisor_top = static_cast<uint16_t>(divisor >> DIVISOR_TOP_SHIFT);
    uint16_t baud_index = m_chip->names_uart_in_baud_index
        ? static_cast<uint16_t>(divisor_top << 8 | uart_number())
        : divisor_top;
    uint16_t stop_bits = settings.stop_bits == 2 ? TWO_STOP_BITS : ONE_STOP_BIT;
    uint16_t data = static_cast<uint16_t>(settings.data_bits) |
                    static_cast<uint16_t>(parity_code(settings.parity_mode) << PARITY_SHIFT) |
                    static_cast<uint16_t>(stop_bits << STOP_BITS_SHIFT);
    uint16_t flow_index = static_cast<uint16_t>((settings.uses_rts_cts ? FLOW_RTS_CTS : 0) | uart_number());

    if (send_request(REQUEST_SET_BAUD_RATE, static_cast<uint16_t>(divisor), baud_index) != 0 ||
        send_request(REQUEST_SET_DATA, data, uart_number()) != 0 ||
        send_request(REQUEST_SET_FLOW_CTRL, 0, flow_index) != 0) {
        return ::serial::ERR_IO;
    }

    return ::serial::OK;
}

int32_t ftdi::set_modem_outputs(uint32_t lines) {
    uint16_t value = MODEM_CTRL_WRITE_BOTH;
    if (lines & terminal::TIOCM_DTR) {
        value |= MODEM_CTRL_DTR;
    }

    if (lines & terminal::TIOCM_RTS) {
        value |= MODEM_CTRL_RTS;
    }

    return send_request(REQUEST_SET_MODEM_CTRL, value, uart_number()) == 0 ? ::serial::OK : ::serial::ERR_IO;
}

// The chip reports its modem inputs at the start of every packet, so they need no request
int32_t ftdi::get_modem_inputs(uint32_t* lines) {
    uint32_t status = m_modem_status.load_relaxed();
    uint32_t inputs = 0;
    if (status & MODEM_STATUS_CTS) {
        inputs |= terminal::TIOCM_CTS;
    }

    if (status & MODEM_STATUS_DSR) {
        inputs |= terminal::TIOCM_DSR;
    }

    if (status & MODEM_STATUS_RI) {
        inputs |= terminal::TIOCM_RNG;
    }

    if (status & MODEM_STATUS_CD) {
        inputs |= terminal::TIOCM_CAR;
    }

    *lines = inputs;
    return ::serial::OK;
}

// The oldest chips may refuse the request, and still pass bytes, only up to 16 ms later
void ftdi::ask_for_latency_timer() {
    if (send_request(REQUEST_SET_LATENCY_TIMER, LATENCY_TIMER_MS, uart_number()) != 0) {
        log::warn("ftdi: %s refused a %u ms latency timer", m_chip->name, static_cast<uint32_t>(LATENCY_TIMER_MS));
    }
}

// FTDI numbers a chip's UARTs from 1 in each vendor request's index
uint16_t ftdi::uart_number() const {
    return static_cast<uint16_t>(m_interface->interface_number + 1);
}

int32_t ftdi::send_request(uint8_t request, uint16_t value, uint16_t index) {
    return control_transfer(
        m_device,
        USB_REQTYPE_DIR_OUT | USB_REQTYPE_TYPE_VENDOR | USB_REQTYPE_RECIP_DEVICE,
        request,
        value,
        index,
        nullptr,
        0
    );
}

REGISTER_USB_CLASS_DRIVER(ftdi,
    USB_MATCH_DEVICE(FTDI_VENDOR_ID, FT232_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ftdi>);
REGISTER_USB_CLASS_DRIVER(ftdi,
    USB_MATCH_DEVICE(FTDI_VENDOR_ID, FT2232_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ftdi>);
REGISTER_USB_CLASS_DRIVER(ftdi,
    USB_MATCH_DEVICE(FTDI_VENDOR_ID, FT4232H_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ftdi>);
REGISTER_USB_CLASS_DRIVER(ftdi,
    USB_MATCH_DEVICE(FTDI_VENDOR_ID, FT232H_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ftdi>);
REGISTER_USB_CLASS_DRIVER(ftdi,
    USB_MATCH_DEVICE(FTDI_VENDOR_ID, FTX_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ftdi>);

} // namespace usb::serial
