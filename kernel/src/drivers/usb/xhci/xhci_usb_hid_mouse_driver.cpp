#include <drivers/usb/xhci/xhci_usb_hid_mouse_driver.h>
#include <serial/serial.h>
#include <input/system_input_manager.h>

int64_t g_mouse_cursor_pos_x = 100;
int64_t g_mouse_cursor_pos_y = 100;

int64_t g_max_mouse_cursor_pos_x = 4095;
int64_t g_max_mouse_cursor_pos_y = 4095;

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
    m_input_layout.scroll_offset    = 3;
    m_input_layout.scroll_size      = 1;
    
    // Initialize button state tracking
    m_previous_button_state = 0;

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

    _initialize_input_field(layout, static_cast<uint16_t>(hid::usage_page::generic_desktop), 
                           static_cast<uint16_t>(hid::generic_desktop_usage::wheel), 
                           m_input_layout.scroll_offset, m_input_layout.scroll_size, "Scroll wheel");
}

void xhci_usb_hid_mouse_driver::on_device_event(uint8_t* data) {
    // Parse input data from HID report
    int32_t dx = parse_signed_field(data, m_input_layout.x_axis_offset, m_input_layout.x_axis_size);
    int32_t dy = parse_signed_field(data, m_input_layout.y_axis_offset, m_input_layout.y_axis_size);
    uint32_t buttons = static_cast<uint32_t>(parse_signed_field(data, m_input_layout.buttons_offset, m_input_layout.buttons_size));
    int32_t scroll = parse_signed_field(data, m_input_layout.scroll_offset, m_input_layout.scroll_size);

    // Update global cursor position
    g_mouse_cursor_pos_x += dx;
    g_mouse_cursor_pos_y += dy;
    
    // Clamp cursor position to reasonable bounds (0-4095 for now)
    if (g_mouse_cursor_pos_x < 0) g_mouse_cursor_pos_x = 0;
    if (g_mouse_cursor_pos_y < 0) g_mouse_cursor_pos_y = 0;
    if (g_mouse_cursor_pos_x > g_max_mouse_cursor_pos_x) g_mouse_cursor_pos_x = g_max_mouse_cursor_pos_x;
    if (g_mouse_cursor_pos_y > g_max_mouse_cursor_pos_y) g_mouse_cursor_pos_y = g_max_mouse_cursor_pos_y;

    // Emit mouse movement event if there was movement
    if (dx != 0 || dy != 0) {
        _emit_input_event(
            input::POINTER_EVT_MOUSE_MOVED,
            static_cast<uint32_t>(g_mouse_cursor_pos_x), // x_pos
            static_cast<uint32_t>(g_mouse_cursor_pos_y), // y_pos
            dx,                                          // delta_x
            dy                                           // delta_y
        );
    }

    // Handle button press/release events
    uint32_t button_changes = buttons ^ m_previous_button_state;
    if (button_changes != 0) {
        // Check each button bit (up to 8 buttons)
        for (int button_num = 1; button_num <= 8; button_num++) {
            uint32_t button_mask = 1U << (button_num - 1);
            
            if (button_changes & button_mask) {
                bool is_pressed = (buttons & button_mask) != 0;
                
                if (is_pressed) {
                    // Button pressed
                    _emit_input_event(
                        input::POINTER_EVT_MOUSE_BTN_PRESSED,
                        button_num,                              // button number
                        static_cast<uint32_t>(g_mouse_cursor_pos_x), // x_pos
                        static_cast<int32_t>(g_mouse_cursor_pos_y),  // y_pos
                        0                                        // reserved
                    );
                } else {
                    // Button released
                    _emit_input_event(
                        input::POINTER_EVT_MOUSE_BTN_RELEASED,
                        button_num,                              // button number
                        static_cast<uint32_t>(g_mouse_cursor_pos_x), // x_pos
                        static_cast<int32_t>(g_mouse_cursor_pos_y),  // y_pos
                        0                                        // reserved
                    );
                }
            }
        }
        
        // Update previous button state
        m_previous_button_state = buttons;
    }

    // Handle scroll wheel events
    if (scroll != 0) {
        _emit_input_event(
            input::POINTER_EVT_MOUSE_SCROLLED,
            0,                                           // scroll_type (0=vertical)
            static_cast<uint32_t>(g_mouse_cursor_pos_x), // x_pos
            static_cast<int32_t>(g_mouse_cursor_pos_y),  // y_pos
            scroll                                       // scroll_delta
        );
    }
}

void xhci_usb_hid_mouse_driver::_emit_input_event(uint32_t event_type, uint32_t udata1, uint32_t udata2, int32_t sdata1, int32_t sdata2) {
    input::input_event_t event = {
        .id = 0,  // Event ID (can be used for tracking)
        .type = static_cast<input::input_event_type>(event_type),
        .udata1 = udata1,
        .udata2 = udata2,
        .sdata1 = sdata1,
        .sdata2 = sdata2
    };
    
    auto& input_mgr = input::system_input_manager::get();
    input_mgr.push_event(INPUT_QUEUE_ID_SYSTEM, event);
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
