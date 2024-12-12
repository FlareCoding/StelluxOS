#include <smp/smp.h>
#include <acpi/madt.h>
#include <memory/memory.h>
#include <memory/paging.h>
#include <time/time.h>
#include <arch/x86/apic/lapic.h>
#include <serial/serial.h>

#define AP_STARTUP_ASM_ADDRESS              static_cast<uint64_t>(0x8000)
#define AP_STARTUP_DATA_ADDRESS             static_cast<uint64_t>(0x9000)
#define AP_STARTUP_STACK_REGION_TOP_ADDRESS static_cast<uint64_t>(0x11000)

#define AP_STARTUP_PAGE_COUNT 10

namespace smp {
struct ap_startup_data {
    volatile uint32_t cpus_runing;
    volatile uint32_t stack_index;
    volatile uintptr_t page_table_address;
    volatile uintptr_t c_entry_address;
    volatile uint64_t acpi_cpu_index;
};

EXTERN_C __PRIVILEGED_CODE void asm_ap_startup();

void ap_startup_entry(uint64_t lapicid, uint64_t acpi_cpu_index) {
    serial::com1_printf("AP core %i started with lapic_id: %i\n", acpi_cpu_index, lapicid);
    while (true) {
        asm volatile ("nop");
    }
}

void smp_init() {
    auto& apic_table = acpi::madt::get();
    auto& lapic = arch::x86::lapic::get();

    serial::com1_printf("[*] %u available cpu cores present\n", apic_table.get_cpu_count());

    // Map the AP startup real mode address
    paging::map_pages(
        AP_STARTUP_ASM_ADDRESS,
        AP_STARTUP_ASM_ADDRESS,
        AP_STARTUP_PAGE_COUNT,
        PTE_DEFAULT_KERNEL_FLAGS,
        paging::get_pml4()
    );

    // Copy the startup assembly code to the real mode address
    memcpy(reinterpret_cast<void*>(AP_STARTUP_ASM_ADDRESS), reinterpret_cast<void*>(asm_ap_startup), PAGE_SIZE);

    // Setup the startup-specific data
    ap_startup_data* startup_data = reinterpret_cast<ap_startup_data*>(AP_STARTUP_DATA_ADDRESS);
    startup_data->cpus_runing = 1; // including the BSP
    startup_data->page_table_address = reinterpret_cast<uintptr_t>(paging::get_pml4());
    startup_data->c_entry_address = reinterpret_cast<uintptr_t>(ap_startup_entry);

    uint32_t stack_index = 0;

    for (acpi::lapic_desc& desc : apic_table.get_lapics()) {
        // Ignore the bootstrapping processor
        if (desc.acpi_processor_id == 0) {
            continue;
        }

        serial::com1_printf("booting core %u (lapic_id: %u)\n", desc.acpi_processor_id, desc.apic_id);

        uint8_t apicid = desc.apic_id;
        uint32_t current_running_cpus = startup_data->cpus_runing;
        startup_data->stack_index = stack_index++;
        startup_data->acpi_cpu_index = desc.acpi_processor_id;

        lapic->send_ipi(apicid, 0x500);
        msleep(20);

        lapic->send_ipi(apicid, 0x600 | (static_cast<uint32_t>(AP_STARTUP_ASM_ADDRESS >> 12)));
        msleep(20);

        if (startup_data->cpus_runing != current_running_cpus + 1) {
            lapic->send_ipi(apicid, 0x600 | (static_cast<uint32_t>(AP_STARTUP_ASM_ADDRESS >> 12)));
            msleep(80);
        }

        // Safety delay
        msleep(1);
    }
}
} // namespace smp
