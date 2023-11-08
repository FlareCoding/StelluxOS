#include "xsdt.h"
#include <memory/kmemory.h>
#include <kprint.h>

// uint32_t queryCpuCount(Madt* madt) {
//     uint32_t cpuCount = 0;
//     uint8_t* entryPtr = madt->tableEntries;
//     uint8_t* madtEnd = (uint8_t*)madt + madt->header.length;

//     while (entryPtr < madtEnd) {
//         uint8_t entryType = *entryPtr;
//         uint8_t entryLength = *(entryPtr + 1);

//         if (entryType == 0) { // Local APIC entry
//             MadtLocalApic* lapic = (MadtLocalApic*)entryPtr;

//             // Now you can access lapic->AcpiProcessorId, lapic->ApicId, etc.
//             // For example, to check if the CPU is enabled:
//             if (lapic->flags & 1) {
//                 cpuCount++;

//                 kprint("Found online CPU!\n");
//                 kprint("local_apic->AcpiProcessorId: %i\n", lapic->acpiProcessorId);
//                 kprint("local_apic->ApicId: %lli\n", lapic->apicId);
//                 kprint("\n");
//             }
//         }

//         entryPtr += entryLength;  // Move to the next entry
//     }

//     return cpuCount;
// }
