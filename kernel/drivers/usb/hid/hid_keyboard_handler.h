#ifndef STELLUX_DRIVERS_USB_HID_HID_KEYBOARD_HANDLER_H
#define STELLUX_DRIVERS_USB_HID_HID_KEYBOARD_HANDLER_H

#include "drivers/usb/hid/hid_handler.h"

namespace usb::hid {

class hid_keyboard_handler : public hid_handler {
public:
    void init(const report_layout& layout) override;
    void on_report(const uint8_t* data, uint32_t length) override;

private:
    struct {
        uint16_t modifier_offset = 0;
        uint16_t modifier_size = 0;
        uint16_t keycode_offset = 0;
        uint16_t keycode_size = 0;   // bits per keycode slot
        uint8_t  keycode_count = 0;  // number of keycode slots
    } m_layout;

    uint8_t m_prev_keycodes[6] = {};
    uint8_t m_prev_modifiers = 0;

    static const char* scancode_to_name(uint8_t scancode);
};

} // namespace usb::hid

#endif // STELLUX_DRIVERS_USB_HID_HID_KEYBOARD_HANDLER_H
