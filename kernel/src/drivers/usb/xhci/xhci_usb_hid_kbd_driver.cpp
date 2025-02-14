#include <drivers/usb/xhci/xhci_usb_hid_kbd_driver.h>
#include <serial/serial.h>

// Global variables to track arrow key states
bool g_arrow_up_pressed = false;
bool g_arrow_down_pressed = false;
bool g_arrow_left_pressed = false;
bool g_arrow_right_pressed = false;

static constexpr uint8_t KEY_UP = 0x52;
static constexpr uint8_t KEY_DOWN = 0x51;
static constexpr uint8_t KEY_LEFT = 0x50;
static constexpr uint8_t KEY_RIGHT = 0x4F;

void xhci_usb_hid_kbd_driver::on_device_init() {}

void xhci_usb_hid_kbd_driver::on_device_event(uint8_t* data) {
    bool new_arrow_up_pressed = false;
    bool new_arrow_down_pressed = false;
    bool new_arrow_left_pressed = false;
    bool new_arrow_right_pressed = false;

    for (int i = 2; i < 8; i++) { // Check the key press array
        switch (data[i]) {
            case KEY_UP:    new_arrow_up_pressed = true; break;
            case KEY_DOWN:  new_arrow_down_pressed = true; break;
            case KEY_LEFT:  new_arrow_left_pressed = true; break;
            case KEY_RIGHT: new_arrow_right_pressed = true; break;
            default: break;
        }
    }

    // Update global states
    g_arrow_up_pressed = new_arrow_up_pressed;
    g_arrow_down_pressed = new_arrow_down_pressed;
    g_arrow_left_pressed = new_arrow_left_pressed;
    g_arrow_right_pressed = new_arrow_right_pressed;
}

void xhci_usb_hid_kbd_driver::_initialize_input_field(
    hid::hid_report_layout& layout, 
    uint16_t usage_page, uint16_t usage, 
    uint16_t& offset, uint16_t& size, 
    const char* field_name
) {
    __unused layout;
    __unused usage_page;
    __unused usage;
    __unused offset;
    __unused size;
    __unused field_name;
}
