#ifndef HID_KEYBOARD_DRIVER_H
#define HID_KEYBOARD_DRIVER_H
#include "xhci_hid.h"

class HidKeyboardDriver : public IHidDeviceDriver {
public:
    HidKeyboardDriver();
    ~HidKeyboardDriver() = default;

    void handleEvent(uint8_t* data) override;

private:
    uint8_t m_previousModifiers;
    uint8_t m_previousKeys[6];

    char _mapKeycodeToChar(uint8_t keycode, bool shiftPressed);
    void _handleKeyPress(uint8_t keycode, bool shiftPressed);
    void _handleKeyRelease(uint8_t keycode);
};

#endif
