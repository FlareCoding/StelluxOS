#include "madt.h"
#include <kprint.h>
#include <kelevate/kelevate.h>

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
            IoApicDescriptor* desc = (IoApicDescriptor*)entryPtr;

            // Initialize the IOAPIC
            RUN_ELEVATED({
                auto ioapic = kstl::SharedPtr<IoApic>(
                    new IoApic((uint64_t)desc->ioapicAddress, (uint64_t)desc->globalSystemInterruptBase)
                );

                m_ioApics.pushBack(ioapic);
            });
            break;
        }
        default: break;
        }

        entryPtr += entryLength;  // Move to the next entry
    }
}
