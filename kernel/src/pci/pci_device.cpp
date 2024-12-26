#include <pci/pci_device.h>
#include <memory/vmm.h>
#include <serial/serial.h>

namespace pci {
pci_device::pci_device(uint64_t function_address, pci_function_desc* desc)
    : m_function_address(function_address), m_desc(desc), m_bars()
{
    m_bus =     (uint8_t)((m_function_address >> 20) & 0xFF);
    m_device =  (uint8_t)((m_function_address >> 15) & 0x1F);
    m_function = (uint8_t)((m_function_address >> 12) & 0x07);

    _parse_bars();
}

__PRIVILEGED_CODE
void pci_device::enable() {
    uint16_t command = _read_command_register();
    command |= (PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE);
    _write_command_register(command);
}

__PRIVILEGED_CODE
void pci_device::disable() {
    uint16_t command = _read_command_register();
    command &= ~(PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE);
    _write_command_register(command);
}

__PRIVILEGED_CODE
void pci_device::enable_bus_mastering() {
    uint16_t command = _read_command_register();
    command |= PCI_COMMAND_BUS_MASTER;
    _write_command_register(command);
}

__PRIVILEGED_CODE
uint16_t pci_device::_read_command_register() {
    return config::read_word(m_bus, m_device, m_function, PCI_COMMAND_OFFSET);
}

__PRIVILEGED_CODE
void pci_device::_write_command_register(uint16_t value) {
    config::write_word(m_bus, m_device, m_function, PCI_COMMAND_OFFSET, value);
}

__PRIVILEGED_CODE
void pci_device::_parse_bars() {
    m_bars.clear();
    m_bars.reserve(PCI_BAR_COUNT);

    for (int i = 0; i < PCI_BAR_COUNT; i++) {
        uint32_t bar_val = m_desc->bar[i];
        if (bar_val == 0) {
            // No BAR present
            pci_bar bar = { pci_bar_type::none, 0, 0, false };
            m_bars.push_back(bar);
            continue;
        }

        pci_bar bar = {};
        bool is_io = (bar_val & PCI_BAR_IO_SPACE_FLAG) != 0;
        bool is_64bit = false;
        
        if (is_io) {
            // I/O space BAR
            bar.type = pci_bar_type::io_space;
            bar.address = bar_val & 0xFFFFFFFCULL;
            bar.prefetchable = false; // Not applicable to I/O

            // Determine size by writing all ones, reading back, and restoring:
            uint32_t offset = PCI_BAR0_OFFSET + i * 4;

            // Save original value
            uint32_t original = bar_val;

            // Write all ones
            config::write_dword(m_bus, m_device, m_function, offset, 0xFFFFFFFF);
            uint32_t size_read = config::read_dword(m_bus, m_device, m_function, offset);

            // Restore original value
            config::write_dword(m_bus, m_device, m_function, offset, original);

            size_read &= 0xFFFFFFFC;
            uint64_t size_val = (~((uint64_t)size_read) + 1);
            bar.size = size_val;
        } else {
            // Memory BAR
            uint8_t type_bits = (bar_val & PCI_BAR_TYPE_MASK) >> 1;
            is_64bit = (type_bits == 0x2);

            if (is_64bit) {
                bar.type = pci_bar_type::mmio_64;
            } else {
                bar.type = pci_bar_type::mmio_32;
            }

            bar.prefetchable = (bar_val & PCI_BAR_PREFETCHABLE_FLAG) != 0;

            // For memory bars, mask off the lower bits
            uint64_t mask = 0xFFFFFFF0ULL;
            uint64_t address = (bar_val & mask);

            // If it's 64-bit, the next BAR holds the high part of the address
            uint32_t offset = PCI_BAR0_OFFSET + i * 4;
            uint32_t original_low = bar_val;
            uint32_t original_high = 0;

            if (is_64bit) {
                // Read the next BAR for high part
                uint32_t bar_val_high = m_desc->bar[i+1];
                address |= ((uint64_t)bar_val_high << 32);
                original_high = bar_val_high;
            }

            bar.address = address;

            // Determine size:
            // Write all ones to the low BAR
            config::write_dword(m_bus, m_device, m_function, offset, 0xFFFFFFFF);
            uint32_t size_read_low = config::read_dword(m_bus, m_device, m_function, offset);

            uint64_t size_val = 0;

            if (is_64bit) {
                // For a 64-bit BAR, also write all ones to the high BAR
                uint32_t offset_high = offset + 4;
                config::write_dword(m_bus, m_device, m_function, offset_high, 0xFFFFFFFF);
                
                // Read back both
                uint32_t size_read_high = config::read_dword(m_bus, m_device, m_function, offset_high);

                // Restore original values
                config::write_dword(m_bus, m_device, m_function, offset, original_low);
                config::write_dword(m_bus, m_device, m_function, offset_high, original_high);

                // Combine into a 64-bit size mask, applying the address mask to the low part
                uint64_t combined_mask = ((uint64_t)size_read_high << 32) | (size_read_low & 0xFFFFFFF0ULL);
                size_val = (~combined_mask) + 1;

                // Since this BAR spans two slots, skip the next one
                i++;
            } else {
                // For a 32-bit memory BAR, just restore the original low value
                config::write_dword(m_bus, m_device, m_function, offset, original_low);

                uint64_t size_mask = (uint64_t)(size_read_low & 0xFFFFFFF0ULL);
                size_val = (~size_mask) + 1;
            }

            bar.size = size_val;
        }

        m_bars.push_back(bar);
    }
}

void pci_device::dbg_print_to_string() const {
    serial::printf(
        "   PCI Device %04x:%04x - %s\n", 
        m_desc->vendor_id, 
        m_desc->device_id, 
        get_pci_device_name(m_desc)
    );

    for (size_t i = 0; i < m_bars.size(); i++) {
        const auto& bar = m_bars[i];
        if (bar.type == pci_bar_type::none) {
            continue; // Skip invalid/non-existent BAR
        }

        const char* bar_type_str = "";
        switch (bar.type) {
            case pci_bar_type::none:     bar_type_str = "none"; break;
            case pci_bar_type::io_space: bar_type_str = "I/O"; break;
            case pci_bar_type::mmio_32:  bar_type_str = "mmio32"; break;
            case pci_bar_type::mmio_64:  bar_type_str = "mmio64"; break;
        }

        serial::printf(
            "      BAR%llu: type: %s, addr: 0x%llx, size: 0x%llx, prefetch: %s\n",
            i, 
            bar_type_str, 
            bar.address, 
            bar.size,
            bar.prefetchable ? "yes" : "no"
        );
    }
}
} // namespace pci
