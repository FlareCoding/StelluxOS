#include <modules/usb/xhci/xhci_usb_hid_mouse_driver.h>
#include <serial/serial.h>

uint64_t g_mouse_cursor_pos_x = 100;
uint64_t g_mouse_cursor_pos_y = 100;

void xhci_usb_hid_mouse_driver::on_event(uint8_t* data) {
    // uint8_t buttons = data[0];
    int8_t dx = static_cast<int8_t>(data[2]);
    int8_t dy = static_cast<int8_t>(data[4]);

    // bool left_button = buttons & 0x01;
    // bool right_button = buttons & 0x02;
    // bool middle_button = buttons & 0x04;

    g_mouse_cursor_pos_x += dx;
    g_mouse_cursor_pos_y += dy;
}
