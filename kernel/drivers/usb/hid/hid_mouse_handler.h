#ifndef STELLUX_DRIVERS_USB_HID_HID_MOUSE_HANDLER_H
#define STELLUX_DRIVERS_USB_HID_HID_MOUSE_HANDLER_H

#include "drivers/usb/hid/hid_handler.h"

namespace usb::hid {

class hid_mouse_handler : public hid_handler {
public:
    void init(const report_layout& layout) override;
    void on_report(const uint8_t* data, uint32_t length) override;

private:
    struct {
        uint16_t buttons_offset = 0;
        uint16_t buttons_size = 0;
        uint16_t x_offset = 0;
        uint16_t x_size = 0;
        uint16_t y_offset = 0;
        uint16_t y_size = 0;
        uint16_t wheel_offset = 0;
        uint16_t wheel_size = 0;
    } m_layout;

    uint32_t m_prev_buttons = 0;

    static int32_t read_signed_field(const uint8_t* data, uint16_t bit_offset, uint16_t bit_size);
    static uint32_t read_unsigned_field(const uint8_t* data, uint16_t bit_offset, uint16_t bit_size);
};

} // namespace usb::hid

#endif // STELLUX_DRIVERS_USB_HID_HID_MOUSE_HANDLER_H
