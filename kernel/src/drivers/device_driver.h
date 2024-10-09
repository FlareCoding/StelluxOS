#ifndef DEVICE_DRIVER_H
#define DEVICE_DRIVER_H
#include <pci/pci.h>

#define DEVICE_INIT_SUCCESS 0
#define DEVICE_INIT_FAILURE 1

struct DeviceDriverParams {
    PciDeviceInfo* pciInfo;
    uint8_t irqVector;
};

class DeviceDriver {
public:
    DeviceDriver() = default;
    virtual ~DeviceDriver() = default;

    virtual const char* getName() const = 0;
    virtual int driverInit(PciDeviceInfo& pciInfo, uint8_t irqVector) = 0;
};

#endif
