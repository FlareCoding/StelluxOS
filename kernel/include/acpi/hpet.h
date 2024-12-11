#ifndef HPET_H
#define HPET_H
#include "acpi.h"

// HPET Register Offsets
#define HPET_GENERAL_CAPABILITIES_ID_REGISTER   0x00
#define HPET_GENERAL_CONFIGURATION_OFFSET       0x10
#define HPET_MAIN_COUNTER_OFFSET                0xF0

// HPET General Configuration Register Bits
#define HPET_ENABLE_BIT 0x1   // Bit to enable HPET

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

class hpet {
public:
    static hpet& get();

    hpet() = default;
    ~hpet() = default;

    void init(acpi_sdt_header* acpi_hpet_table);
    uint64_t read_counter();

    uint64_t qeuery_frequency() const;

private:
    uint64_t m_base;

    uint64_t _read_hpet_register(uint64_t offset) const;
    void _write_hpet_register(uint64_t offset, uint64_t value);
};
} // namespace acpi
#endif // HPET_H
