#include "ap_startup.h"
#include <acpi/acpi_controller.h>
#include <paging/page_frame_allocator.h>
#include <paging/page.h>
#include <time/ktime.h>
#include <kelevate/kelevate.h>
#include <gdt/gdt.h>
#include <interrupts/idt.h>
#include <sched/sched.h>

//
// ------------------------------------ IMPORTANT -------------------------------------
// Below addresses are also hardcoded and must match those in arch/x86/asm/ap_startup.s
//

#define AP_STARTUP_ASM_ADDRESS              (void*)0x8000
#define AP_STARTUP_C_ENTRY_ADDRESS          (void*)0x9000
#define AP_STARTUP_PAGE_TABLE_PTR_ADDRESS   (void*)0x15000

#define AP_STARTUP_AP_RUNNING_COUNT_PTR     (volatile uint8_t*)0x11000
#define AP_STARTUP_BSP_ID_PTR               (volatile uint8_t*)0x11008
#define AP_STARTUP_BSP_SPINLOCK_PTR         (volatile uint8_t*)0x11010

//
// Each core will have 512 bytes of stack space during bootup
// until a new 2k stack gets allocated in C entry code. 
//
#define AP_STARTUP_STACK_POOL_BASE          (void*)0x18000
#define AP_STARTUP_STACK_POOL_TOP           (void*)0x70000
#define AP_STARTUP_STACK_POOL_PAGE_COUNT    8
#define AP_STARTUP_STACK_POOL_STACK_SIZE    512

EXTERN_C __PRIVILEGED_CODE void __ap_startup_asm();
EXTERN_C __PRIVILEGED_CODE void apStartupEntryC(int apicid);
void apStartupEntryLowered();

void _acquireApStartupSpinlockFlag() {
    *AP_STARTUP_BSP_SPINLOCK_PTR = 0;
}

void _releaseApStartupSpinlockFlag() {
    *AP_STARTUP_BSP_SPINLOCK_PTR = 1;
}

/**
 * @brief Prepares memory mappings for Application Processor (AP) startup.
 *
 * This function locks the necessary physical pages and sets up the memory mappings 
 * required for the AP startup process. It copies the AP startup assembly code 
 * to a 16-bit real mode address and maps the C entry point to a lower physical 
 * address. These mappings ensure that the AP can transition between real mode 
 * and protected mode during the boot sequence.
 * 
 * Specifically, it:
 * - Locks physical pages for AP startup code, stack, and runtime data.
 * - Copies the startup assembly code to the real mode address.
 * - Maps the C entry point and sets up synchronization pointers for tracking AP states.
 *
 * This setup allows AP cores to boot properly and transition to the system's SMP 
 * environment.
 */
void _prepareApStartupMemoryMappings() {
    auto& globalPageFrameAllocator = paging::getGlobalPageFrameAllocator();

    // Copy the AP startup code to a 16bit address
    globalPageFrameAllocator.lockPhysicalPage(AP_STARTUP_ASM_ADDRESS);
    globalPageFrameAllocator.lockPhysicalPage(AP_STARTUP_C_ENTRY_ADDRESS);
    globalPageFrameAllocator.lockPhysicalPage((void*)AP_STARTUP_AP_RUNNING_COUNT_PTR);
    globalPageFrameAllocator.lockPhysicalPage(AP_STARTUP_PAGE_TABLE_PTR_ADDRESS);

    // Stack region top is 0x70000, 8 pages of 512 bytes of stack 
    globalPageFrameAllocator.lockPhysicalPages(AP_STARTUP_STACK_POOL_BASE, AP_STARTUP_STACK_POOL_PAGE_COUNT);
    
    // Copy the startup assembly code to the real mode address
    memcpy(AP_STARTUP_ASM_ADDRESS, (void*)__ap_startup_asm, PAGE_SIZE);
    
    // Copy the C entry point address to a lower physical address
    uint64_t apStartupCEntryAddress = (uint64_t)apStartupEntryC;
    memcpy(AP_STARTUP_C_ENTRY_ADDRESS, &apStartupCEntryAddress, sizeof(uint64_t));

    // Copy the page table address to a lower physical address
    // to setup paging during AP startup initialization sequence.
    memcpy(AP_STARTUP_PAGE_TABLE_PTR_ADDRESS, paging::g_kernelRootPageTable, PAGE_SIZE);

    // Configure startup flags
    *AP_STARTUP_AP_RUNNING_COUNT_PTR = 0;
    *AP_STARTUP_BSP_ID_PTR = 0;

    // Get the BSP's Local APIC ID
    __asm__ __volatile__ ("mov $1, %%eax; cpuid; shrl $24, %%ebx;" : "=b"(*AP_STARTUP_BSP_SPINLOCK_PTR) ::);
}

