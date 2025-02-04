#include <drivers/usb/xhci/xhci_usb_hid_mouse_driver.h>
#include <serial/serial.h>

uint64_t g_mouse_cursor_pos_x = 100;
uint64_t g_mouse_cursor_pos_y = 100;

static int32_t parse_signed_field(const uint8_t* data, uint16_t bit_offset, uint16_t bit_size) {
    int32_t value = 0;

    // Read the necessary bytes and handle sign extension if needed
    memcpy(&value, data + (bit_offset / 8), (bit_size + 7) / 8);  // Round bit size up to the nearest byte

    // Handle sign extension if necessary
    int32_t shift = 32 - bit_size;
    value = (value << shift) >> shift;  // Sign extend based on bit size

    return value;
}

void xhci_usb_hid_mouse_driver::on_device_init() {
    // Default values
    m_input_layout.buttons_offset   = 0;
    m_input_layout.x_axis_offset    = 1;
    m_input_layout.y_axis_offset    = 2;
    m_input_layout.buttons_size     = 1;
    m_input_layout.x_axis_size      = 1;
    m_input_layout.y_axis_size      = 1;

    kstl::vector<hid::hid_report_item> hid_report_items;
    bool success = hid::hid_report_parser::parse_descriptor(
        m_interface->additional_data,
        m_interface->additional_data_length,
        hid_report_items
    );

    if (!success) {
        serial::printf("[Mouse Driver] Failed to parse HID report descriptor.\n");
        return;
    }

    hid::hid_report_layout layout(hid_report_items);

    // Initialize button, X-axis, and Y-axis fields using the helper function
    _initialize_input_field(layout, static_cast<uint16_t>(hid::usage_page::buttons), 1, 
                           m_input_layout.buttons_offset, m_input_layout.buttons_size, "Buttons");

    _initialize_input_field(layout, static_cast<uint16_t>(hid::usage_page::generic_desktop), 
                           static_cast<uint16_t>(hid::generic_desktop_usage::x_axis), 
                           m_input_layout.x_axis_offset, m_input_layout.x_axis_size, "X-axis");

    _initialize_input_field(layout, static_cast<uint16_t>(hid::usage_page::generic_desktop), 
                           static_cast<uint16_t>(hid::generic_desktop_usage::y_axis), 
                           m_input_layout.y_axis_offset, m_input_layout.y_axis_size, "Y-axis");
}

void xhci_usb_hid_mouse_driver::on_device_event(uint8_t* data) {
    int32_t dx = parse_signed_field(data, m_input_layout.x_axis_offset, m_input_layout.x_axis_size);
    int32_t dy = parse_signed_field(data, m_input_layout.y_axis_offset, m_input_layout.y_axis_size);

    g_mouse_cursor_pos_x += dx;
    g_mouse_cursor_pos_y += dy;
}

void xhci_usb_hid_mouse_driver::_initialize_input_field(
    hid::hid_report_layout& layout, 
    uint16_t usage_page, uint16_t usage, 
    uint16_t& offset, uint16_t& size, 
    const char* field_name
) {
    const hid::field_info* field = layout.find_field_by_usage(usage_page, usage);
    if (field) {
        offset = field->bit_offset;
        
        // Special case for usage pages with multiple fields (like buttons)
        if (usage_page == static_cast<uint16_t>(hid::usage_page::buttons)) {
            size = layout.get_total_bits_for_usage_page(usage_page);
        } else {
            size = field->bit_size;
        }

#if 0
        serial::printf(
            "[Mouse Driver] %s field detected at offset=%u, size=%u bits.\n", 
            field_name, offset, size
        );
#else
        __unused field_name;
#endif
    }
}
