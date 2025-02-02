#include <modules/usb/xhci/xhci_usb_hid_mouse_driver.h>
#include <serial/serial.h>

uint64_t g_mouse_cursor_pos_x = 100;
uint64_t g_mouse_cursor_pos_y = 100;

// Reads field data (of size field_size bytes, where field_size can be 1, 2, or 4)
// and returns a sign‑extended int32_t value (assuming little‑endian data).
int32_t parse_signed_field(const uint8_t* data, uint8_t field_size) {
    int32_t value = 0;
    // Copy the available bytes into value.
    memcpy(&value, data, field_size);
    // If the field is less than 4 bytes, sign‑extend it.
    if (field_size < 4) {
        // Calculate how many bits to shift.
        int shift = (4 - field_size) * 8;
        // Left shift then arithmetic right shift to sign-extend.
        value = (value << shift) >> shift;
    }
    return value;
}

void xhci_usb_hid_mouse_driver::on_device_init() {
    _construct_default_report_layout();

    if (!m_interface->additional_data) {
        return;
    }

    kstl::vector<hid_report_item> hid_items;
    if (!hid_report_parser::parse_descriptor(
        m_interface->additional_data, m_interface->additional_data_length, hid_items
    )) {
        return;
    }

    if (!parse_mouse_report_layout(hid_items, m_report_layout)) {
        return;
    }

    // Store the computed layout in your interface structure for later use.
    serial::printf("Parsed mouse layout: buttons at %u (%u bytes), dx at %u (%u bytes), dy at %u (%u bytes)\n",
        m_report_layout.buttons_offset, m_report_layout.buttons_size,
        m_report_layout.dx_offset, m_report_layout.dx_size,
        m_report_layout.dy_offset, m_report_layout.dy_size);
}

void xhci_usb_hid_mouse_driver::on_device_event(uint8_t* data) {
    int32_t dx = parse_signed_field(data + m_report_layout.dx_offset, m_report_layout.dx_size);
    int32_t dy = parse_signed_field(data + m_report_layout.dy_offset, m_report_layout.dy_size);

    g_mouse_cursor_pos_x += dx;
    g_mouse_cursor_pos_y += dy;
}

void xhci_usb_hid_mouse_driver::_construct_default_report_layout() {
    m_report_layout.buttons_offset  = 0;
    m_report_layout.dx_offset       = 1;
    m_report_layout.dy_offset       = 2;
    m_report_layout.buttons_size    = 1;
    m_report_layout.dx_size         = 1;
    m_report_layout.dy_size         = 1;
}
