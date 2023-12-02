#include "ioapic.h"
#include <acpi/acpi_controller.h>
#include <paging/page.h>
#include <paging/tlb.h>
#include <kelevate/kelevate.h>

volatile uint32_t* g_ioapicBase = nullptr;

void initializeIoApic() {
    auto& acpiController = AcpiController::get();
    
    if (!acpiController.hasApicTable()) {
        return;
    }

    auto apicTable = acpiController.getApic();
    IoApicDescriptor& ioApic = apicTable->getIoApicDescriptor(0);

    g_ioapicBase = (volatile uint32_t*)(uint64_t)ioApic.ioapicAddress;

    RUN_ELEVATED({
        paging::mapPage(
            (void*)g_ioapicBase,
            (void*)g_ioapicBase,
            USERSPACE_PAGE,
            paging::g_kernelRootPageTable,
            paging::getGlobalPageFrameAllocator()
        );
        paging::flushTlbAll();
    });
}

__PRIVILEGED_CODE
void writeIoApicRegister(uint32_t reg, uint32_t value) {
    g_ioapicBase[IOAPIC_REGSEL] = reg;
    g_ioapicBase[IOAPIC_IOWIN] = value;
}

__PRIVILEGED_CODE
uint32_t readIoApicRegister(uint32_t reg) {
    g_ioapicBase[IOAPIC_REGSEL] = reg;
    return g_ioapicBase[IOAPIC_IOWIN];
}

__PRIVILEGED_CODE
void mapIoApicIrq(uint8_t irq, uint32_t vector, uint8_t apicId) {
    uint32_t lowIndex = 0x10 + 2 * irq;
    uint32_t highIndex = lowIndex + 1;

    writeIoApicRegister(highIndex, apicId << 24); // Destination APIC ID
    writeIoApicRegister(lowIndex, vector); // Vector number and other flags
}
