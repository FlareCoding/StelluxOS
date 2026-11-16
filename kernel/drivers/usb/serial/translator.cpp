#include "drivers/usb/serial/translator.h"
#include "drivers/usb/usb_descriptors.h"

namespace usb::serial {

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

} // namespace usb::serial
