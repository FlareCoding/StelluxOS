#include <modules/usb/xhci/xhci.h>
#include <serial/serial.h>
#include <sched/sched.h>

namespace modules {
xhci_driver_module::xhci_driver_module() : pci_module_base("xhci_driver_module") {}

bool xhci_driver_module::init() {
    

    return true;
}

bool xhci_driver_module::start() {
    return true;
}

bool xhci_driver_module::stop() {
    return true;
}

bool xhci_driver_module::on_command(
    uint64_t    command,
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
} // namespace modules
