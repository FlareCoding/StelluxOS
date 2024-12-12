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

#define AP_STARTUP_PAGE_COUNT 10 // Number of pages for mapping the startup code

// LAPIC IPI delivery mode constants
#define IPI_INIT           0x500 // INIT IPI
#define IPI_STARTUP        0x600 // STARTUP IPI with vector shift

// Delays (in ms) for IPI synchronization
#define IPI_INIT_DELAY     20
#define IPI_STARTUP_DELAY  20
#define IPI_RETRY_DELAY    100

namespace smp {
struct ap_startup_data {
    volatile uint32_t   cpus_running;       // Tracks the number of running CPUs
    volatile uint32_t   stack_index;        // Index for the temporary stack assignment
    volatile uintptr_t  page_table_address; // Address of the PML4 page table
    volatile uintptr_t  c_entry_address;    // Address of the C entry function
    volatile uint64_t   acpi_cpu_index;     // CPU index from the ACPI MADT table
};

EXTERN_C __PRIVILEGED_CODE void asm_ap_startup();

__PRIVILEGED_CODE
void ap_startup_entry(uint64_t lapicid, uint64_t acpi_cpu_index);

// Map and load the AP startup code
__PRIVILEGED_CODE
void setup_ap_startup_code() {
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
}

// Initialize the AP startup data structure
__PRIVILEGED_CODE
ap_startup_data* initialize_startup_data() {
    auto* startup_data = reinterpret_cast<ap_startup_data*>(AP_STARTUP_DATA_ADDRESS);
    startup_data->cpus_running = 1; // Including the BSP
    startup_data->page_table_address = reinterpret_cast<uintptr_t>(paging::get_pml4());
    startup_data->c_entry_address = reinterpret_cast<uintptr_t>(ap_startup_entry);
    startup_data->stack_index = 0;

    return startup_data;
}

__PRIVILEGED_CODE
bool send_ap_startup_sequence(ap_startup_data* startup_data, uint8_t apicid) {
    uint32_t current_running_cpus = startup_data->cpus_running;
    auto& lapic = arch::x86::lapic::get();

    lapic->send_ipi(apicid, IPI_INIT);
    msleep(IPI_INIT_DELAY);

    uint32_t startup_command = IPI_STARTUP | (AP_STARTUP_ASM_ADDRESS >> 12);
    lapic->send_ipi(apicid, startup_command);
    msleep(IPI_STARTUP_DELAY);

    // Check if the first STARTUP IPI worked and no retry is needed
    if (startup_data->cpus_running == current_running_cpus + 1) {
        return true;
    }

    lapic->send_ipi(apicid, startup_command);
    msleep(IPI_RETRY_DELAY);

    return (startup_data->cpus_running == current_running_cpus + 1);
}

__PRIVILEGED_CODE
void smp_init() {
    auto& apic_table = acpi::madt::get();

    serial::com1_printf("[*] %u available cpu cores present\n", apic_table.get_cpu_count());

    // Setup AP startup code and data
    setup_ap_startup_code();
    auto* startup_data = initialize_startup_data();

    uint32_t stack_index = 0;

    for (acpi::lapic_desc& desc : apic_table.get_lapics()) {
        // Ignore the bootstrapping processor
        if (desc.acpi_processor_id == 0) {
            continue;
        }

        startup_data->stack_index = stack_index++;
        startup_data->acpi_cpu_index = desc.acpi_processor_id;

        // Send INIT and STARTUP IPI sequence
        if (!send_ap_startup_sequence(startup_data, desc.apic_id)) {
            serial::com1_printf("[!] Core %u failed to start (lapic_id: %u)\n", desc.acpi_processor_id, desc.apic_id);
            continue;
        }

        serial::com1_printf("[*] Successfully started core %u (lapic_id: %u)\n", desc.acpi_processor_id, desc.apic_id);

        // Safety delay
        msleep(1);
    }
}

__PRIVILEGED_CODE
void ap_startup_entry(uint64_t lapicid, uint64_t acpi_cpu_index) {
    serial::com1_printf("AP core %i started with lapic_id: %i\n", acpi_cpu_index, lapicid);
    while (true) {
        asm volatile ("hlt");
    }
}
} // namespace smp
