#ifndef XHCI_USB_HID_MOUSE_DRIVER_H
#define XHCI_USB_HID_MOUSE_DRIVER_H
#include "xhci_usb_hid_driver.h"
#include <modules/usb/hid/hid_report_parser.h>

class xhci_usb_hid_mouse_driver : public xhci_usb_hid_driver {
public:
    xhci_usb_hid_mouse_driver() = default;
    ~xhci_usb_hid_mouse_driver() = default;

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
};

#endif // XHCI_USB_HID_MOUSE_DRIVER_H
