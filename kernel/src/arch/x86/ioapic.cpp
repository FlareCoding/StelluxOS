#include "ioapic.h"
#include <acpi/acpi_controller.h>
#include <paging/page.h>
#include <paging/tlb.h>
#include <kelevate/kelevate.h>
#include <kprint.h>

void write_ioapic_register(const uintptr_t apic_base, const uint8_t offset, const uint32_t val) 
{
    /* tell IOREGSEL where we want to write to */
    *(volatile uint32_t*)(apic_base) = offset;
    /* write the value to IOWIN */
    *(volatile uint32_t*)((char*)apic_base + 0x10) = val; 
}
 
uint32_t read_ioapic_register(const uintptr_t apic_base, const uint8_t offset)
{
    /* tell IOREGSEL where we want to read from */
    *(volatile uint32_t*)(apic_base) = offset;
    /* return the data from IOWIN */
    return *(volatile uint32_t*)((char*)apic_base + 0x10);
}

IOAPIC::IOAPIC(unsigned long physRegs, unsigned long gsib) {
    this->virtAddr = (uint64_t)zallocPage();

    paging::mapPage((void*)virtAddr, (void*)physRegs, KERNEL_PAGE, paging::g_kernelRootPageTable);

    auto pte = paging::getPteForAddr((void*)virtAddr, paging::g_kernelRootPageTable);
    pte->pageCacheDisabled = 1;
    paging::flushTlbAll();

    this->apicId = (read(IOAPICID) >> 24) & 0xF0;
    this->apicVer = read(IOAPICVER);// cast to uint8_t (unsigned char) hides upper bits

    //< max. redir entry is given IOAPICVER[16:24]
    this->redirEntryCnt = (read(IOAPICVER) >> 16) + 1;// cast to uint8_t occuring ok!
    this->globalIntrBase = gsib;

    kuPrint("ApicId  : %i\n", (int)apicId);
    kuPrint("ApicVer : %i\n", (int)apicVer);
    kuPrint("GSIB    : %i\n", (int)globalIntrBase);
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
IOAPIC::RedirectionEntry IOAPIC::getRedirEntry(unsigned char entNo) {
    // Check if the entry number is within the valid range
    if (entNo >= redirectionEntries())
    {
        // Handle the error, e.g., by returning an empty RedirectionEntry or throwing an exception
        RedirectionEntry emptyEntry;
        memset(&emptyEntry, 0, sizeof(RedirectionEntry));
        return emptyEntry; // Or handle as per your error handling mechanism
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
void IOAPIC::writeRedirEntry(unsigned char entNo, IOAPIC::RedirectionEntry *entry) {
    // Check if the entry number is within the valid range
    if (entNo >= redirectionEntries())
    {
        // Handle the error, e.g., by returning or throwing an exception
        return; // Or handle as per your error handling mechanism
    }

    // Write the lower and upper 32-bits of the redirection entry
    write(IOAPICREDTBL(entNo), entry->lowerDword);
    write(IOAPICREDTBL(entNo) + 1, entry->upperDword);
}

/*
    * Reads the data present in the register at offset regOff.
    *
    * @param regOff - the register's offset which is being read
    * @return the data present in the register associated with that offset
    */
uint32_t IOAPIC::read(unsigned char regOff)
{
        *(uint32_t volatile*) virtAddr = regOff;
        return *(uint32_t volatile*)(virtAddr + 0x10);
}

/*
    * Writes the data into the register associated. 
    *
    * @param regOff - the register's offset which is being written
    * @param data - dword to write to the register
    */
void IOAPIC::write(unsigned char regOff, uint32_t data)
{
        *(uint32_t volatile*) virtAddr = regOff;
        *(uint32_t volatile*)(virtAddr + 0x10) = data;
}

void setAndEnableKeyboardInterrupt(IOAPIC& ioapic, unsigned char cpuId)
{
    const unsigned char keyboardIRQ = 1; // IRQ1 for keyboard
    const unsigned char ioapicEntry = keyboardIRQ; // Assuming a direct mapping, adjust if necessary

    IOAPIC::RedirectionEntry entry;
    memset(&entry, 0, sizeof(IOAPIC::RedirectionEntry));
    entry.vector = 33; // Example interrupt vector, adjust as needed
    entry.delvMode = 0; // Fixed delivery mode
    entry.destMode = 0; // Physical destination mode
    entry.delvStatus = 0; // Delivery status (0 for delivery complete)
    entry.pinPolarity = 0; // Active high
    entry.remoteIRR = 0; // Remote IRR (0 for edge-triggered)
    entry.triggerMode = 0; // Edge-triggered
    entry.mask = 0; // 0 to enable the interrupt
    entry.destination = cpuId; // Destination APIC ID

    // Write the redirection entry to the IOAPIC
    ioapic.writeRedirEntry(ioapicEntry, &entry);
}
