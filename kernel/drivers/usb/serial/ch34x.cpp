#include "drivers/usb/serial/ch34x.h"
#include "drivers/usb/serial/bridge.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/usb_descriptors.h"
#include "terminal/tty.h"

namespace usb::serial {

struct ch34x_product {
    uint16_t    id;
    const char* chip_name;
};

struct baud_clock {
    uint32_t hz;
    uint8_t  prescaler;
    uint32_t min_divisor; // the smallest divisor used with this clock
    uint32_t max_divisor; // the largest
};

struct speed_choice {
    const baud_clock* clock;
    uint32_t          divisor;
    uint32_t          rate;
};

static constexpr uint16_t WCH_VENDOR_ID     = 0x1A86;
static constexpr uint16_t WCH_OLD_VENDOR_ID = 0x4348;
static constexpr uint16_t CH340_PRODUCT_ID  = 0x7523;
static constexpr uint16_t CH340K_PRODUCT_ID = 0x7522;
static constexpr uint16_t CH341_PRODUCT_ID  = 0x5523;

static constexpr const char* FAMILY_NAME = "ch34x";

static constexpr ch34x_product PRODUCTS[] = {
    {CH340_PRODUCT_ID,  "ch340"},
    {CH340K_PRODUCT_ID, "ch340k"},
    {CH341_PRODUCT_ID,  "ch341"},
};

// The CH340 release whose line control register lays out its bits otherwise, so it keeps the chip's own 8N1 format,
// and whose prescaler register takes no unbuffered bit
static constexpr uint16_t OLD_CH340_RELEASE = 0x0250;

static constexpr uint8_t READ_REGISTERS  = 0x95;
static constexpr uint8_t WRITE_REGISTERS = 0x9A;
static constexpr uint8_t RESET           = 0xA1;
static constexpr uint8_t MODEM_CONTROL   = 0xA4;

// Register pairs that one request reads or writes, the first register's address in the low byte. A write's index
// holds the values the same way
static constexpr uint16_t SPEED_REGISTERS        = 0x1312; // prescaler, then divisor
static constexpr uint16_t LINE_CONTROL_REGISTERS = 0x2518; // line control, then a second one written as 0
static constexpr uint16_t BREAK_REGISTERS        = 0x1805; // break, then line control
static constexpr uint16_t STATUS_REGISTERS       = 0x0706; // modem status, then a second status register

static constexpr uint16_t REGISTER_PAIR_BYTES = 2;

// The chip divides one of these clocks, chosen by the prescaler register, by a divisor from the clock's min_divisor to
// its max_divisor, which the divisor register holds as 256 less the divisor
static constexpr baud_clock BAUD_CLOCKS[] = {
    {6000000,  0x03, 3,  256}, // up to 2,000,000 baud, the chip's fastest
    {12000000, 0x07, 13, 13},  // only 921,600 baud, the one rate this clock is known to make
    {750000,   0x02, 1,  256},
    {93750,    0x01, 1,  256},
    {11719,    0x00, 1,  256},
};

static constexpr uint32_t DIVISOR_REGISTER_BASE = 256;

// Set in the prescaler register so the chip hands over each received byte without first filling a packet
static constexpr uint8_t PRESCALER_UNBUFFERED = 0x80;

static constexpr uint8_t LCR_RECEIVER_ON    = 0x80;
static constexpr uint8_t LCR_TRANSMITTER_ON = 0x40;
static constexpr uint8_t LCR_PARITY_FIXED   = 0x20; // the parity bit always 1, or always 0 with LCR_PARITY_EVEN
static constexpr uint8_t LCR_PARITY_EVEN    = 0x10;
static constexpr uint8_t LCR_PARITY_ON      = 0x08;
static constexpr uint8_t LCR_TWO_STOP_BITS  = 0x04;
static constexpr uint8_t LCR_MIN_DATA_BITS  = 5;    // the data bits less this, in the low two bits

static constexpr uint8_t MODEM_CONTROL_DTR = 0x20;
static constexpr uint8_t MODEM_CONTROL_RTS = 0x40;

// Set in the break register while the chip sends no break
static constexpr uint8_t BREAK_IDLE = 0x01;

static const char* name_for_product(uint16_t product_id) {
    for (const ch34x_product& product : PRODUCTS) {
        if (product.id == product_id) {
            return product.chip_name;
        }
    }

    return PRODUCTS[0].chip_name;
}

static uint32_t clamp_divisor(const baud_clock& clock, uint64_t divisor) {
    if (divisor < clock.min_divisor) {
        return clock.min_divisor;
    }

    if (divisor > clock.max_divisor) {
        return clock.max_divisor;
    }

    return static_cast<uint32_t>(divisor);
}

// A tie keeps the earlier choice, so between equally near rates the clock listed first in BAUD_CLOCKS wins
static void consider_divisor(const baud_clock& clock, uint32_t divisor, uint32_t baud, speed_choice* nearest) {
    uint32_t rate = (clock.hz + divisor / 2) / divisor;
    if (!nearest->clock || distance(rate, baud) < distance(nearest->rate, baud)) {
        *nearest = {&clock, divisor, rate};
    }
}

// The speed registers' values for the rate nearest `baud`, as a register write's index. False when that rate is more
// than 3 percent off
static bool encode_speed(uint32_t baud, bool is_unbuffered, uint16_t* registers) {
    // Of a clock's rates, the two nearest `baud` come from the divisors on either side of the clock over `baud`
    speed_choice nearest = {};
    for (const baud_clock& clock : BAUD_CLOCKS) {
        uint64_t quotient = clock.hz / baud;
        consider_divisor(clock, clamp_divisor(clock, quotient), baud, &nearest);
        consider_divisor(clock, clamp_divisor(clock, quotient + 1), baud, &nearest);
    }

    if (!::serial::is_rate_within_tolerance(nearest.rate, baud)) {
        return false;
    }

    uint8_t prescaler = nearest.clock->prescaler | (is_unbuffered ? PRESCALER_UNBUFFERED : 0);
    uint8_t divisor_register = static_cast<uint8_t>(DIVISOR_REGISTER_BASE - nearest.divisor);
    *registers = static_cast<uint16_t>(prescaler | (divisor_register << 8));

    return true;
}

static uint8_t parity_bits(::serial::parity mode) {
    switch (mode) {
        case ::serial::parity::none:  return 0;
        case ::serial::parity::odd:   return LCR_PARITY_ON;
        case ::serial::parity::even:  return LCR_PARITY_ON | LCR_PARITY_EVEN;
        case ::serial::parity::mark:  return LCR_PARITY_ON | LCR_PARITY_FIXED;
        case ::serial::parity::space: return LCR_PARITY_ON | LCR_PARITY_FIXED | LCR_PARITY_EVEN;
    }

    return 0;
}

static bool is_8n1(const ::serial::line_settings& settings) {
    return settings.data_bits == 8 && settings.parity_mode == ::serial::parity::none && settings.stop_bits == 1;
}

ch34x::ch34x(device* dev, const interface& iface)
    : translator(dev, iface), m_chip_name(name_for_product(dev->pid)) {}

int32_t ch34x::configure() {
    return send_request(RESET, 0, 0);
}

const char* ch34x::driver_name() const {
    return FAMILY_NAME;
}

int32_t ch34x::set_line(const ::serial::line_settings& settings) {
    bool is_old_ch340 = m_device->bcd_device == OLD_CH340_RELEASE;
    if (settings.uses_rts_cts || (is_old_ch340 && !is_8n1(settings))) {
        return ::serial::ERR_UNSUPPORTED;
    }

    uint16_t speed = 0;
    if (!encode_speed(settings.baud, !is_old_ch340, &speed)) {
        return ::serial::ERR_UNSUPPORTED;
    }

    if (send_request(WRITE_REGISTERS, SPEED_REGISTERS, speed) != ::serial::OK) {
        return ::serial::ERR_IO;
    }

    if (is_old_ch340) {
        return ::serial::OK;
    }

    // The transmitter stays off through a break, which turns it off in this register
    uint8_t line_control = LCR_RECEIVER_ON | parity_bits(settings.parity_mode);
    line_control |= static_cast<uint8_t>(settings.data_bits - LCR_MIN_DATA_BITS);
    if (!m_is_breaking) {
        line_control |= LCR_TRANSMITTER_ON;
    }

    if (settings.stop_bits == 2) {
        line_control |= LCR_TWO_STOP_BITS;
    }

    return send_request(WRITE_REGISTERS, LINE_CONTROL_REGISTERS, line_control);
}

// The chip raises a line whose bit is clear
int32_t ch34x::set_modem_outputs(uint32_t lines) {
    uint8_t raised = 0;
    if (lines & terminal::TIOCM_DTR) {
        raised |= MODEM_CONTROL_DTR;
    }

    if (lines & terminal::TIOCM_RTS) {
        raised |= MODEM_CONTROL_RTS;
    }

    return send_request(MODEM_CONTROL, static_cast<uint8_t>(~raised), 0);
}

// The status register holds CTS, DSR, RI and CD in its low four bits, each clear while raised
int32_t ch34x::get_modem_inputs(uint32_t* lines) {
    uint8_t status[REGISTER_PAIR_BYTES];
    if (read_registers(STATUS_REGISTERS, status) != ::serial::OK) {
        return ::serial::ERR_IO;
    }

    *lines = modem_inputs_from_status(static_cast<uint8_t>(~status[0] << 4));
    return ::serial::OK;
}

// The pair holds the line control register too, so a break reads both and keeps the character format set_line wrote
int32_t ch34x::set_break(bool on) {
    uint8_t values[REGISTER_PAIR_BYTES];
    if (read_registers(BREAK_REGISTERS, values) != ::serial::OK) {
        return ::serial::ERR_IO;
    }

    uint8_t break_control = values[0];
    uint8_t line_control = values[1];
    if (on) {
        break_control &= static_cast<uint8_t>(~BREAK_IDLE);
        line_control &= static_cast<uint8_t>(~LCR_TRANSMITTER_ON);
    } else {
        break_control |= BREAK_IDLE;
        line_control |= LCR_TRANSMITTER_ON;
    }

    uint16_t written = static_cast<uint16_t>(break_control | (line_control << 8));
    if (send_request(WRITE_REGISTERS, BREAK_REGISTERS, written) != ::serial::OK) {
        return ::serial::ERR_IO;
    }

    m_is_breaking = on;
    return ::serial::OK;
}

ssize_t ch34x::tx_pending() {
    return ::serial::TX_UNKNOWN;
}

int32_t ch34x::send_request(uint8_t request, uint16_t value, uint16_t index) {
    int32_t rc = control_transfer(
        m_device,
        USB_REQTYPE_DIR_OUT | USB_REQTYPE_TYPE_VENDOR | USB_REQTYPE_RECIP_DEVICE,
        request,
        value,
        index,
        nullptr,
        0
    );

    return rc == 0 ? ::serial::OK : ::serial::ERR_IO;
}

int32_t ch34x::read_registers(uint16_t registers, uint8_t* values) {
    int32_t rc = control_transfer(
        m_device,
        USB_REQTYPE_DIR_IN | USB_REQTYPE_TYPE_VENDOR | USB_REQTYPE_RECIP_DEVICE,
        READ_REGISTERS,
        registers,
        0,
        values,
        REGISTER_PAIR_BYTES
    );

    return rc == 0 ? ::serial::OK : ::serial::ERR_IO;
}

REGISTER_USB_CLASS_DRIVER(ch34x,
    USB_MATCH_DEVICE(WCH_VENDOR_ID, CH340_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ch34x>);
REGISTER_USB_CLASS_DRIVER(ch34x,
    USB_MATCH_DEVICE(WCH_VENDOR_ID, CH340K_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ch34x>);
REGISTER_USB_CLASS_DRIVER(ch34x,
    USB_MATCH_DEVICE(WCH_VENDOR_ID, CH341_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ch34x>);
REGISTER_USB_CLASS_DRIVER(ch34x,
    USB_MATCH_DEVICE(WCH_OLD_VENDOR_ID, CH341_PRODUCT_ID, USB_MATCH_ANY, USB_MATCH_ANY, USB_MATCH_ANY),
    create_bridge<ch34x>);

} // namespace usb::serial
