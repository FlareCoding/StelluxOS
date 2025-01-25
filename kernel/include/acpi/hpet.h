#ifndef HPET_H
#define HPET_H
#include "acpi.h"

// HPET Register Offsets
#define HPET_GENERAL_CAPABILITIES_ID_REGISTER   0x00
#define HPET_GENERAL_CONFIGURATION_OFFSET       0x10
#define HPET_MAIN_COUNTER_OFFSET                0xF0

// HPET General Configuration Register Bits
#define HPET_ENABLE_BIT     (1ULL << 0)   // Bit to enable HPET
#define HPET_64BIT_MODE_BIT (1ULL << 13)  // Bit 13: Enable 64-bit counter mode

namespace acpi {
struct hpet_table {
    acpi_sdt_header header;
    uint8_t hardware_rev_id;
    uint8_t comparator_count:5;
    uint8_t counter_size:1;
    uint8_t reserved:1;
    uint8_t legacy_replacement:1;
    uint16_t pci_vendor_id;
    uint8_t address_space_id;
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t reserved2;
    uint64_t address;
} __attribute__((packed));

/**
 * @class hpet
 * @brief High Precision Event Timer (HPET) manager.
 * 
 * This class is responsible for initializing and managing the HPET, including accessing its registers,
 * reading the counter, and querying its frequency.
 */
class hpet {
public:
    /**
     * @brief Gets the singleton instance of the hpet class.
     * @return The singleton hpet instance.
     * 
     * Provides a global point of access to the HPET manager, ensuring only one instance exists.
     */
    static hpet& get();

    /**
     * @brief Default constructor for the hpet class.
     */
    hpet() = default;

    /**
     * @brief Default destructor for the hpet class.
     */
    ~hpet() = default;

    /**
     * @brief Initializes the HPET using the provided ACPI HPET table.
     * @param acpi_hpet_table Pointer to the ACPI HPET table header.
     * 
     * Parses the HPET table and sets up the base address for accessing HPET registers.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void init(acpi_sdt_header* acpi_hpet_table);

    /**
     * @brief Reads the current value of the HPET counter.
     * @return The current value of the HPET counter.
     * 
     * This function accesses the HPET main counter register to retrieve the current timer value.
     */
    uint64_t read_counter();

    /**
     * @brief Queries the frequency of the HPET timer.
     * @return The frequency of the HPET timer in Hz.
     * 
     * Retrieves the operating frequency of the HPET, which can be used to calculate time intervals.
     */
    uint64_t qeuery_frequency() const;

private:
    uint64_t m_base;

    /**
     * @brief Reads a value from an HPET register.
     * @param offset Offset from the base address of the register to read.
     * @return The value read from the specified HPET register.
     * 
     * This helper function abstracts the process of reading a register at a specific offset.
     */
    uint64_t _read_hpet_register(uint64_t offset) const;

    /**
     * @brief Writes a value to an HPET register.
     * @param offset Offset from the base address of the register to write.
     * @param value The value to write to the specified HPET register.
     * 
     * This helper function abstracts the process of writing a value to a register at a specific offset.
     */
    void _write_hpet_register(uint64_t offset, uint64_t value);
};
} // namespace acpi
#endif // HPET_H
