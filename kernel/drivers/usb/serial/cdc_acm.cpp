#include "drivers/usb/serial/cdc_acm.h"
#include "drivers/usb/serial/bridge.h"
#include "drivers/usb/usb_descriptors.h"
#include "common/logging.h"
#include "terminal/tty.h"

namespace usb::serial {

struct cdc_union_descriptor {
    usb_descriptor_header header;
    uint8_t bDescriptorSubtype;
    uint8_t bControlInterface;
    uint8_t bSubordinateInterface0;
} __attribute__((packed));
static_assert(sizeof(cdc_union_descriptor) == 5);

static constexpr uint8_t ACM_SUBCLASS = 0x02;

// A vendor protocol, such as RNDIS, carries something other than a serial port
static constexpr uint8_t VENDOR_PROTOCOL = 0xFF;

static constexpr size_t CONFIG_HEADER_BYTES = __builtin_offsetof(usb_configuration_descriptor, data);

static constexpr uint8_t CS_INTERFACE  = 0x24; // a class-specific interface descriptor
static constexpr uint8_t UNION_SUBTYPE = 0x06;

// Class requests to the control interface
static constexpr uint8_t SET_LINE_CODING        = 0x20;
static constexpr uint8_t SET_CONTROL_LINE_STATE = 0x22;
static constexpr uint8_t SEND_BREAK             = 0x23;

// SET_LINE_CODING's data: the speed in 4 bytes, then the stop bits, parity and data bits
static constexpr size_t  LINE_CODING_BYTES = 7;
static constexpr uint8_t ONE_STOP_BIT      = 0;
static constexpr uint8_t TWO_STOP_BITS     = 2;
static constexpr uint8_t PARITY_NONE       = 0;
static constexpr uint8_t PARITY_ODD        = 1;
static constexpr uint8_t PARITY_EVEN       = 2;
static constexpr uint8_t PARITY_MARK       = 3;
static constexpr uint8_t PARITY_SPACE      = 4;

static constexpr uint16_t CONTROL_LINE_DTR = 1u << 0;
static constexpr uint16_t CONTROL_LINE_RTS = 1u << 1;

// SEND_BREAK's value is the break's length in milliseconds, except that 0xFFFF holds it until a request with 0
static constexpr uint16_t BREAK_UNTIL_ENDED = 0xFFFF;
static constexpr uint16_t BREAK_ENDED       = 0;

// A notification is an 8-byte header and its data, whose length the header's last 2 bytes give, and SERIAL_STATE's
// data is 2 bytes of line states
static constexpr uint8_t  NOTIFICATION_REQUEST_TYPE  = 0xA1;
static constexpr uint8_t  SERIAL_STATE               = 0x20;
static constexpr uint32_t NOTIFICATION_HEADER_BYTES  = 8;
static constexpr uint32_t NOTIFICATION_LENGTH_OFFSET = 6;
static constexpr uint32_t SERIAL_STATE_BYTES         = NOTIFICATION_HEADER_BYTES + 2;

// SERIAL_STATE's bits for the modem inputs CD, DSR and RI
static constexpr uint16_t STATE_CD  = 1u << 0;
static constexpr uint16_t STATE_DSR = 1u << 1;
static constexpr uint16_t STATE_RI  = 1u << 3;

// As few reads as a stream with a callback allows, since a device notifies only when a modem input changes
static constexpr uint32_t NOTIFICATION_READS = 2;

static bool is_union_descriptor(const usb_descriptor_header* header) {
    if (header->bDescriptorType != CS_INTERFACE || header->bLength < sizeof(cdc_union_descriptor)) {
        return false;
    }

    return reinterpret_cast<const cdc_union_descriptor*>(header)->bDescriptorSubtype == UNION_SUBTYPE;
}

static bool is_alternate_setting_0_of(const usb_descriptor_header* header, uint8_t interface_number) {
    if (header->bLength < sizeof(usb_interface_descriptor)) {
        return false;
    }

    auto* descriptor = reinterpret_cast<const usb_interface_descriptor*>(header);
    return descriptor->bInterfaceNumber == interface_number && descriptor->bAlternateSetting == 0;
}

// Only a union descriptor among the control interface's own descriptors counts, since each port of a device has its own
static bool find_data_interface(
    const uint8_t* config,
    size_t length,
    uint8_t control_interface,
    uint8_t* data_interface
) {
    if (length < CONFIG_HEADER_BYTES || config[1] != USB_DESCRIPTOR_CONFIGURATION) {
        return false;
    }

    size_t total_length = static_cast<size_t>(config[2]) | (static_cast<size_t>(config[3]) << 8);
    size_t end = total_length < length ? total_length : length;

    bool is_in_control_interface = false;
    size_t offset = 0;
    while (offset + sizeof(usb_descriptor_header) <= end) {
        auto* header = reinterpret_cast<const usb_descriptor_header*>(&config[offset]);
        if (header->bLength < sizeof(*header) || header->bLength > end - offset) {
            return false;
        }

        if (header->bDescriptorType == USB_DESCRIPTOR_INTERFACE) {
            is_in_control_interface = is_alternate_setting_0_of(header, control_interface);
        } else if (is_in_control_interface && is_union_descriptor(header)) {
            *data_interface = reinterpret_cast<const cdc_union_descriptor*>(header)->bSubordinateInterface0;
            return true;
        }

        offset += header->bLength;
    }

    return false;
}

static bool is_acm_control_interface(const interface& iface) {
    return iface.interface_class == USB_CLASS_CDC && iface.interface_subclass == ACM_SUBCLASS &&
           iface.interface_protocol != VENDOR_PROTOCOL;
}

// On a device with several CDC-ACM functions, a port's channel is its control interface's place among theirs
static int8_t channel_of(const device& dev, const interface& control) {
    int8_t channel = -1;
    uint8_t functions = 0;
    for (uint8_t i = 0; i < dev.num_interfaces; i++) {
        if (!is_acm_control_interface(dev.interfaces[i])) {
            continue;
        }

        if (dev.interfaces[i].interface_number == control.interface_number) {
            channel = static_cast<int8_t>(functions);
        }

        functions++;
    }

    return functions > 1 ? channel : -1;
}

static uint8_t parity_code(::serial::parity mode) {
    switch (mode) {
        case ::serial::parity::none:  return PARITY_NONE;
        case ::serial::parity::odd:   return PARITY_ODD;
        case ::serial::parity::even:  return PARITY_EVEN;
        case ::serial::parity::mark:  return PARITY_MARK;
        case ::serial::parity::space: return PARITY_SPACE;
    }

    return PARITY_NONE;
}

static bool decode_serial_state(const uint8_t* notification, uint32_t length, uint32_t* lines) {
    bool is_serial_state = length >= SERIAL_STATE_BYTES &&
                           notification[0] == NOTIFICATION_REQUEST_TYPE &&
                           notification[1] == SERIAL_STATE;
    if (!is_serial_state) {
        return false;
    }

    const uint8_t* state_bytes = &notification[NOTIFICATION_HEADER_BYTES];
    uint16_t state = static_cast<uint16_t>(state_bytes[0] | (state_bytes[1] << 8));

    uint32_t inputs = 0;
    if (state & STATE_CD) {
        inputs |= terminal::TIOCM_CAR;
    }

    if (state & STATE_DSR) {
        inputs |= terminal::TIOCM_DSR;
    }

    if (state & STATE_RI) {
        inputs |= terminal::TIOCM_RNG;
    }

    *lines = inputs;
    return true;
}

cdc_acm::cdc_acm(device* dev, const interface& iface)
    : translator(dev, iface),
      m_channel(channel_of(*dev, iface)),
      m_data_interface_number(iface.interface_number),
      m_notification_stream(nullptr),
      m_modem_inputs(0),
      m_notification{},
      m_notification_received(0) {}

int32_t cdc_acm::configure() {
    if (m_interface->interface_protocol == VENDOR_PROTOCOL) {
        return ::serial::ERR_UNSUPPORTED;
    }

    // As much of the configuration descriptor as the xHCI driver reads, so the walk covers every interface it listed
    uint8_t config[sizeof(usb_configuration_descriptor)] = {};
    int32_t rc = control_transfer(
        m_device,
        USB_REQTYPE_DIR_IN | USB_REQTYPE_TYPE_STANDARD | USB_REQTYPE_RECIP_DEVICE,
        USB_REQUEST_GET_DESCRIPTOR,
        USB_DESCRIPTOR_REQUEST(USB_DESCRIPTOR_CONFIGURATION, 0),
        0,
        config,
        sizeof(config)
    );
    if (rc != 0) {
        return ::serial::ERR_IO;
    }

    if (!find_data_interface(config, sizeof(config), m_interface->interface_number, &m_data_interface_number)) {
        return ::serial::ERR_UNSUPPORTED;
    }

    return ::serial::OK;
}

void cdc_acm::open_streams() {
    const endpoint* notification_in = find_interrupt_in(*m_interface);
    if (!notification_in) {
        return;
    }

    // A read of one packet ends at that packet, so a notification that ends on a full packet never runs into the next
    m_notification_received = 0;
    int32_t rc = open_in_stream(
        m_device,
        notification_in->address,
        notification_in->max_packet_size,
        NOTIFICATION_READS,
        deliver_packet,
        this,
        &m_notification_stream
    );
    if (rc != 0) {
        log::warn("usb-serial: %04x:%04x could not open a stream on EP 0x%02x, so CD, DSR and RI read as lowered",
                  m_device->vid, m_device->pid, notification_in->address);
    }
}

void cdc_acm::close_streams() {
    if (m_notification_stream) {
        close_in_stream(m_notification_stream);
        m_notification_stream = nullptr;
    }
}

int32_t cdc_acm::set_line(const ::serial::line_settings& settings) {
    if (settings.uses_rts_cts) {
        return ::serial::ERR_UNSUPPORTED;
    }

    uint8_t coding[LINE_CODING_BYTES];
    store_le32(coding, settings.baud);
    coding[4] = settings.stop_bits == 2 ? TWO_STOP_BITS : ONE_STOP_BIT;
    coding[5] = parity_code(settings.parity_mode);
    coding[6] = settings.data_bits;

    return send_request(SET_LINE_CODING, 0, coding, sizeof(coding));
}

int32_t cdc_acm::set_modem_outputs(uint32_t lines) {
    uint16_t state = 0;
    if (lines & terminal::TIOCM_DTR) {
        state |= CONTROL_LINE_DTR;
    }

    if (lines & terminal::TIOCM_RTS) {
        state |= CONTROL_LINE_RTS;
    }

    return send_request(SET_CONTROL_LINE_STATE, state, nullptr, 0);
}

int32_t cdc_acm::get_modem_inputs(uint32_t* lines) {
    *lines = m_modem_inputs.load_acquire() | terminal::TIOCM_CTS;
    return ::serial::OK;
}

int32_t cdc_acm::set_break(bool on) {
    return send_request(SEND_BREAK, on ? BREAK_UNTIL_ENDED : BREAK_ENDED, nullptr, 0);
}

ssize_t cdc_acm::tx_pending() {
    return ::serial::TX_UNKNOWN;
}

// Runs on the HCD task once per notification packet, in the order the packets came
void cdc_acm::deliver_packet(void* context, const uint8_t* data, uint32_t length, transfer_status status) {
    bool is_read = status == transfer_status::ok || status == transfer_status::short_packet;
    if (is_read) {
        static_cast<cdc_acm*>(context)->handle_packet(data, length, status == transfer_status::short_packet);
    }
}

// A notification ends once its header's length of data has come, and a short packet before then ends its transfer
// early, which drops it
void cdc_acm::handle_packet(const uint8_t* packet, uint32_t length, bool is_short) {
    static_assert(KEPT_NOTIFICATION_BYTES >= SERIAL_STATE_BYTES);
    for (uint32_t i = 0; i < length && m_notification_received + i < KEPT_NOTIFICATION_BYTES; i++) {
        m_notification[m_notification_received + i] = packet[i];
    }

    m_notification_received += length;
    if (m_notification_received >= NOTIFICATION_HEADER_BYTES) {
        const uint8_t* data_length = &m_notification[NOTIFICATION_LENGTH_OFFSET];
        uint32_t notification_length = NOTIFICATION_HEADER_BYTES + (data_length[0] | (data_length[1] << 8));
        if (m_notification_received >= notification_length) {
            if (notification_length <= KEPT_NOTIFICATION_BYTES) {
                handle_notification(m_notification, notification_length);
            }

            m_notification_received = 0;
            return;
        }
    }

    if (is_short) {
        m_notification_received = 0;
    }
}

void cdc_acm::handle_notification(const uint8_t* notification, uint32_t length) {
    uint32_t inputs = 0;
    if (decode_serial_state(notification, length, &inputs)) {
        m_modem_inputs.store_release(inputs);
    }
}

int32_t cdc_acm::send_request(uint8_t request, uint16_t value, void* data, uint16_t length) {
    int32_t rc = control_transfer(
        m_device,
        USB_REQTYPE_DIR_OUT | USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE,
        request,
        value,
        m_interface->interface_number,
        data,
        length
    );

    return rc == 0 ? ::serial::OK : ::serial::ERR_IO;
}

REGISTER_USB_CLASS_DRIVER(cdc_acm,
    USB_MATCH(USB_CLASS_CDC, ACM_SUBCLASS, USB_MATCH_ANY),
    create_bridge<cdc_acm>);

} // namespace usb::serial
