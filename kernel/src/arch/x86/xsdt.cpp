#include "xsdt.h"
#include <memory/kmemory.h>
#include <kprint.h>

uint32_t get_cpu_count(Madt* madt) {
    uint32_t cpu_count = 0;
    uint8_t* entry_ptr = madt->TableEntries;
    uint8_t* madt_end = (uint8_t*)madt + madt->Header.Length;

    while (entry_ptr < madt_end) {
        uint8_t entry_type = *entry_ptr;
        uint8_t entry_length = *(entry_ptr + 1);

        if (entry_type == 0) { // Local APIC entry
            MadtLocalApic* local_apic = (MadtLocalApic*)entry_ptr;

            // Now you can access local_apic->AcpiProcessorId, local_apic->ApicId, etc.
            // For example, to check if the CPU is enabled:
            if (local_apic->Flags & 1) {
                cpu_count++;

                kprint("Found online CPU!\n");
                kprint("local_apic->AcpiProcessorId: %lli\n", local_apic->AcpiProcessorId);
                kprint("local_apic->ApicId: %lli\n", local_apic->ApicId);
                kprint("\n");
            }
        }

        entry_ptr += entry_length;  // Move to the next entry
    }

    return cpu_count;
}
