#include "kernel_entry_tests.h"
#include <core/kprint.h>
#include <kelevate/kelevate.h>
#include <paging/page.h>
#include <paging/page_frame_allocator.h>
#include <memory/kmemory.h>
#include <time/ktime.h>
#include <acpi/acpi_controller.h>
#include <arch/x86/apic.h>
#include <arch/x86/msr.h>
#include <arch/x86/x86_cpu_control.h>
#include <gdt/gdt.h>
#include <interrupts/idt.h>
#include <sched/sched.h>

EXTERN_C void __ap_startup_asm();

void __ap_startup_user_entry();

EXTERN_C
void __ap_startup(int apicid) {
    // Switch onto a clean kernel stack for the core
    void* apKernelStack = zallocPages(4);
    uint64_t apKernelStackTop = reinterpret_cast<uint64_t>(apKernelStack) + PAGE_SIZE;
    asm volatile ("mov %0, %%rsp" :: "r"(apKernelStackTop));

    // Setup the GDT
    initializeAndInstallGDT(apicid, (void*)apKernelStackTop);

    // Install the existing IDT and enable interrupts
    loadIdtr();
    enableInterrupts();

    // Enable syscalls
    enableSyscallInterface();

    // Update cr3
    paging::setCurrentTopLevelPageTable(paging::g_kernelRootPageTable);

    // Initialize the default root kernel swapper task (this thread).
    g_kernelSwapperTasks[apicid].state = ProcessState::RUNNING;
    g_kernelSwapperTasks[apicid].pid = 1;
    zeromem(&g_kernelSwapperTasks[apicid].context, sizeof(CpuContext));
    g_kernelSwapperTasks[apicid].context.rflags |= 0x200;
    g_kernelSwapperTasks[apicid].elevated = 0;

    // Set the current task in the per cpu region
    __per_cpu_data.__cpu[apicid].currentTask = &g_kernelSwapperTasks[apicid];
    __per_cpu_data.__cpu[apicid].currentTask->cpu = apicid;

    // Setup per-cpu stack info
    char* usermodeStack = (char*)zallocPages(8);
    size_t usermodeStackSize = 8 * PAGE_SIZE;
    size_t userStackTop = (uint64_t)(usermodeStack + usermodeStackSize);
    
    __call_lowered_entry(__ap_startup_user_entry, (void*)userStackTop);
}

void __ap_startup_user_entry() {
    uint64_t rsp = 0;
    asm volatile ("mov %%rsp, %0" : "=r"(rsp));

    uint8_t cpu = getCurrentCpuId();

    kuPrint("[CPU%i] stack: 0x%llx\n", cpu, rsp);

    while (1);
}

void ke_test_ap_startup() {
    auto& globalPageFrameAllocator = paging::getGlobalPageFrameAllocator();
    auto& acpiController = AcpiController::get();
    Madt* apicTable = acpiController.getApicTable();

    RUN_ELEVATED({
        void* __ap_startup_code_real_mode_address = (void*)0x8000;

        // Copy the AP startup code to a 16bit address
        globalPageFrameAllocator.lockPage(__ap_startup_code_real_mode_address);
        globalPageFrameAllocator.lockPage((void*)0x9000);
        globalPageFrameAllocator.lockPage((void*)0x11000);
        globalPageFrameAllocator.lockPage((void*)0x15000);
        memcpy(__ap_startup_code_real_mode_address, (void*)__ap_startup_asm, PAGE_SIZE);
        
        uint64_t __ap_startup_c_entry_address = (uint64_t)__ap_startup;
        memcpy((void*)0x9000, &__ap_startup_c_entry_address, sizeof(uint64_t));

        kprint("AP startup vector: 0x%llx\n", 0x600 | ((uint32_t)((uint64_t)__ap_startup_code_real_mode_address >> 12)));

        volatile uint8_t* aprunning_ptr = (volatile uint8_t*)0x11000;  // count how many APs have started
        uint8_t* bspid_ptr = (uint8_t*)0x11008; // BSP id
        uint8_t* bspdone_ptr = (uint8_t*)0x11010; // Spinlock flag

        memcpy((void*)0x15000, paging::g_kernelRootPageTable, PAGE_SIZE);

        *aprunning_ptr = 0;
        *bspid_ptr = 0;
        *bspdone_ptr = 0;

        // get the BSP's Local APIC ID
        __asm__ __volatile__ ("mov $1, %%eax; cpuid; shrl $24, %%ebx;": "=b"(*bspid_ptr) : :);

        auto& lapic = Apic::getLocalApic();

        // for (size_t i = 1; i < apicTable->getCpuCount(); i++) {
        for (size_t i = 1; i < 2; i++) {
            uint8_t apicid = apicTable->getLocalApicDescriptor(i).apicId;

            kprint("Waking up cpu %i\n", apicid);
            lapic->sendIpi(apicid, 0x500);
            msleep(20);

            lapic->sendIpi(apicid, 0x600 | ((uint32_t)((uint64_t)__ap_startup_code_real_mode_address >> 12)));
            msleep(20);
        }

        *bspdone_ptr = 1;
    });
}
