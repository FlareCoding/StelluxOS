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

static const endpoint* find_bulk(const interface& iface, bool is_in) {
    for (uint8_t i = 0; i < iface.num_endpoints; i++) {
        const endpoint& ep = iface.endpoints[i];
        if (ep.transfer_type == USB_ENDPOINT_TYPE_BULK && ep.is_in() == is_in && ep.max_packet_size > 0) {
            return &ep;
        }
    }

    return nullptr;
}

const endpoint* find_bulk_in(const interface& iface) {
    return find_bulk(iface, true);
}

const endpoint* find_bulk_out(const interface& iface) {
    return find_bulk(iface, false);
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

} // namespace usb::serial
