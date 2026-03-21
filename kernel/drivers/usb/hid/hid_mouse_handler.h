#ifndef STELLUX_DRIVERS_USB_HID_HID_MOUSE_HANDLER_H
#define STELLUX_DRIVERS_USB_HID_HID_MOUSE_HANDLER_H

#include "drivers/usb/hid/hid_handler.h"

namespace usb::hid {

class hid_mouse_handler : public hid_handler {
public:
    ~hid_mouse_handler() override;

    int32_t init(const report_layout& layout,
                 const input_report_info& report) override;
    void on_report(const uint8_t* data, uint32_t length) override;

private:
    const field_info*  m_x_field = nullptr;
    const field_info*  m_y_field = nullptr;
    const field_info*  m_wheel_field = nullptr;
    const field_info** m_button_fields = nullptr;
    uint8_t*           m_prev_buttons = nullptr;
    uint16_t           m_button_count = 0;
    uint8_t            m_report_id = 0;
    bool               m_ready = false;

    void reset_state();
    static int32_t read_signed_field(const uint8_t* data, uint32_t length,
                                     uint32_t bit_offset, uint16_t bit_size);
    static uint32_t read_unsigned_field(const uint8_t* data, uint32_t length,
                                        uint32_t bit_offset, uint16_t bit_size);
};

} // namespace usb::hid

#endif // STELLUX_DRIVERS_USB_HID_HID_MOUSE_HANDLER_H
