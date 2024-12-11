#ifndef PCI_H
#define PCI_H
#include "pci_class_codes.h"
#include <kstl/vector.h>

// PCI configuration space constants
#define PCI_CONFIG_ADDRESS           0xCF8
#define PCI_CONFIG_DATA              0xCFC

// PCI configuration register offsets
#define PCI_COMMAND_OFFSET           0x04
#define PCI_STATUS_OFFSET            0x06
#define PCI_BAR0_OFFSET              0x10
#define PCI_BAR_COUNT                6

// Command register bits
#define PCI_COMMAND_IO_SPACE         (1 << 0)
#define PCI_COMMAND_MEMORY_SPACE     (1 << 1)
#define PCI_COMMAND_BUS_MASTER       (1 << 2)

// BAR type masks
#define PCI_BAR_IO_SPACE_FLAG        (1 << 0)
#define PCI_BAR_TYPE_MASK            (0x3 << 1)
#define PCI_BAR_TYPE_64BIT           (0x2 << 1)
#define PCI_BAR_PREFETCHABLE_FLAG    (1 << 3)

namespace pci {
struct pci_function_desc {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t revision_id;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t cache_line_size;
    uint8_t latency_timer;
    uint8_t header_type;
    uint8_t bist;
    uint32_t bar[6];
    uint32_t cardbus_cisptr;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint32_t expansion_rombase_addr;
    uint8_t capabilities_ptr;
    uint8_t reserved[7];
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t min_grant;
    uint8_t max_latency;
};

enum class pci_bar_type {
    none,
    io_space,
    mmio_32,
    mmio_64
};

struct pci_bar {
    pci_bar_type type;
    uint64_t address;
    uint32_t size;
    bool prefetchable;
};

namespace config {
__PRIVILEGED_CODE
uint8_t read_byte(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);

__PRIVILEGED_CODE
uint16_t read_word(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);

__PRIVILEGED_CODE
uint32_t read_dword(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);

__PRIVILEGED_CODE
void write_byte(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t value);

__PRIVILEGED_CODE
void write_word(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint16_t value);

__PRIVILEGED_CODE
void write_dword(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t value);
} // namespace config

uint32_t make_address(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);
} // namespace pci

#endif // PCI_H
