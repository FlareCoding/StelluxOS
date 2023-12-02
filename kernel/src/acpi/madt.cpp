#include "madt.h"
#include <kprint.h>

Madt::Madt(MadtDescriptor* desc) {
    uint8_t* entryPtr = desc->tableEntries;
    uint8_t* madtEnd = (uint8_t*)desc + desc->header.length;

    while (entryPtr < madtEnd) {
        uint8_t entryType = *entryPtr;
        uint8_t entryLength = *(entryPtr + 1);

        switch (entryType) {
        case 0: { // Local APIC
            LocalApicDescriptor* lapic = (LocalApicDescriptor*)entryPtr;
            if (lapic->flags & 1) {
                LocalApicDescriptor desc;
                memcpy(&desc, lapic, sizeof(LocalApicDescriptor));
                m_localApics.pushBack(desc);
            }
            break;
        }
        case 1: { // IOAPIC
            IoApicDescriptor* ioapic = (IoApicDescriptor*)entryPtr;
            IoApicDescriptor desc;
            memcpy(&desc, ioapic, sizeof(IoApicDescriptor));

            m_ioApics.pushBack(desc);
            break;
        }
        default: break;
        }

        entryPtr += entryLength;  // Move to the next entry
    }
}
