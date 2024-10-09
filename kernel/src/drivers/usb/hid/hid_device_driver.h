#ifndef HID_DEVICE_DRIVER_H
#define HID_DEVICE_DRIVER_H
#include <ktypes.h>

class IHidDeviceDriver {
public:
    IHidDeviceDriver() = default;
    virtual ~IHidDeviceDriver() = default;

    virtual void handleEvent(uint8_t* data) = 0;
};

#endif
