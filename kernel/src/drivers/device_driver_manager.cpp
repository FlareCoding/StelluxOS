#include "device_driver_manager.h"
#include <memory/kmemory.h>
#include <kelevate/kelevate.h>
#include <acpi/acpi_controller.h>
#include <sched/sched.h>
#include <drivers/usb/xhci/xhci.h>
#include <kstring.h>
#include <kprint.h>

#define GET_PCI_DEVICE_IDENTIFIER(deviceInfo) {\
            deviceInfo.headerInfo.classCode, \
            deviceInfo.headerInfo.subclass, \
            deviceInfo.headerInfo.progIF \
        }

const uint8_t DEVICE_IDENTIFIER_XHCI[3] = { 0x0C, 0x03, 0x30 };

#define DEVICE_MATCHES(id) (memcmp(identifier, (void*)id, PCI_DEVICE_IDENTIFIER_LEN) == 0)

struct DriverEntryThreadParams {
    PciDeviceInfo*  pciInfo;
    uint8_t         irqVector;
    DeviceDriver*   driverInstance;
};

void startDriverEntryThread(void* threadParams) {
    auto params = (DriverEntryThreadParams*)threadParams;
    params->driverInstance->driverInit(*params->pciInfo, params->irqVector);

    exitKernelThread();
}

DeviceDriver* DeviceDriverManager::getDeviceDriver(uint8_t identifier[PCI_DEVICE_IDENTIFIER_LEN], bool& needsIrq) {
    if (DEVICE_MATCHES(DEVICE_IDENTIFIER_XHCI)) {
        needsIrq = true;
        return static_cast<DeviceDriver*>(new XhciDriver());
    }

    return nullptr;
};

void DeviceDriverManager::installPciDeviceDrivers() {
    auto& acpi = AcpiController::get();
    auto pciTable = acpi.getPciDeviceTable();

    for (size_t i = 0; i < pciTable->getDeviceCount(); i++) {
        // Get PCI device information structure
        auto& deviceInfo = pciTable->getDeviceInfo(i);

        bool needsIrq;
        uint8_t identifier[3] = GET_PCI_DEVICE_IDENTIFIER(deviceInfo);

        // Attempt to find the driver instance
        DeviceDriver* driver = getDeviceDriver(identifier, needsIrq);

        // Continue to the next device if no driver is found
        if (!driver) {
            continue;
        }

        // Construct the device driver thread params
        auto params = new DriverEntryThreadParams();
        params->pciInfo = &deviceInfo;
        params->driverInstance = driver;

        // Allocate the IRQ vector and perform any necessary IRQ routing if needed 
        if (needsIrq) {
            params->irqVector = findFreeIrqVector();

            uint8_t legacyIrqLine = deviceInfo.headerInfo.interruptLine;

            if (legacyIrqLine != 255) {
                routeIoApicIrq(legacyIrqLine, params->irqVector, 0, IRQ_LEVEL_TRIGGERED);
            } else if (HAS_PCI_CAP(deviceInfo, PciCapability::PciCapabilityMsiX)) {
                PciMsiXCapability cap;
                RUN_ELEVATED({
                    cap = readMsixCapability(
                        deviceInfo.bus,
                        deviceInfo.device,
                        deviceInfo.function
                    );
                });

                bool enabled = cap.enableBit;
                kuPrint("MSI-X capability: %s\n", enabled ? "enabled" : "disabled");
            } else if (HAS_PCI_CAP(deviceInfo, PciCapability::PciCapabilityMsi)) {
                RUN_ELEVATED({
                    if (setupMsiInterrupt(deviceInfo, params->irqVector, 0)) {
                        kprintInfo("MSI interrupts enabled!\n");
                    } else {
                        kprintError("Failed to setup MSI interrupts\n");
                    }
                });
            }
        }

        // Start the kernel thread that will launch and init the driver
        auto driverThread = createKernelTask(startDriverEntryThread, params);

        // Register driver task name
        const char* driverName = driver->getName();
        const size_t driverNameLen = strlen(driverName);
        memcpy(driverThread->name, driverName, driverNameLen);
        
        Scheduler::get().addTask(driverThread);
    }
}
