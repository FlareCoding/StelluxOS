#include <modules/pci/pci_manager_module.h>
#include <pci/pci_manager.h>
#include <sched/sched.h>
#include <dynpriv/dynpriv.h>
#include <serial/serial.h>

#include <drivers/usb/xhci/xhci.h>

namespace modules {
pci_manager_module::pci_manager_module() : module_base("pci_manager_module") {}

bool pci_manager_module::init() {
    return true;
}

bool pci_manager_module::start() {
    RUN_ELEVATED({
        auto& pci = pci::pci_manager::get();
        for (auto dev : pci.get_devices()) {
            // Search for XHCI controllers
            if (dev->class_code() != PCI_CLASS_SERIAL_BUS_CONTROLLER) {
                continue;
            }
            if (dev->subclass() != PCI_SUBCLASS_SERIAL_BUS_USB) {
                continue;
            }
            if (dev->prog_if() != PCI_PROGIF_USB_XHCI) {
                continue;
            }

            auto xhc_driver = new drivers::xhci_driver();
            xhc_driver->attach_device(dev, true);

            // Create a new process for the driver
            auto driver_process = new process();

            const auto proc_flags =
                process_creation_flags::IS_KERNEL |
                process_creation_flags::SCHEDULE_NOW;

            bool ret = driver_process->init_with_entry(
                xhc_driver->get_name().c_str(),
                reinterpret_cast<task_entry_fn_t>(&pci_manager_module::_driver_thread_entry),
                xhc_driver,
                proc_flags
            );

            if (!ret) {
                serial::printf("[!] Failed to initialize driver process for '%s'\n", xhc_driver->get_name());
                continue;
            }
        }
    });
    return true;
}

bool pci_manager_module::stop() {
    return true;
}

bool pci_manager_module::on_command(
    uint64_t  command,
    const void* data_in,
    size_t      data_in_size,
    void*       data_out,
    size_t      data_out_size
) {
    __unused command;
    __unused data_in;
    __unused data_in_size;
    __unused data_out;
    __unused data_out_size;
    return true;
}

void pci_manager_module::_driver_thread_entry(drivers::pci_device_driver* driver) {
    if (!driver->init_device()) {
        serial::printf("[!] Failed to initialize '%s'\n", driver->get_name());
        sched::exit_process();
    }

    if (!driver->start_device()) {
        serial::printf("[!] Failed to start '%s'\n", driver->get_name());
        sched::exit_process();
    }

    if (!driver->shutdown_device()) {
        serial::printf("[!] Failed to shutdown '%s'\n", driver->get_name());
        sched::exit_process();
    }

    // Final thread exit
    sched::exit_process();
}
} // namespace modules
