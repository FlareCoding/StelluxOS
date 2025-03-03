#ifdef ARCH_X86_64
#include <smp/smp.h>
#include <acpi/madt.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <time/time.h>
#include <arch/percpu.h>
#include <arch/x86/apic/lapic.h>
#include <arch/x86/gdt/gdt.h>
#include <arch/x86/idt/idt.h>
#include <arch/x86/pat.h>
#include <arch/x86/cpuid.h>
#include <arch/x86/fsgsbase.h>
#include <syscall/syscalls.h>
#include <sched/sched.h>
#include <core/klog.h>

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

#define AP_TASK_STACK_PAGES   2
#define AP_TASK_STACK_SIZE    0x2000 - 0x10

#define AP_SYSTEM_STACK_PAGES   2
#define AP_SYSTEM_STACK_SIZE    0x2000 - 0x10

EXTERN_C __PRIVILEGED_CODE void asm_ap_startup();

__PRIVILEGED_DATA
__attribute__((aligned(PAGE_SIZE)))
uintptr_t g_ap_task_stacks[MAX_SYSTEM_CPUS] = { 0 };

__PRIVILEGED_DATA
__attribute__((aligned(PAGE_SIZE)))
uintptr_t g_ap_system_stacks[MAX_SYSTEM_CPUS] = { 0 };

namespace arch::x86 {
EXTERN_C
__PRIVILEGED_CODE
void ap_startup_entry(uint64_t lapicid, uint64_t acpi_cpu_index) {
    __unused lapicid;

    // Setup kernel stack
    uint64_t ap_system_stack_top =
        reinterpret_cast<uint64_t>(g_ap_system_stacks[acpi_cpu_index]) + AP_SYSTEM_STACK_SIZE;

    // Setup the GDT with userspace support
    init_gdt(acpi_cpu_index, ap_system_stack_top);

    // Setup the IDT and enable interrupts
    install_idt();
    enable_interrupts();

    // Re-mark the currently used kernel serial UART port as unprivileged
    // since the bitmap was reinitialized inside the `init_gdt` function.
    serial::mark_serial_port_unprivileged(serial::g_kernel_uart_port, acpi_cpu_index);

    // Setup the kernel PAT for this processor core
    setup_kernel_pat();

    // Enable fsgsbase instructions if they are supported
    if (cpuid_is_fsgsbase_supported()) {
        enable_fsgsbase();
    }

    // Setup per-cpu area for the bootstrapping processor
    init_ap_per_cpu_area(acpi_cpu_index);

    // Setup BSP's idle task (current)
    task_control_block* ap_idle_task = sched::get_idle_task(acpi_cpu_index);
    zeromem(ap_idle_task, sizeof(task_control_block));
    this_cpu_write(current_task, ap_idle_task);
    this_cpu_write(current_system_stack, ap_system_stack_top);

    current->system_stack_top = ap_system_stack_top;
    current->cpu = acpi_cpu_index;
    current->elevated = 1;
    current->state = process_state::RUNNING;
    current->pid = 0;

    // Enable the syscall interface
    enable_syscall_interface();

    // Initialize the local APIC controller
    auto& lapic = x86::lapic::get();
    lapic->init();

    // Calibrate the local APIC timer to a tickrate of 4ms
    kernel_timer::calibrate_cpu_timer(4);

    // Log that the cpu is now online
    kprint("CPU %u online!\n", current->cpu);

    // Start local APIC timer in order to receive timer IRQs
    kernel_timer::start_cpu_periodic_timer();

    //serial::printf("AP core %i ready with lapic_id: %i\n", acpi_cpu_index, current->cpu);
    while (true) {
        asm volatile ("hlt");
    }
}
} // namespace arch::x86

