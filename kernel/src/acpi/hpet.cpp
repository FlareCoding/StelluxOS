#include "hpet.h"
#include <paging/page.h>
#include <paging/phys_addr_translation.h>
#include <memory/kmemory.h>

Hpet::Hpet(HpetTable* table) {
    void* physicalBase = reinterpret_cast<void*>(table->address);
    void* virtualBase = zallocPage();

    paging::mapPage(virtualBase, physicalBase, USERSPACE_PAGE, paging::g_kernelRootPageTable);

    m_base = reinterpret_cast<uint64_t>(virtualBase);
}

void Hpet::init() {
    // Enable the HPET by setting the ENABLE bit in the General Configuration Register
    uint64_t genConfig = _readHpetRegister(HPET_GENERAL_CONFIGURATION_OFFSET);
    genConfig |= HPET_ENABLE_BIT;
    _writeHpetRegister(HPET_GENERAL_CONFIGURATION_OFFSET, genConfig);
}

uint64_t Hpet::readCounter() {
    return _readHpetRegister(HPET_MAIN_COUNTER_OFFSET);
}

uint64_t Hpet::qeueryFrequency() const {
    uint64_t gcIdReg = _readHpetRegister(HPET_GENERAL_CAPABILITIES_ID_REGISTER);
    uint32_t clockPeriodFs = (uint32_t)(gcIdReg >> 32);  // The upper 32 bits contain the period

    if (clockPeriodFs == 0) {
        return 0;
    }

    // Convert the period from femtoseconds to Hz
    return 1000000000000000ULL / clockPeriodFs;
}

uint64_t Hpet::_readHpetRegister(uint64_t offset) const {
    return *(volatile uint64_t*)(m_base + offset);
}

void Hpet::_writeHpetRegister(uint64_t offset, uint64_t value) {
    *(volatile uint64_t*)(m_base + offset) = value;
}
