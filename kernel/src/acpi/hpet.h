#ifndef HPET_H
#define HPET_H
#include "acpi.h"

// HPET Register Offsets
#define HPET_GENERAL_CAPABILITIES_ID_REGISTER   0x00
#define HPET_GENERAL_CONFIGURATION_OFFSET       0x10
#define HPET_MAIN_COUNTER_OFFSET                0xF0

// HPET General Configuration Register Bits
#define HPET_ENABLE_BIT 0x1   // Bit to enable HPET

struct HpetTable {
    AcpiTableHeader header;
    uint8_t hardwareRevId;
    uint8_t comparatorCount:5;
    uint8_t counterSize:1;
    uint8_t reserved:1;
    uint8_t legacyReplacement:1;
    uint16_t pciVendorId;
    uint8_t addressSpaceId;
    uint8_t registerBitWidth;
    uint8_t registerBitOffset;
    uint8_t reserved2;
    uint64_t address;
} __attribute__((packed));

class Hpet {
public:
    Hpet(HpetTable* table);
    ~Hpet() = default;

    void init();
    uint64_t readCounter();

    uint64_t qeueryFrequency() const;

private:
    uint64_t m_base;

    uint64_t _readHpetRegister(uint64_t offset) const;
    void _writeHpetRegister(uint64_t offset, uint64_t value);
};

#endif
