#include "hid_mouse_driver.h"
#include <kprint.h>

HidMouseDriver::HidMouseDriver() {}

void HidMouseDriver::handleEvent(uint8_t* data) {
    uint8_t buttons = data[0] & 0x07; // Bits 0-2 for buttons 1-3
    int8_t xDisplacement = static_cast<int8_t>(data[1]);
    int8_t yDisplacement = static_cast<int8_t>(data[2]);
    int8_t wheelDelta = static_cast<int8_t>(data[3]);

    if (buttons != 0) {
        _handleButtonPress(buttons);
    }

    if (xDisplacement != 0 || yDisplacement != 0) {
        _handleMovement(xDisplacement, yDisplacement);
    }

    if (wheelDelta != 0) {
        _handleWheel(wheelDelta);
    }
}

void HidMouseDriver::_handleButtonPress(uint8_t buttons) {
    if (buttons & 0x01) {
        kprint("Left Button Clicked\n");
    }
    
    if (buttons & 0x02) {
        kprint("Right Button Clicked\n");
    }

    if (buttons & 0x04) {
        kprint("Middle Button Clicked\n");
    }
}

void HidMouseDriver::_handleMovement(int8_t xDisplacement, int8_t yDisplacement) {
    // static int i = 5;
    // if (--i == 0) {
    //     i = 5;
    //     kprint("(%i, %i)\n", (int)xDisplacement, (int)yDisplacement);
    // }
    __unused xDisplacement;
    __unused yDisplacement;
}

void HidMouseDriver::_handleWheel(int8_t wheelDelta) {
    if (wheelDelta > 0) {
        kprint("Wheel scrolled up by %i\n", wheelDelta);
    } else if (wheelDelta < 0) {
        kprint("Wheel scrolled down by %i\n", -wheelDelta);
    }
}
