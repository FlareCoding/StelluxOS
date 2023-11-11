#include "madt.h"
#include <kprint.h>

Madt::Madt(MadtDescriptor* desc) {
    uint8_t* entryPtr = desc->tableEntries;
    uint8_t* madtEnd = (uint8_t*)desc + desc->header.length;

    while (entryPtr < madtEnd) {
        uint8_t entryType = *entryPtr;
        uint8_t entryLength = *(entryPtr + 1);

        if (entryType == 0) {
            LocalApicDescriptor* lapic = (LocalApicDescriptor*)entryPtr;

            // Now you can access lapic->AcpiProcessorId, lapic->ApicId, etc.
            // For example, to check if the CPU is enabled:
            if (lapic->flags & 1) {
                LocalApicDescriptor desc;
                memcpy(&desc, lapic, sizeof(LocalApicDescriptor));

                m_localApics.pushBack(desc);
            }
        }

        entryPtr += entryLength;  // Move to the next entry
    }
}
