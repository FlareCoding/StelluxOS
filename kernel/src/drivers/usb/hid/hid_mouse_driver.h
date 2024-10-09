#ifndef HID_MOUSE_DRIVER_H
#define HID_MOUSE_DRIVER_H
#include "hid_device_driver.h"

class HidMouseDriver : public IHidDeviceDriver {
public:
    HidMouseDriver();
    ~HidMouseDriver() = default;

    void handleEvent(uint8_t* data) override;

private:
    void _handleButtonPress(uint8_t buttons);
    void _handleMovement(int8_t xDisplacement, int8_t yDisplacement);
    void _handleWheel(int8_t wheelDelta);
};

#endif