void initializeApCores() {
    const uint32_t coreStartupMaxTimeout = 3; // seconds

    auto& acpiController = AcpiController::get();
    Madt* apicTable = acpiController.getApicTable();

    auto& sched = Scheduler::get();

    RUN_ELEVATED({
        // Copy the necessary resources and data to the lower physical address
        // accessible from the 16-bit real mode that APs are in at this point.
        _prepareApStartupMemoryMappings();

        // Acquire the BSP spinlock to manage the
        // initialization sequence of AP cores.
        _acquireApStartupSpinlockFlag();

        // For each core, register it in the scheduler for SMP support
        // and send the appropriate startup signal.
        // *Note* starting at 1 because BSP_ID is 0
        for (size_t cpu = 1; cpu < apicTable->getCpuCount(); cpu++) {
            // Create a scheduler run queue for each detected core
            sched.registerCoreForScheduling(cpu);

            // Get the APIC ID of the core
            uint8_t apicid = apicTable->getLocalApicDescriptor(cpu).apicId;

            // Send the startup IPIs
            bootAndInitApCore(apicid);
        }

        // Let the AP cores continue on their own asynchronously
        _releaseApStartupSpinlockFlag();
    });

    // Wait for all cores to fully start and finish initializing
    sleep(coreStartupMaxTimeout);
}

void bootAndInitApCore(uint8_t apicid) {
    auto& lapic = Apic::getLocalApic();

    lapic->sendIpi(apicid, 0x500);
    msleep(20);

    lapic->sendIpi(apicid, 0x600 | ((uint32_t)((uint64_t)AP_STARTUP_ASM_ADDRESS >> 12)));
    msleep(20);
}

EXTERN_C __PRIVILEGED_CODE
void apStartupEntryC(int apicid) {
    // Allocate a clean 2k kernel stack
    void* apKernelStack = zallocPages(2);
    uint64_t apKernelStackTop = (uint64_t)apKernelStack + 2 * PAGE_SIZE;
    asm volatile ("mov %0, %%rsp" :: "r"(apKernelStackTop));

    // Setup the GDT
    initializeAndInstallGDT(apicid, (void*)apKernelStackTop);

    // Initialize the default root kernel swapper task (this thread).
    g_kernelSwapperTasks[apicid].state = ProcessState::RUNNING;
    g_kernelSwapperTasks[apicid].pid = apicid;
    zeromem(&g_kernelSwapperTasks[apicid].context, sizeof(CpuContext));
    g_kernelSwapperTasks[apicid].context.rflags |= 0x200;
    g_kernelSwapperTasks[apicid].elevated = 0;
    g_kernelSwapperTasks[apicid].cpu = apicid;

    // Set the current task in the per cpu region
    __per_cpu_data.__cpu[apicid].currentTask = &g_kernelSwapperTasks[apicid];

    // Install the existing IDT and enable interrupts
    loadIdtr();
    enableInterrupts();

    // Enable syscalls
    enableSyscallInterface();

    // Update cr3
    paging::setCurrentTopLevelPageTable(paging::g_kernelRootPageTable);
    
    // Setup a clean 8k per-cpu stack
    char* usermodeStack = (char*)zallocPages(8);
    size_t usermodeStackSize = 8 * PAGE_SIZE;
    size_t userStackTop = (uint64_t)(usermodeStack + usermodeStackSize);

    // Set the userStackTop field of the CPU's swapper task
    g_kernelSwapperTasks[apicid].userStackTop = userStackTop;

    __call_lowered_entry(apStartupEntryLowered, (void*)userStackTop);
    while (1);
}

void apStartupEntryLowered() {
    // Initialize core's LAPIC
    Apic::initializeLocalApic();

    // Calibrate apic timer tickrate to 100 milliseconds
    KernelTimer::calibrateApicTimer(100);

    // Start the kernel-wide APIC periodic timer
    KernelTimer::startApicPeriodicTimer();

    while (1);
}
