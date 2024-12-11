#include <pci/pci.h>
#include <memory/vmm.h>
#include <serial/serial.h>

namespace pci {
namespace config {
__PRIVILEGED_CODE
uint8_t read_byte(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
    uint32_t address = make_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t value = inl(PCI_CONFIG_DATA);

    // Extract the correct byte from the dword
    uint8_t shift = (offset & 0x3) * 8;
    return (uint8_t)((value >> shift) & 0xFF);
}

__PRIVILEGED_CODE
uint16_t read_word(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
    uint32_t address = make_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t value = inl(PCI_CONFIG_DATA);

    // Extract the correct word from the dword
    uint8_t shift = (uint8_t)((offset & 0x2) * 8);
    return (uint16_t)((value >> shift) & 0xFFFF);
}

__PRIVILEGED_CODE
uint32_t read_dword(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
    uint32_t address = make_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

__PRIVILEGED_CODE
void write_byte(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t value) {
    uint32_t address = make_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);

    // We must preserve other bytes in this 32-bit field
    uint32_t old = inl(PCI_CONFIG_DATA);
    uint8_t shift = (uint8_t)((offset & 0x3) * 8);
    uint32_t mask = 0xFF << shift;
    uint32_t new_value = (old & ~mask) | ((value & 0xFF) << shift);

    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, new_value);
}

__PRIVILEGED_CODE
void write_word(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint16_t value) {
    uint32_t address = make_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);

    // Preserve the other word in this dword
    uint32_t old = inl(PCI_CONFIG_DATA);
    uint8_t shift = (uint8_t)((offset & 0x2) * 8);
    uint32_t mask = 0xFFFF << shift;
    uint32_t new_value = (old & ~mask) | ((value & 0xFFFF) << shift);

    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, new_value);
}

__PRIVILEGED_CODE
void write_dword(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t value) {
    uint32_t address = make_address(bus, device, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}
} // namespace config

uint32_t make_address(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset) {
    uint32_t address = (1U << 31) 
                    | ((uint32_t)bus << 16) 
                    | ((uint32_t)device << 11)
                    | ((uint32_t)function << 8)
                    | (offset & 0xFC); // Align to 4-byte boundary
    return address;
}
} // namespace pci
