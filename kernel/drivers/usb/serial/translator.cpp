#include "drivers/usb/serial/translator.h"
#include "drivers/usb/usb_descriptors.h"
#include "terminal/tty.h"

namespace usb::serial {

static constexpr uint8_t MODEM_STATUS_CTS = 1u << 4;
static constexpr uint8_t MODEM_STATUS_DSR = 1u << 5;
static constexpr uint8_t MODEM_STATUS_RI = 1u << 6;
static constexpr uint8_t MODEM_STATUS_CD = 1u << 7;

void translator::handle_received(::serial::port* p, const uint8_t* data, size_t length, uint16_t) {
    ::serial::receive(p, data, length);
}

static const endpoint* find_endpoint(const interface& iface, uint8_t transfer_type, bool is_in) {
    for (uint8_t i = 0; i < iface.num_endpoints; i++) {
        const endpoint& ep = iface.endpoints[i];
        if (ep.transfer_type == transfer_type && ep.is_in() == is_in && ep.max_packet_size > 0) {
            return &ep;
        }
    }

    return nullptr;
}

const endpoint* find_bulk_in(const interface& iface) {
    return find_endpoint(iface, USB_ENDPOINT_TYPE_BULK, true);
}

const endpoint* find_bulk_out(const interface& iface) {
    return find_endpoint(iface, USB_ENDPOINT_TYPE_BULK, false);
}

const endpoint* find_interrupt_in(const interface& iface) {
    return find_endpoint(iface, USB_ENDPOINT_TYPE_INTERRUPT, true);
}

uint32_t modem_inputs_from_status(uint8_t status) {
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

    return inputs;
}

void store_le32(uint8_t* bytes, uint32_t value) {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
    bytes[2] = static_cast<uint8_t>(value >> 16);
    bytes[3] = static_cast<uint8_t>(value >> 24);
}

} // namespace usb::serial
