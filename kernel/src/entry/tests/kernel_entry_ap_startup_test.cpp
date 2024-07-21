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
#include <gdt/gdt.h>
#include <interrupts/idt.h>

EXTERN_C void __ap_startup_asm();

void __ap_startup_user_entry();

EXTERN_C
void __ap_startup(int apicid) {
    (void)apicid;

    kprint("Hello from core %i!\n", apicid);

    void* apKernelStack = zallocPages(4);
    kprintInfo("[CPU%i] Kernel stack: 0x%llx\n", apicid, (uint64_t)apKernelStack);

    initializeAndInstallGDT(apicid, apKernelStack);
    kprintInfo("[CPU%i] GDT installed\n", apicid);

    loadIdtr();
    enableInterrupts();
    kprintInfo("[CPU%i] IDT installed and interrupts enabled\n", apicid);

    enableSyscallInterface();
    kprintInfo("[CPU%i] Syscalls enabled\n", apicid);

    uint64_t gs_cpuid;
    asm volatile (
        "movq %%gs:0x20, %0"
        : "=r" (gs_cpuid)
    );

    kprintInfo("[CPU%i] __cpu_id: %i\n", apicid, gs_cpuid);

    while (1);

    // char* usermodeStack = (char*)zallocPages(8);
    // size_t usermodeStackSize = 8 * PAGE_SIZE;

	// __call_lowered_entry(__ap_startup_user_entry, usermodeStack + usermodeStackSize);
}

// void __ap_startup_user_entry() {
//     __kelevate();
//     uint64_t gs_cpuid;
//     asm volatile (
//         "movq %%gs:0x20, %0"
//         : "=r" (gs_cpuid)
//     );

//     kprintInfo("[CPU%i] __cpu_id: %i\n", 1, gs_cpuid);
//     __klower();

//     // ---===== SHOULD GIVE A PAGE FAULT =====---
//     // uint8_t i = *(uint8_t*)0x40000;
//     // kuPrint("%i\n", i);

//     while (1);
// }

void ke_test_ap_startup() {
    auto& globalPageFrameAllocator = paging::getGlobalPageFrameAllocator();

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

        // Test with only one extra CPU
        kprint("Waking up cpu %i\n", 1);
        lapic->sendIpi(1, 0x500);
        msleep(20);

        lapic->sendIpi(1, 0x600 | ((uint32_t)((uint64_t)__ap_startup_code_real_mode_address >> 12)));
        msleep(20);

        *bspdone_ptr = 1;
    });
}