namespace smp {
struct ap_startup_data {
    volatile uint32_t   cpus_running;       // Tracks the number of running CPUs
    volatile uint32_t   stack_index;        // Index for the temporary stack assignment
    volatile uintptr_t  page_table_address; // Address of the PML4 page table
    volatile uintptr_t  c_entry_address;    // Address of the C entry function
    volatile uint64_t   acpi_cpu_index;     // CPU index from the ACPI MADT table
};

// Map and load the AP startup code
__PRIVILEGED_CODE
void setup_ap_startup_code() {
    // Map the AP startup real mode address
    paging::map_pages(
        AP_STARTUP_ASM_ADDRESS,
        AP_STARTUP_ASM_ADDRESS,
        AP_STARTUP_PAGE_COUNT,
        PTE_DEFAULT_PRIV_KERNEL_FLAGS,
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
    startup_data->c_entry_address = reinterpret_cast<uintptr_t>(&arch::x86::ap_startup_entry);
    startup_data->stack_index = 0;

    return startup_data;
}

__PRIVILEGED_CODE
bool send_ap_startup_sequence(ap_startup_data* startup_data, uint8_t apicid) {
    uint32_t current_running_cpus = startup_data->cpus_running;
    auto& lapic = arch::x86::lapic::get();

    lapic->send_init_ipi(apicid);
    msleep(IPI_INIT_DELAY);

    uint32_t startup_vector = (AP_STARTUP_ASM_ADDRESS >> 12);
    lapic->send_startup_ipi(apicid, startup_vector);
    msleep(IPI_STARTUP_DELAY);

    // Check if the first STARTUP IPI worked and no retry is needed
    if (startup_data->cpus_running == current_running_cpus + 1) {
        return true;
    }

    lapic->send_startup_ipi(apicid, startup_vector);
    msleep(IPI_RETRY_DELAY);

    return (startup_data->cpus_running == current_running_cpus + 1);
}

__PRIVILEGED_CODE
void smp_init() {
    auto& apic_table = acpi::madt::get();

    serial::printf("[*] %u available cpu cores detected\n", apic_table.get_cpu_count());

    // Setup AP startup code and data
    setup_ap_startup_code();
    auto* startup_data = initialize_startup_data();

    uint32_t stack_index = 0;

    for (acpi::lapic_desc& desc : apic_table.get_lapics()) {
        uint8_t cpu_index = desc.acpi_processor_id;

        // Ignore the bootstrapping processor and out-of-range cores
        if (desc.apic_id == BSP_CPU_ID || cpu_index >= MAX_SYSTEM_CPUS) {
            continue;
        }

        // Register a run queue in the scheduler for this core
        sched::scheduler::get().register_cpu_run_queue(cpu_index);

        // Allocate a task stack for the AP core
        void* ap_task_stack = vmm::alloc_contiguous_virtual_pages(AP_TASK_STACK_PAGES, DEFAULT_UNPRIV_PAGE_FLAGS);
        g_ap_task_stacks[cpu_index] = reinterpret_cast<uintptr_t>(ap_task_stack);

        // Allocate a system stack for the AP core
        void* ap_system_stack = vmm::alloc_linear_mapped_persistent_pages(AP_SYSTEM_STACK_PAGES);
        g_ap_system_stacks[cpu_index] = reinterpret_cast<uintptr_t>(ap_system_stack);

        // Allocate a per-cpu area for the processor
        arch::allocate_ap_per_cpu_area(cpu_index);

        startup_data->stack_index = stack_index++;
        startup_data->acpi_cpu_index = cpu_index;

        // Send INIT and STARTUP IPI sequence
        if (!send_ap_startup_sequence(startup_data, desc.apic_id)) {
            serial::printf("[!] Core %u failed to start (lapic_id: %u)\n", cpu_index, desc.apic_id);

            // Free the resources allocated for the core
            sched::scheduler::get().unregister_cpu_run_queue(cpu_index);
            vmm::unmap_contiguous_virtual_pages(g_ap_task_stacks[cpu_index], AP_TASK_STACK_PAGES);
            vmm::unmap_contiguous_virtual_pages(g_ap_system_stacks[cpu_index], AP_SYSTEM_STACK_PAGES);
            arch::deallocate_ap_per_cpu_area(cpu_index);

            continue;
        }

        // Safety delay
        msleep(1);
    }
}
} // namespace smp

#endif // ARCH_X86_64
