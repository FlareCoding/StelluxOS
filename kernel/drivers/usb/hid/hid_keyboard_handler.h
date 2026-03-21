#ifndef STELLUX_DRIVERS_USB_HID_HID_KEYBOARD_HANDLER_H
#define STELLUX_DRIVERS_USB_HID_HID_KEYBOARD_HANDLER_H

#include "drivers/usb/hid/hid_handler.h"

namespace usb::hid {

class hid_keyboard_handler : public hid_handler {
public:
    ~hid_keyboard_handler() override;

    int32_t init(const report_layout& layout,
                 const input_report_info& report) override;
    void on_report(const uint8_t* data, uint32_t length) override;

private:
    const field_info* m_modifier_fields[8] = {};
    const field_info** m_key_fields = nullptr;
    uint16_t* m_prev_keycodes = nullptr;
    uint16_t* m_curr_keycodes = nullptr;
    uint16_t m_key_field_count = 0;
    uint8_t m_prev_modifiers = 0;
    uint8_t m_report_id = 0;
    bool    m_ready = false;

    void reset_state();
    static uint32_t read_unsigned_field(const uint8_t* data, uint32_t length,
                                        uint32_t bit_offset, uint16_t bit_size);
    static bool is_modifier_usage(uint16_t usage);
    static bool is_reserved_array_usage(uint16_t usage);
    static bool contains_keycode(const uint16_t* keycodes, uint16_t count, uint16_t keycode);
    static const char* scancode_to_name(uint16_t scancode);
};

} // namespace usb::hid

#endif // STELLUX_DRIVERS_USB_HID_HID_KEYBOARD_HANDLER_H
