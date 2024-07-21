#include "ioapic.h"
#include <acpi/acpi_controller.h>
#include <paging/page.h>
#include <paging/tlb.h>
#include <kelevate/kelevate.h>

__PRIVILEGED_CODE
IoApic::IoApic(uint64_t physRegs, uint64_t gsib) {
    m_virtualBase = (uint64_t)zallocPage();

    paging::mapPage((void*)m_virtualBase, (void*)physRegs, KERNEL_PAGE, paging::g_kernelRootPageTable);

    auto pte = paging::getPteForAddr((void*)m_virtualBase, paging::g_kernelRootPageTable);
    pte->pageCacheDisabled = 1;
    paging::flushTlbAll();

    m_apicId = (read(IOAPICID) >> 24) & 0xF0;
    m_apicVersion = read(IOAPICVER);

    m_redirectionEntryCount = (read(IOAPICVER) >> 16) + 1;
    m_globalIntrBase = gsib;
}

/*
* Bit of assignment here - implement this on your own. Use the lowerDword & upperDword
* fields of RedirectionEntry using
*                                 ent.lowerDword = read(entNo);
*                                 ent.upperDword = read(entNo);
*                                 return (ent);
*
* Be sure to check that entNo < redirectionEntries()
*
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
* Bit of assignment here - implement this on your own. Use the lowerDword & upperDword
* fields of RedirectionEntry using
*                               write(entNo, ent->lowerDword);
*                               write(entNo, ent->upperDword);
*
* Be sure to check that entNo < redirectionEntries()
*
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

