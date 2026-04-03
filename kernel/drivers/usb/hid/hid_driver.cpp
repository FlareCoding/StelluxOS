#include "drivers/usb/hid/hid_driver.h"
#include "drivers/usb/hid/hid_mouse_handler.h"
#include "drivers/usb/hid/hid_keyboard_handler.h"
#include "drivers/usb/hid/hid_constants.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/usb_descriptors.h"
#include "common/string.h"
#include "common/logging.h"
#include "mm/paging_types.h"
#include "mm/heap.h"

namespace usb::hid {

namespace {

struct report_capabilities {
    bool keyboard = false;
    bool mouse = false;
};

static bool is_keyboard_modifier_usage(uint16_t usage) {
    return usage >= 0xE0u && usage <= 0xE7u;
}

static report_capabilities classify_report(const report_layout& layout,
                                           const input_report_info& report) {
    report_capabilities caps = {};
    const field_info* fields = report_fields(layout, report);
    if (!fields) {
        return caps;
    }

    auto gd = static_cast<uint16_t>(usage_page::generic_desktop);
    auto kb = static_cast<uint16_t>(usage_page::keyboard);
    bool has_x = false;
    bool has_y = false;

    for (uint16_t i = 0; i < report.field_count; i++) {
        const auto& field = fields[i];
        if (field.is_constant()) {
            continue;
        }

        if (field.usage_page == gd) {
            if (field.usage == static_cast<uint16_t>(generic_desktop_usage::x_axis)) {
                has_x = true;
            }
            if (field.usage == static_cast<uint16_t>(generic_desktop_usage::y_axis)) {
                has_y = true;
            }
        }

        if (field.usage_page == kb &&
            (!field.is_variable() || !is_keyboard_modifier_usage(field.usage))) {
            caps.keyboard = true;
        }
    }

    caps.mouse = has_x && has_y;
    return caps;
}

} // namespace

hid_driver::hid_driver(usb::device* dev, usb::interface* iface)
    : class_driver("usb-hid"), m_iface(iface) {
    (void)dev;
}

hid_driver::~hid_driver() {
    if (m_stream) {
        usb::close_interrupt_in_stream(m_stream);
        m_stream = nullptr;
    }
    destroy_bindings();
    m_layout.destroy();
}

int32_t hid_driver::probe(usb::device* dev, usb::interface* iface) {
    const usb::endpoint* ep = find_interrupt_in_endpoint();
    if (!ep) {
        log::error("hid: no interrupt IN endpoint found");
        return -1;
    }

    // GET_REPORT_DESCRIPTOR — HID report descriptor is at interface level
    int32_t rc = 0;
    uint16_t desc_len = iface->hid_report_desc_length != 0
        ? iface->hid_report_desc_length
        : 256;
    if (desc_len > 4096) {
        log::error("hid: report descriptor too large (%u bytes)", desc_len);
        return -1;
    }

    auto* desc_buf = static_cast<uint8_t*>(heap::ualloc(desc_len));
    if (!desc_buf) {
        log::error("hid: failed to allocate report descriptor buffer");
        return -1;
    }
    string::memset(desc_buf, 0, desc_len);

    rc = usb::control_transfer(dev,
        usb::USB_REQTYPE_DIR_IN | usb::USB_REQTYPE_TYPE_STANDARD | usb::USB_REQTYPE_RECIP_INTERFACE,
        usb::USB_REQUEST_GET_DESCRIPTOR,
        usb::USB_DESCRIPTOR_REQUEST(usb::USB_DESCRIPTOR_HID_REPORT, 0),
        iface->interface_number,
        desc_buf, desc_len);

    if (rc != 0) {
        log::error("hid: failed to get report descriptor");
        heap::ufree(desc_buf);
        return -1;
    }

    rc = parse_report_descriptor(desc_buf, desc_len, m_layout);
    heap::ufree(desc_buf);
    if (rc != 0) {
        log::error("hid: failed to parse report descriptor");
        return -1;
    }

    log::info("hid: parsed %u input fields across %u input reports%s",
              m_layout.num_fields,
              m_layout.num_input_reports,
              m_layout.uses_report_ids ? " with report IDs" : "");

    rc = create_handlers();
    if (rc != 0) {
        log::warn("hid: no supported report-protocol handlers for interface %u",
                  iface->interface_number);
        return -1;
    }

    if (iface->interface_subclass == 0x01) {
        if (iface->interface_protocol != 0x01 && iface->interface_protocol != 0x02) {
            log::warn("hid: unsupported boot-protocol HID interface %u (protocol=%u)",
                      iface->interface_number, iface->interface_protocol);
            return -1;
        }

        rc = usb::control_transfer(dev,
            usb::USB_REQTYPE_DIR_OUT | usb::USB_REQTYPE_TYPE_CLASS | usb::USB_REQTYPE_RECIP_INTERFACE,
            usb::HID_REQUEST_SET_PROTOCOL,
            1,
            iface->interface_number,
            nullptr, 0);
        if (rc != 0) {
            log::warn("hid: SET_PROTOCOL(REPORT) failed for interface %u",
                      iface->interface_number);
            return -1;
        }
    } else if (iface->interface_subclass != 0x00) {
        log::warn("hid: unsupported HID subclass %u on interface %u",
                  iface->interface_subclass, iface->interface_number);
        return -1;
    }

    // Only apply SET_IDLE to keyboard reports, and target those report IDs
    // individually when the interface uses Report IDs.
    apply_idle_policy(dev);

    m_payload_length = max_input_report_bytes(*ep);
    if (m_payload_length == 0) {
        log::error("hid: invalid input report payload length");
        return -1;
    }

    rc = usb::open_interrupt_in_stream(
        dev, ep->address, m_payload_length, &m_stream);
    if (rc != 0) {
        log::error("hid: failed to open interrupt stream on EP 0x%02x", ep->address);
        return -1;
    }

    log::info("hid: opened interrupt stream on EP 0x%02x (%u byte payload)",
              ep->address, m_payload_length);
    return 0;
}

void hid_driver::run() {
    const usb::endpoint* ep = find_interrupt_in_endpoint();
    if (!m_stream || !ep || m_payload_length == 0) {
        log::warn("hid: run() called without an active interrupt stream");
        return;
    }

    auto* report_buf = static_cast<uint8_t*>(heap::ualloc(m_payload_length));
    if (!report_buf) {
        log::error("hid: failed to allocate report buffer");
        return;
    }

    log::info("hid: starting interrupt stream loop on EP 0x%02x (%u bytes)",
              ep->address, m_payload_length);

    while (!m_disconnected) {
        uint32_t actual = 0;
        string::memset(report_buf, 0, m_payload_length);
        int32_t rc = usb::read_interrupt_in_stream(
            m_stream, report_buf, m_payload_length, &actual);
        if (rc != 0) {
            if (m_disconnected) {
                break;
            }
            log::warn("hid: interrupt stream read failed (%d)", rc);
            break;
        }

        uint8_t report_id = 0;
        const uint8_t* report_data = report_buf;
        uint32_t report_length = actual;

        if (m_layout.uses_report_ids) {
            if (actual == 0) {
                log::warn("hid: dropped empty report missing report ID");
                continue;
            }
            report_id = report_buf[0];
            report_data = report_buf + 1;
            report_length = actual - 1;
        }

        if (!find_input_report(m_layout, report_id)) {
            log::warn("hid: unknown input report id %u", report_id);
            continue;
        }

        dispatch_report(report_id, report_data, report_length);
    }

    heap::ufree(report_buf);
    if (m_stream) {
        usb::close_interrupt_in_stream(m_stream);
        m_stream = nullptr;
    }
    log::info("hid: driver exiting");
}

void hid_driver::disconnect() {
    m_disconnected = true;
}

const usb::endpoint* hid_driver::find_interrupt_in_endpoint() const {
    for (uint8_t i = 0; i < m_iface->num_endpoints; i++) {
        const auto& ep = m_iface->endpoints[i];
        if (ep.transfer_type == 3 && ep.is_in()) { // 3 = interrupt
            return &ep;
        }
    }
    return nullptr;
}

uint32_t hid_driver::max_input_report_bytes(const usb::endpoint& ep) const {
    if (m_layout.max_input_report_bytes == 0) {
        return 0;
    }

    if (m_layout.max_input_report_bytes > ep.max_packet_size) {
        log::error("hid: input report wire length %u exceeds EP 0x%02x max packet size %u",
                   m_layout.max_input_report_bytes, ep.address, ep.max_packet_size);
        return 0;
    }

    if (m_layout.max_input_report_bytes > paging::PAGE_SIZE_4KB) {
        log::error("hid: input report wire length %u exceeds stream limit %u",
                   m_layout.max_input_report_bytes,
                   static_cast<uint32_t>(paging::PAGE_SIZE_4KB));
        return 0;
    }

    return m_layout.max_input_report_bytes;
}

void hid_driver::destroy_bindings() {
    if (m_bindings) {
        for (uint16_t i = 0; i < m_binding_count; i++) {
            if (m_bindings[i].handler) {
                heap::ufree_delete(m_bindings[i].handler);
                m_bindings[i].handler = nullptr;
            }
        }
        heap::ufree(m_bindings);
        m_bindings = nullptr;
    }
    m_binding_count = 0;
}

int32_t hid_driver::create_handlers() {
    destroy_bindings();

    uint16_t binding_count = 0;
    for (uint16_t i = 0; i < m_layout.num_input_reports; i++) {
        report_capabilities caps = classify_report(m_layout, m_layout.input_reports[i]);
        if (caps.keyboard) {
            binding_count++;
        }
        if (caps.mouse) {
            binding_count++;
        }
    }

    if (binding_count == 0) {
        return -1;
    }

    m_bindings = static_cast<handler_binding*>(
        heap::ualloc(binding_count * sizeof(handler_binding)));
    if (!m_bindings) {
        return -1;
    }

    uint16_t slot = 0;
    m_binding_count = 0;
    for (uint16_t i = 0; i < m_layout.num_input_reports; i++) {
        const auto& report = m_layout.input_reports[i];
        report_capabilities caps = classify_report(m_layout, report);

        if (caps.keyboard) {
            auto* handler = heap::ualloc_new<hid_keyboard_handler>();
            if (!handler || handler->init(m_layout, report) != 0) {
                if (handler) {
                    heap::ufree_delete(handler);
                }
            } else {
                m_bindings[slot++] = { report.report_id, binding_kind::keyboard, handler };
                m_binding_count = slot;
            }
        }

        if (caps.mouse) {
            auto* handler = heap::ualloc_new<hid_mouse_handler>();
            if (!handler || handler->init(m_layout, report) != 0) {
                if (handler) {
                    heap::ufree_delete(handler);
                }
            } else {
                m_bindings[slot++] = { report.report_id, binding_kind::mouse, handler };
                m_binding_count = slot;
            }
        }
    }

    m_binding_count = slot;
    return m_binding_count > 0 ? 0 : -1;
}

void hid_driver::apply_idle_policy(usb::device* dev) {
    for (uint16_t i = 0; i < m_binding_count; i++) {
        if (!m_bindings[i].handler || m_bindings[i].kind != binding_kind::keyboard) {
            continue;
        }

        uint8_t report_id = m_layout.uses_report_ids ? m_bindings[i].report_id : 0;
        bool already_sent = false;
        for (uint16_t j = 0; j < i; j++) {
            if (m_bindings[j].handler &&
                m_bindings[j].kind == binding_kind::keyboard &&
                m_bindings[j].report_id == m_bindings[i].report_id) {
                already_sent = true;
                break;
            }
        }
        if (already_sent) {
            continue;
        }

        int32_t rc = usb::control_transfer(
            dev,
            usb::USB_REQTYPE_DIR_OUT | usb::USB_REQTYPE_TYPE_CLASS | usb::USB_REQTYPE_RECIP_INTERFACE,
            usb::HID_REQUEST_SET_IDLE,
            report_id,
            m_iface->interface_number,
            nullptr, 0);
        if (rc != 0) {
            log::warn("hid: SET_IDLE failed for interface %u report %u",
                      m_iface->interface_number, report_id);
        }
    }
}

void hid_driver::dispatch_report(uint8_t report_id, const uint8_t* data, uint32_t length) {
    for (uint16_t i = 0; i < m_binding_count; i++) {
        if (m_bindings[i].report_id == report_id && m_bindings[i].handler) {
            m_bindings[i].handler->on_report(data, length);
        }
    }
}

} // namespace usb::hid

REGISTER_USB_CLASS_DRIVER(hid_driver,
    USB_MATCH(usb::USB_CLASS_HID, usb::USB_MATCH_ANY, usb::USB_MATCH_ANY),
    USB_CLASS_DRIVER_FACTORY(usb::hid::hid_driver));
