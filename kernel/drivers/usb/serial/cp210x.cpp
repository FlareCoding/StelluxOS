#include "drivers/usb/serial/cp210x.h"
#include "drivers/usb/serial/bridge.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/usb_descriptors.h"
#include "terminal/tty.h"

namespace usb::serial {

// How a part makes its rates
enum class rate_source : uint8_t {
    rate_table,    // asked only for the rates RATE_TABLE lists
    clock_divider, // any rate in range, from a divider the chip picks itself
};

struct cp210x_part {
    uint8_t     number; // what the part number request answers
    const char* name;
    rate_source rates;
    uint32_t    max_baud;
    uint32_t    max_short_character_baud; // the fastest rate with 5 or 6 data bits
};

static constexpr const char* FAMILY_NAME = "cp210x";

static constexpr uint16_t SILABS_VENDOR_ID = 0x10C4;
static constexpr uint16_t CP210X_PRODUCT_ID = 0xEA60; // the default of every part with one UART

static constexpr uint8_t REQUEST_IFC_ENABLE = 0x00;
static constexpr uint8_t REQUEST_SET_LINE_CTL = 0x03;
static constexpr uint8_t REQUEST_SET_BREAK = 0x05;
static constexpr uint8_t REQUEST_SET_MHS = 0x07;
static constexpr uint8_t REQUEST_GET_MDMSTS = 0x08;
static constexpr uint8_t REQUEST_GET_COMM_STATUS = 0x10;
static constexpr uint8_t REQUEST_SET_FLOW = 0x13;
static constexpr uint8_t REQUEST_SET_BAUDRATE = 0x1E;
static constexpr uint8_t REQUEST_VENDOR_SPECIFIC = 0xFF;

static constexpr uint16_t UART_ENABLED = 1;
static constexpr uint16_t BREAK_ON = 1;
static constexpr uint16_t BREAK_OFF = 0;
static constexpr uint16_t GET_PART_NUMBER = 0x370B; // a VENDOR_SPECIFIC value

// SET_LINE_CTL's value: stop bits in bits 0 to 3, parity in bits 4 to 7 and data bits in bits 8 to 15
static constexpr uint16_t ONE_STOP_BIT = 0;
static constexpr uint16_t TWO_STOP_BITS = 2;
static constexpr uint16_t PARITY_NONE = 0;
static constexpr uint16_t PARITY_ODD = 1;
static constexpr uint16_t PARITY_EVEN = 2;
static constexpr uint16_t PARITY_MARK = 3;
static constexpr uint16_t PARITY_SPACE = 4;
static constexpr uint16_t PARITY_SHIFT = 4;
static constexpr uint16_t DATA_BITS_SHIFT = 8;

// SET_MHS's value: each line's level, and a bit that lets the request change it
static constexpr uint16_t MHS_DTR = 1u << 0;
static constexpr uint16_t MHS_RTS = 1u << 1;
static constexpr uint16_t MHS_WRITE_DTR = 1u << 8;
static constexpr uint16_t MHS_WRITE_RTS = 1u << 9;

// SET_FLOW's data is four little-endian words: DTR's mode and whether CTS gates sending, RTS's mode, then the XON and
// XOFF thresholds, which only XON and XOFF flow control uses
static constexpr uint32_t FLOW_DTR_ACTIVE = 1u << 0;
static constexpr uint32_t FLOW_CTS_HANDSHAKE = 1u << 3;
static constexpr uint32_t FLOW_RTS_ACTIVE = 1u << 6;
static constexpr uint32_t FLOW_RTS_RECEIVE_HANDSHAKE = 2u << 6; // the chip lowers RTS while its buffer is nearly full
static constexpr uint16_t FLOW_LENGTH = 16;
static constexpr size_t FLOW_RTS_OFFSET = 4;

// GET_COMM_STATUS's reply, whose fourth word counts the bytes waiting in the chip to be sent
static constexpr uint16_t COMM_STATUS_LENGTH = 19;
static constexpr size_t BYTES_TO_SEND_OFFSET = 12;

// Every part makes 24 MHz over the whole divider nearest the rate it is asked for, and a prescaler at the lowest rates
// changes that by under 0.01 percent
static constexpr uint32_t HALF_CLOCK_HZ = 24000000;

// The slowest rate a clock-divider part is asked for
static constexpr uint32_t MIN_BAUD = 300;

static constexpr uint32_t RATE_TABLE[] = {
    300,    600,    1200,   1800,   2400,   4000,   4800,   7200,   9600,   14400,
    16000,  19200,  28800,  38400,  51200,  56000,  57600,  64000,  76800,  115200,
    128000, 153600, 230400, 250000, 256000, 460800, 500000, 576000, 921600,
};

static constexpr cp210x_part PARTS[] = {
    {.number = 0x02, .name = "cp2102", .rates = rate_source::rate_table, .max_baud = 921600,
     .max_short_character_baud = 576000},
    {.number = 0x03, .name = "cp2103", .rates = rate_source::rate_table, .max_baud = 921600,
     .max_short_character_baud = 576000},
    {.number = 0x04, .name = "cp2104", .rates = rate_source::clock_divider, .max_baud = 2000000,
     .max_short_character_baud = 921600},
    {.number = 0x09, .name = "cp2109", .rates = rate_source::rate_table, .max_baud = 921600,
     .max_short_character_baud = 576000},
    {.number = 0x20, .name = "cp2102n", .rates = rate_source::clock_divider, .max_baud = 3000000,
     .max_short_character_baud = 3000000},
    {.number = 0x21, .name = "cp2102n", .rates = rate_source::clock_divider, .max_baud = 3000000,
     .max_short_character_baud = 3000000},
    {.number = 0x22, .name = "cp2102n", .rates = rate_source::clock_divider, .max_baud = 3000000,
     .max_short_character_baud = 3000000},
};

// A part the table lacks, the CP2101 among them, takes the family's name and a CP2102's rates, which every listed part
// makes
static constexpr cp210x_part UNLISTED_PART = {
    .number = 0,
    .name = FAMILY_NAME,
    .rates = rate_source::rate_table,
    .max_baud = 921600,
    .max_short_character_baud = 576000,
};

static const cp210x_part* part_for_number(uint8_t number) {
    for (const cp210x_part& part : PARTS) {
        if (part.number == number) {
            return &part;
        }
    }

    return &UNLISTED_PART;
}

static uint32_t nearest_table_rate(uint32_t baud) {
    uint32_t nearest = RATE_TABLE[0];
    for (uint32_t rate : RATE_TABLE) {
        if (distance(rate, baud) < distance(nearest, baud)) {
            nearest = rate;
        }
    }

    return nearest;
}

// A clock-divider part picks its own divider, so it is asked for the rate wanted, within its range
static uint32_t rate_to_ask(const cp210x_part& part, uint32_t baud) {
    if (part.rates == rate_source::rate_table) {
        return nearest_table_rate(baud);
    }

    if (baud < MIN_BAUD) {
        return MIN_BAUD;
    }

    return baud > part.max_baud ? part.max_baud : baud;
}

static uint32_t rate_made(uint32_t asked) {
    uint32_t divider = (HALF_CLOCK_HZ + asked / 2) / asked;
    return HALF_CLOCK_HZ / divider;
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

static uint32_t load_le32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           static_cast<uint32_t>(bytes[1]) << 8 |
           static_cast<uint32_t>(bytes[2]) << 16 |
           static_cast<uint32_t>(bytes[3]) << 24;
}

cp210x::cp210x(device* dev, const interface& iface) : translator(dev, iface), m_part(&UNLISTED_PART) {}

// A chip that refuses the part number request is named for the family. The UART is enabled last, since the chip
// resends its last packet while it receives with no read posted
int32_t cp210x::configure() {
    uint8_t number = 0;
    if (read_request(REQUEST_VENDOR_SPECIFIC, GET_PART_NUMBER, &number, sizeof(number)) == 0) {
        m_part = part_for_number(number);
    }

    return send_request(REQUEST_IFC_ENABLE, UART_ENABLED, nullptr, 0) == 0 ? ::serial::OK : ::serial::ERR_IO;
}

const char* cp210x::chip_name() const {
    return m_part->name;
}

const char* cp210x::driver_name() const {
    return FAMILY_NAME;
}

int32_t cp210x::set_line(const ::serial::line_settings& settings) {
    uint32_t asked = rate_to_ask(*m_part, settings.baud);
    uint32_t made = rate_made(asked);
    bool is_rate_made = ::serial::is_rate_within_tolerance(made, settings.baud);
    bool is_too_fast_for_size = settings.data_bits < 7 && asked > m_part->max_short_character_baud;
    if (!is_rate_made || is_too_fast_for_size) {
        return ::serial::ERR_UNSUPPORTED;
    }

    uint8_t rate[sizeof(uint32_t)];
    store_le32(rate, asked);

    uint16_t stop_bits = settings.stop_bits == 2 ? TWO_STOP_BITS : ONE_STOP_BIT;
    uint16_t line_control = static_cast<uint16_t>(settings.data_bits << DATA_BITS_SHIFT) |
                            static_cast<uint16_t>(parity_code(settings.parity_mode) << PARITY_SHIFT) |
                            stop_bits;

    if (send_request(REQUEST_SET_BAUDRATE, 0, rate, sizeof(rate)) != 0 ||
        send_request(REQUEST_SET_LINE_CTL, line_control, nullptr, 0) != 0 ||
        send_flow_control(settings.uses_rts_cts) != 0) {
        return ::serial::ERR_IO;
    }

    m_uses_rts_cts = settings.uses_rts_cts;
    return ::serial::OK;
}

// Under flow control by CTS the chip drives RTS itself, so the request leaves RTS alone
int32_t cp210x::set_modem_outputs(uint32_t lines) {
    uint16_t value = MHS_WRITE_DTR;
    if (lines & terminal::TIOCM_DTR) {
        value |= MHS_DTR;
    }

    if (!m_uses_rts_cts) {
        value |= MHS_WRITE_RTS;
        if (lines & terminal::TIOCM_RTS) {
            value |= MHS_RTS;
        }
    }

    if (send_request(REQUEST_SET_MHS, value, nullptr, 0) != 0) {
        return ::serial::ERR_IO;
    }

    m_modem_outputs = lines;
    return ::serial::OK;
}

int32_t cp210x::get_modem_inputs(uint32_t* lines) {
    uint8_t status = 0;
    if (read_request(REQUEST_GET_MDMSTS, 0, &status, sizeof(status)) != 0) {
        return ::serial::ERR_IO;
    }

    *lines = modem_inputs_from_status(status);
    return ::serial::OK;
}

int32_t cp210x::set_break(bool on) {
    uint16_t value = on ? BREAK_ON : BREAK_OFF;
    return send_request(REQUEST_SET_BREAK, value, nullptr, 0) == 0 ? ::serial::OK : ::serial::ERR_IO;
}

ssize_t cp210x::tx_pending() {
    uint8_t status[COMM_STATUS_LENGTH] = {};
    if (read_request(REQUEST_GET_COMM_STATUS, 0, status, sizeof(status)) != 0) {
        return ::serial::ERR_IO;
    }

    return static_cast<ssize_t>(load_le32(&status[BYTES_TO_SEND_OFFSET]));
}

int32_t cp210x::send_request(uint8_t request, uint16_t value, void* data, uint16_t length) {
    return control_transfer(
        m_device,
        USB_REQTYPE_DIR_OUT | USB_REQTYPE_TYPE_VENDOR | USB_REQTYPE_RECIP_INTERFACE,
        request,
        value,
        m_interface->interface_number,
        data,
        length
    );
}

int32_t cp210x::read_request(uint8_t request, uint16_t value, void* reply, uint16_t length) {
    return control_transfer(
        m_device,
        USB_REQTYPE_DIR_IN | USB_REQTYPE_TYPE_VENDOR | USB_REQTYPE_RECIP_INTERFACE,
        request,
        value,
        m_interface->interface_number,
        reply,
        length
    );
}

// SET_FLOW also sets DTR and RTS, so it sets them as last set, and under flow control by CTS hands RTS to the chip
int32_t cp210x::send_flow_control(bool uses_rts_cts) {
    uint32_t handshake = (m_modem_outputs & terminal::TIOCM_DTR) ? FLOW_DTR_ACTIVE : 0;
    uint32_t rts_mode = (m_modem_outputs & terminal::TIOCM_RTS) ? FLOW_RTS_ACTIVE : 0;
    if (uses_rts_cts) {
        handshake |= FLOW_CTS_HANDSHAKE;
        rts_mode = FLOW_RTS_RECEIVE_HANDSHAKE;
    }

    uint8_t flow[FLOW_LENGTH] = {};
    store_le32(flow, handshake);
    store_le32(&flow[FLOW_RTS_OFFSET], rts_mode);
    return send_request(REQUEST_SET_FLOW, 0, flow, sizeof(flow));
}

REGISTER_USB_CLASS_DRIVER(cp210x,
    USB_MATCH_DEVICE(SILABS_VENDOR_ID, CP210X_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<cp210x>);

} // namespace usb::serial
