#include "madt.h"
#include <kprint.h>

Madt::Madt(MadtDescriptor* desc) {
    uint32_t cpuCount = 0;
    uint8_t* entryPtr = desc->tableEntries;
    uint8_t* madtEnd = (uint8_t*)desc + desc->header.length;

    while (entryPtr < madtEnd) {
        uint8_t entryType = *entryPtr;
        uint8_t entryLength = *(entryPtr + 1);

        if (entryType == 0) { // Local APIC entry
            LocalApicDescriptor* lapic = (LocalApicDescriptor*)entryPtr;

            // Now you can access lapic->AcpiProcessorId, lapic->ApicId, etc.
            // For example, to check if the CPU is enabled:
            if (lapic->flags & 1) {
                cpuCount++;

                kuPrint("Found online CPU!\n");
                kuPrint("local_apic->AcpiProcessorId: %i\n", lapic->acpiProcessorId);
                kuPrint("local_apic->ApicId: %lli\n", lapic->apicId);
                kuPrint("\n");
            }
        }

        entryPtr += entryLength;  // Move to the next entry
    }
}
