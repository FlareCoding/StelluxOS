#include <pci/pci_manager.h>
#include <memory/vmm.h>

namespace pci {
pci_manager& pci_manager::get() {
    GENERATE_STATIC_SINGLETON(pci_manager);
}

void pci_manager::init(acpi::acpi_sdt_header* mcfg_ptr) {
    _parse_mcfg(mcfg_ptr);

    for (uint8_t bus = m_mcfg_segment->start_bus; bus < m_mcfg_segment->end_bus; bus++) {
        _enumerate_bus(m_mcfg_segment->base_address, bus);
    }
}

void pci_manager::_parse_mcfg(acpi::acpi_sdt_header* mcfg_ptr) {
    // MCFG structure:
    // header + reserved field (8 bytes) + segment entries
    mcfg_table* table = reinterpret_cast<mcfg_table*>(mcfg_ptr);
    uint32_t offset = sizeof(mcfg_table);

    // Stellux for now only supports systems with one PCI segment
    m_mcfg_segment = reinterpret_cast<pci_segment_entry*>(reinterpret_cast<uintptr_t>(table) + offset);
}

void pci_manager::_enumerate_bus(uint64_t segment_base_addr, uint8_t bus) {
    uint64_t bus_offset = bus << 20;
    uint64_t bus_address = segment_base_addr + bus_offset;

    void* bus_virtual_base = vmm::map_physical_page(bus_address, DEFAULT_PRIV_PAGE_FLAGS);

    pci_function_desc* desc = reinterpret_cast<pci_function_desc*>(bus_virtual_base);

    if (desc->device_id == 0 || desc->device_id == 0xffff) {
        vmm::unmap_virtual_page(reinterpret_cast<uintptr_t>(desc));
        return;
    }

    for (uint64_t device = 0; device < 32; device++){
        _enumerate_device(bus_address, device);
    }
}

void pci_manager::_enumerate_device(uint64_t bus_addr, uint8_t device) {
    uint64_t device_offset = device << 15;
    uint64_t device_address = bus_addr + device_offset;

    void* device_virtual_base = vmm::map_physical_page(device_address, DEFAULT_PRIV_PAGE_FLAGS);

    pci_function_desc* desc = reinterpret_cast<pci_function_desc*>(device_virtual_base);

    if (desc->device_id == 0 || desc->device_id == 0xffff) {
        vmm::unmap_virtual_page(reinterpret_cast<uintptr_t>(desc));
        return;
    }

    for (uint64_t function = 0; function < 8; function++){
        _enumerate_function(device_address, function);
    }
}

void pci_manager::_enumerate_function(uint64_t device_addr, uint8_t function) {
    uint64_t function_offset = function << 12;

    uint64_t function_address = device_addr + function_offset;
    void* function_virtual_base = vmm::map_physical_page(function_address, DEFAULT_PRIV_PAGE_FLAGS);

    pci_function_desc* desc = reinterpret_cast<pci_function_desc*>(function_virtual_base);

    if (desc->device_id == 0 || desc->device_id == 0xffff) {
        vmm::unmap_virtual_page(reinterpret_cast<uintptr_t>(desc));
        return;
    }

    // Register and store the PCI device class instance
    auto dev = kstl::make_shared<pci_device>(function_address, desc);
    m_devices.push_back(dev);
}

kstl::shared_ptr<pci_device> pci_manager::find_device(uint16_t vendor_id, uint16_t device_id) const {
    for (auto& dev : m_devices) {
        if (dev->vendor_id() == vendor_id && dev->device_id() == device_id) {
            return dev;
        }
    }

    return kstl::shared_ptr<pci_device>(nullptr);
}

kstl::vector<kstl::shared_ptr<pci_device>> pci_manager::find_all_devices(uint16_t vendor_id, uint16_t device_id) const {
    kstl::vector<kstl::shared_ptr<pci_device>> matches;

    for (auto& dev : m_devices) {
        if (dev->vendor_id() == vendor_id && dev->device_id() == device_id) {
            matches.push_back(dev);
        }
    }

    return matches;
}

kstl::shared_ptr<pci_device> pci_manager::find_by_class(uint8_t class_code, uint8_t subclass) const {
    for (auto& dev : m_devices) {
        if (dev->class_code() == class_code && dev->subclass() == subclass) {
            return dev;
        }
    }
    
    return kstl::shared_ptr<pci_device>(nullptr);
}

kstl::shared_ptr<pci_device> pci_manager::find_by_progif(uint8_t class_code, uint8_t subclass, uint8_t prog_if) const {
    for (auto& dev : m_devices) {
        if (dev->class_code() == class_code && dev->subclass() == subclass && dev->prog_if() == prog_if) {
            return dev;
        }
    }
    
    return kstl::shared_ptr<pci_device>(nullptr);
}

kstl::vector<kstl::shared_ptr<pci_device>> pci_manager::find_all_by_class(uint8_t class_code, uint8_t subclass) const {
    kstl::vector<kstl::shared_ptr<pci_device>> matches;

    for (auto& dev : m_devices) {
        if (dev->class_code() == class_code && dev->subclass() == subclass) {
            matches.push_back(dev);
        }
    }

    return matches;
}
} // namespace pci
