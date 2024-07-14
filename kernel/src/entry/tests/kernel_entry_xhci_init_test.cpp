#include "kernel_entry_tests.h"
#include <core/kprint.h>
#include <kelevate/kelevate.h>
#include <acpi/acpi_controller.h>
#include <drivers/usb/xhci.h>

void ke_test_xhci_init() {
    auto& acpiController = AcpiController::get();

    if (acpiController.hasPciDeviceTable()) {
        auto pciDeviceTable = acpiController.getPciDeviceTable();
        
        for (size_t i = 0; i < pciDeviceTable->getDeviceCount(); i++) {
            //dbgPrintPciDeviceInfo(&pciDeviceTable->getDeviceInfo(i).headerInfo);
        }

        size_t idx = pciDeviceTable->findXhciController();
        if (idx != kstl::npos) {
            auto& xhciControllerPciDeviceInfo = pciDeviceTable->getDeviceInfo(idx);
            kuPrint("bus             : 0x%llx\n", xhciControllerPciDeviceInfo.bus);
            kuPrint("device          : 0x%llx\n", xhciControllerPciDeviceInfo.device);
            kuPrint("function        : 0x%llx\n", xhciControllerPciDeviceInfo.function);
            kuPrint("caps            : 0x%llx\n", xhciControllerPciDeviceInfo.capabilities);
            kuPrint("MSI    Support  : %i\n", HAS_PCI_CAP(xhciControllerPciDeviceInfo, PciCapabilityMsi));
            kuPrint("MSI-X  Support  : %i\n", HAS_PCI_CAP(xhciControllerPciDeviceInfo, PciCapabilityMsiX));

            RUN_ELEVATED({
                auto& xhciDriver = drivers::XhciDriver::get();
                bool status = xhciDriver.init(xhciControllerPciDeviceInfo);

                if (!status) {
                    kuPrint("[-] Failed to initialize xHci USB3.0 controller\n\n");
                }
            });
        }
    }
}
