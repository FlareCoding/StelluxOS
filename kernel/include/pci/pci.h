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
/**
 * @struct pci_function_desc
 * @brief Represents the configuration space header of a PCI function.
 * 
 * Contains details about the PCI function, including its vendor and device IDs, class codes,
 * BARs (Base Address Registers), and interrupt settings.
 */
struct pci_function_desc {
    uint16_t vendor_id;            // Vendor ID of the PCI device
    uint16_t device_id;            // Device ID of the PCI device
    uint16_t command;              // Command register for controlling device behavior
    uint16_t status;               // Status register indicating device status
    uint8_t revision_id;           // Revision ID of the PCI device
    uint8_t prog_if;               // Programming interface
    uint8_t subclass;              // Subclass code
    uint8_t class_code;            // Class code
    uint8_t cache_line_size;       // Cache line size in 32-bit words
    uint8_t latency_timer;         // Latency timer for bus transactions
    uint8_t header_type;           // Header type indicating device type
    uint8_t bist;                  // Built-in self-test register
    uint32_t bar[6];               // Base Address Registers (BARs) for memory or I/O
    uint32_t cardbus_cisptr;       // CardBus CIS pointer
    uint16_t subsystem_vendor_id;  // Subsystem vendor ID
    uint16_t subsystem_id;         // Subsystem ID
    uint32_t expansion_rombase_addr; // Expansion ROM base address
    uint8_t capabilities_ptr;      // Pointer to the capabilities list
    uint8_t reserved[7];           // Reserved bytes
    uint8_t interrupt_line;        // Interrupt line
    uint8_t interrupt_pin;         // Interrupt pin
    uint8_t min_grant;             // Minimum grant value for bus mastering
    uint8_t max_latency;           // Maximum latency value for bus mastering
};

/**
 * @enum pci_bar_type
 * @brief Represents the type of a PCI Base Address Register (BAR).
 */
enum class pci_bar_type {
    none,       // No BAR is present
    io_space,   // I/O space mapping
    mmio_32,    // 32-bit MMIO (Memory-Mapped I/O) mapping
    mmio_64     // 64-bit MMIO mapping
};

/**
 * @struct pci_bar
 * @brief Represents a parsed PCI Base Address Register (BAR).
 * 
 * Contains information about the BAR's type, address, size, and prefetchability.
 */
struct pci_bar {
    pci_bar_type type;    // Type of the BAR (I/O, MMIO, or none)
    uint64_t address;     // Address mapped by the BAR
    uint32_t size;        // Size of the memory or I/O space mapped by the BAR
    bool prefetchable;    // Indicates if the BAR supports prefetching
};

/**
 * @namespace config
 * @brief Provides low-level access to the PCI configuration space.
 * 
 * Contains functions to read and write configuration registers for PCI devices.
 */
namespace config {
/**
 * @brief Reads a byte from the PCI configuration space.
 * @param bus PCI bus number.
 * @param device PCI device number.
 * @param function PCI function number.
 * @param offset Offset within the configuration space.
 * @return The byte value read from the specified location.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint8_t read_byte(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);

/**
 * @brief Reads a word (16 bits) from the PCI configuration space.
 * @param bus PCI bus number.
 * @param device PCI device number.
 * @param function PCI function number.
 * @param offset Offset within the configuration space.
 * @return The word value read from the specified location.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint16_t read_word(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);

/**
 * @brief Reads a double word (32 bits) from the PCI configuration space.
 * @param bus PCI bus number.
 * @param device PCI device number.
 * @param function PCI function number.
 * @param offset Offset within the configuration space.
 * @return The double word value read from the specified location.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t read_dword(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);

/**
 * @brief Writes a byte to the PCI configuration space.
 * @param bus PCI bus number.
 * @param device PCI device number.
 * @param function PCI function number.
 * @param offset Offset within the configuration space.
 * @param value The byte value to write.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void write_byte(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t value);

/**
 * @brief Writes a word (16 bits) to the PCI configuration space.
 * @param bus PCI bus number.
 * @param device PCI device number.
 * @param function PCI function number.
 * @param offset Offset within the configuration space.
 * @param value The word value to write.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void write_word(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint16_t value);

/**
 * @brief Writes a double word (32 bits) to the PCI configuration space.
 * @param bus PCI bus number.
 * @param device PCI device number.
 * @param function PCI function number.
 * @param offset Offset within the configuration space.
 * @param value The double word value to write.
 * 
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void write_dword(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t value);
} // namespace config

/**
 * @brief Constructs a configuration address for a PCI device.
 * @param bus PCI bus number.
 * @param device PCI device number.
 * @param function PCI function number.
 * @param offset Offset within the configuration space.
 * @return The 32-bit configuration address.
 */
uint32_t make_address(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);
} // namespace pci

#endif // PCI_H
