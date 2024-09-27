#ifndef DEVICE_DRIVER_MANAGER_H
#define DEVICE_DRIVER_MANAGER_H
#include "device_driver.h"

#define PCI_DEVICE_IDENTIFIER_LEN 3

class DeviceDriverManager {
public:
    DeviceDriverManager() = delete;
    ~DeviceDriverManager() = delete;

    static DeviceDriver* getDeviceDriver(uint8_t identifier[PCI_DEVICE_IDENTIFIER_LEN], bool& needsIrq);

    // Must be called after MCFG (PCI Table) has finished initializing and parsing devices
    static void installPciDeviceDrivers();
};

#endif
