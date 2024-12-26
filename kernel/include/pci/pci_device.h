#ifndef PCI_DEVICE_H
#define PCI_DEVICE_H
#include "pci.h"

namespace pci {
class pci_device {
public:
    pci_device(uint64_t function_address, pci_function_desc* desc);

    uint16_t vendor_id() const { return m_desc->vendor_id; }
    uint16_t device_id() const { return m_desc->device_id; }
    uint8_t class_code() const { return m_desc->class_code; }
    uint8_t subclass() const { return m_desc->subclass; }
    uint8_t prog_if() const { return m_desc->prog_if; }
    uint8_t revision_id() const { return m_desc->revision_id; }

    // Enable the device by setting memory space and IO space bits in the command register
    __PRIVILEGED_CODE void enable();

    // Disable the device by clearing memory and IO space bits in the command register
    __PRIVILEGED_CODE void disable();

    // Enable bus mastering
    __PRIVILEGED_CODE void enable_bus_mastering();

    const kstl::vector<pci_bar>& get_bars() const { return m_bars; }

    // Debug method to print device info and BARs
    void dbg_print_to_string() const;

private:
    uint32_t m_function_address;
    pci_function_desc* m_desc;

    // Extract bus/device/function from m_device_address:
    uint8_t m_bus;
    uint8_t m_device;
    uint8_t m_function;

    kstl::vector<pci_bar> m_bars;

    __PRIVILEGED_CODE void _parse_bars();
    __PRIVILEGED_CODE void _write_command_register(uint16_t value);
    __PRIVILEGED_CODE uint16_t _read_command_register();
};

} // namespace pci

#endif // PCI_DEVICE_H
