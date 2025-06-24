#ifndef XHCI_USB_HID_MOUSE_DRIVER_H
#define XHCI_USB_HID_MOUSE_DRIVER_H
#include "xhci_usb_hid_driver.h"
#include <drivers/usb/hid/hid_report_parser.h>

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
        uint16_t scroll_offset;
        uint16_t scroll_size;
    } m_input_layout;
    
    uint32_t m_previous_button_state; // Track previous button state for press/release detection

    void _initialize_input_field(
        hid::hid_report_layout& layout, 
        uint16_t usage_page, uint16_t usage, 
        uint16_t& offset, uint16_t& size, 
        const char* field_name
    );
    
    void _emit_input_event(uint32_t event_type, uint32_t udata1, uint32_t udata2, int32_t sdata1, int32_t sdata2);
};

#endif // XHCI_USB_HID_MOUSE_DRIVER_H
