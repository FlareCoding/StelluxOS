#include "kernel_entry_tests.h"
#include <core/kprint.h>
#include <kelevate/kelevate.h>
#include <acpi/acpi_controller.h>
#include <drivers/usb/xhci.h>

void ke_test_xhci_init() {
    auto& acpiController = AcpiController::get();

    if (acpiController.hasPciDeviceTable()) {
        auto pciDeviceTable = acpiController.getPciDeviceTable();

        size_t idx = pciDeviceTable->findXhciController();
        if (idx != kstl::npos) {
            auto& xhciControllerPciDeviceInfo = pciDeviceTable->getDeviceInfo(idx);

            RUN_ELEVATED({
                auto& xhciDriver = drivers::XhciDriver::get();
                bool status = xhciDriver.init(xhciControllerPciDeviceInfo);

                if (!status) {
                    kprintError("[-] Failed to initialize xHCI controller\n\n");
                } else {
                    kprintInfo("[*] xHCI controller initialized\n\n");
                }
            });
        }
    }
}
