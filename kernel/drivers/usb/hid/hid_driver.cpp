#include "drivers/usb/hid/hid_driver.h"
#include "drivers/usb/hid/hid_mouse_handler.h"
#include "drivers/usb/hid/hid_keyboard_handler.h"
#include "drivers/usb/hid/hid_constants.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/usb_descriptors.h"
#include "common/logging.h"
#include "mm/heap.h"

namespace usb::hid {

hid_driver::hid_driver(usb::device* dev, usb::interface* iface)
    : class_driver("usb-hid"), m_dev(dev), m_iface(iface) {}

hid_driver::~hid_driver() {
    m_layout.destroy();
    if (m_handler) {
        heap::ufree_delete(m_handler);
        m_handler = nullptr;
    }
}

int32_t hid_driver::probe(usb::device* dev, usb::interface* iface) {
    // SET_IDLE(0) — tell the device to only send reports on change
    usb::control_transfer(dev,
        usb::USB_REQTYPE_DIR_OUT | usb::USB_REQTYPE_TYPE_CLASS | usb::USB_REQTYPE_RECIP_INTERFACE,
        usb::HID_REQUEST_SET_IDLE,
        0, iface->interface_number, nullptr, 0);

    // GET_REPORT_DESCRIPTOR — HID report descriptor is at interface level
    uint8_t desc_buf[256] = {};
    int32_t rc = usb::control_transfer(dev,
        usb::USB_REQTYPE_DIR_IN | usb::USB_REQTYPE_TYPE_STANDARD | usb::USB_REQTYPE_RECIP_INTERFACE,
        usb::USB_REQUEST_GET_DESCRIPTOR,
        usb::USB_DESCRIPTOR_REQUEST(usb::USB_DESCRIPTOR_HID_REPORT, 0),
        iface->interface_number,
        desc_buf, sizeof(desc_buf));

    if (rc != 0) {
        log::error("hid: failed to get report descriptor");
        return -1;
    }

    // Parse the report descriptor into a field layout
    rc = parse_report_descriptor(desc_buf, sizeof(desc_buf), m_layout);
    if (rc != 0) {
        log::error("hid: failed to parse report descriptor");
        return -1;
    }

    log::info("hid: parsed %u input fields from report descriptor", m_layout.num_fields);

    // Detect device type and create the appropriate handler
    m_handler = create_handler();
    if (!m_handler) {
        log::warn("hid: no handler for this device type");
        return -1;
    }

    m_handler->init(m_layout);
    return 0;
}

void hid_driver::run() {
    uint8_t ep_addr = find_interrupt_in_endpoint();
    if (ep_addr == 0) {
        log::error("hid: no interrupt IN endpoint found");
        return;
    }

    log::info("hid: starting interrupt transfer loop on EP 0x%02x", ep_addr);

    uint8_t report_buf[64] = {};

    while (!m_disconnected) {
        int32_t rc = usb::interrupt_transfer(m_dev, ep_addr,
                                             report_buf, sizeof(report_buf));
        if (rc != 0) {
            if (m_disconnected) break;
            log::warn("hid: interrupt transfer failed (%d)", rc);
            break;
        }

        m_handler->on_report(report_buf, sizeof(report_buf));
    }

    log::info("hid: driver exiting");
}

void hid_driver::disconnect() {
    m_disconnected = true;
}

uint8_t hid_driver::find_interrupt_in_endpoint() const {
    for (uint8_t i = 0; i < m_iface->num_endpoints; i++) {
        auto& ep = m_iface->endpoints[i];
        if (ep.transfer_type == 3 && ep.is_in()) { // 3 = interrupt
            return ep.address;
        }
    }
    return 0;
}

hid_handler* hid_driver::create_handler() {
    auto gd = static_cast<uint16_t>(usage_page::generic_desktop);

    auto kb = static_cast<uint16_t>(usage_page::keyboard);
    bool has_x = false, has_y = false, has_keyboard = false;

    for (uint16_t i = 0; i < m_layout.num_fields; i++) {
        auto& f = m_layout.fields[i];
        if (f.usage_page == gd) {
            if (f.usage == static_cast<uint16_t>(generic_desktop_usage::x_axis)) has_x = true;
            if (f.usage == static_cast<uint16_t>(generic_desktop_usage::y_axis)) has_y = true;
        }
        if (f.usage_page == kb) {
            has_keyboard = true;
        }
    }

    // Pointing device: has X and Y axes
    if (has_x && has_y) {
        log::info("hid: detected mouse/pointing device");
        return heap::ualloc_new<hid_mouse_handler>();
    }

    // Keyboard: has keyboard usage page fields
    if (has_keyboard) {
        log::info("hid: detected keyboard");
        return heap::ualloc_new<hid_keyboard_handler>();
    }

    log::warn("hid: unrecognized device type (no matching handler)");
    return nullptr;
}

} // namespace usb::hid

REGISTER_USB_CLASS_DRIVER(hid_driver,
    USB_MATCH(usb::USB_CLASS_HID, usb::USB_MATCH_ANY, usb::USB_MATCH_ANY),
    USB_CLASS_DRIVER_FACTORY(usb::hid::hid_driver));
