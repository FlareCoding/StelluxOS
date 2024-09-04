#include "ioapic.h"
#include <acpi/acpi_controller.h>
#include <paging/page.h>
#include <paging/tlb.h>
#include <kelevate/kelevate.h>

__PRIVILEGED_CODE
IoApic::IoApic(uint64_t physRegs, uint64_t gsib) {
    m_virtualBase = (uint64_t)zallocPage();

    paging::mapPage((void*)m_virtualBase, (void*)physRegs, KERNEL_PAGE, PAGE_ATTRIB_CACHE_DISABLED, paging::g_kernelRootPageTable);
    paging::flushTlbPage((void*)m_virtualBase);

    m_apicId = (read(IOAPICID) >> 24) & 0xF0;
    m_apicVersion = read(IOAPICVER);

    m_redirectionEntryCount = (read(IOAPICVER) >> 16) + 1;
    m_globalIntrBase = gsib;
}

/*
* @param entNo - entry no. for which redir-entry is required
* @return entry associated with entry no.
*/
IoApic::RedirectionEntry IoApic::getRedirectionEntry(uint8_t entNo) {
    // Check if the entry number is within the valid range
    if (entNo >= m_redirectionEntryCount)
    {
        RedirectionEntry emptyEntry;
        memset(&emptyEntry, 0, sizeof(RedirectionEntry));
        return emptyEntry;
    }

    RedirectionEntry entry;
    // Read the lower and upper 32-bits of the redirection entry
    entry.lowerDword = read(IOAPICREDTBL(entNo));
    entry.upperDword = read(IOAPICREDTBL(entNo) + 1);

    return entry;
}

/*
* @param entNo - entry no. for which redir-entry is required
* @param entry - ptr to entry to write
*/
bool IoApic::writeRedirectionEntry(uint8_t entNo, IoApic::RedirectionEntry *entry) {
    // Check if the entry number is within the valid range
    if (entNo >= m_redirectionEntryCount) {
        return false;
    }

    // Write the lower and upper 32-bits of the redirection entry
    write(IOAPICREDTBL(entNo), entry->lowerDword);
    write(IOAPICREDTBL(entNo) + 1, entry->upperDword);

    return true;
}

/*
* Reads the data present in the register at offset regOff.
*
* @param regOff - the register's offset which is being read
* @return the data present in the register associated with that offset
*/
uint32_t IoApic::read(uint8_t regOff)
{
    uint32_t result = 0;

    RUN_ELEVATED({
        *(uint32_t volatile*)(m_virtualBase + IOAPIC_REGSEL) = regOff;
        result = *(uint32_t volatile*)(m_virtualBase + IOAPIC_IOWIN);
    });

    return result;
}

/*
* Writes the data into the register associated. 
*
* @param regOff - the register's offset which is being written
* @param data - dword to write to the register
*/
void IoApic::write(uint8_t regOff, uint32_t data) {
    RUN_ELEVATED({
        *(uint32_t volatile*)(m_virtualBase + IOAPIC_REGSEL) = regOff;
        *(uint32_t volatile*)(m_virtualBase + IOAPIC_IOWIN) = data;
    });
}

