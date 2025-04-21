#ifndef XHCI_USB_HID_KBD_DRIVER_H
#define XHCI_USB_HID_KBD_DRIVER_H
#include "xhci_usb_hid_driver.h"
#include <drivers/usb/hid/hid_report_parser.h>
#include <input/system_input_manager.h>

enum kbd_mod_mask : uint32_t {
    KBD_MOD_LCTRL   = 1 << 0,
    KBD_MOD_LSHIFT  = 1 << 1,
    KBD_MOD_LALT    = 1 << 2,
    KBD_MOD_LGUI    = 1 << 3,
    KBD_MOD_RCTRL   = 1 << 4,
    KBD_MOD_RSHIFT  = 1 << 5,
    KBD_MOD_RALT    = 1 << 6,
    KBD_MOD_RGUI    = 1 << 7,
};

class xhci_usb_hid_kbd_driver : public xhci_usb_hid_driver {
public:
    xhci_usb_hid_kbd_driver() = default;
    ~xhci_usb_hid_kbd_driver() = default;

    void on_device_init() override;
    void on_device_event(uint8_t* data) override;

private:
    struct input_data_layout {
        uint16_t buttons_offset;
        uint16_t buttons_size;
        uint16_t x_axis_offset;
        uint16_t x_axis_size;
        uint16_t y_axis_offset;
        uint16_t y_axis_size;
    } m_input_layout;

    void _initialize_input_field(
        hid::hid_report_layout& layout, 
        uint16_t usage_page, uint16_t usage, 
        uint16_t& offset, uint16_t& size, 
        const char* field_name
    );

    void _process_input_report(const uint8_t* current_keys, uint8_t modifier_byte);
    void _emit_key_event(uint8_t key, input::input_event_type type, uint32_t modifiers);

    uint8_t m_prev_keys[6]{};
};

#endif // XHCI_USB_HID_KBD_DRIVER_H
