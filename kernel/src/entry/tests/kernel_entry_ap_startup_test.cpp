#include "kernel_entry_tests.h"
#include <core/kprint.h>
#include <kelevate/kelevate.h>
#include <paging/page.h>
#include <paging/page_frame_allocator.h>
#include <memory/kmemory.h>
#include <time/ktime.h>
#include <acpi/acpi_controller.h>
#include <arch/x86/apic.h>

EXTERN_C void __ap_startup_asm();

EXTERN_C void __ap_startup(int apicid) {
    (void)apicid;

    kprint("Hello from core %i!\n", apicid);
	// do what you want to do on the AP
	while(1);
}

void ke_test_ap_startup() {
    auto& acpiController = AcpiController::get();
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

        for (uint32_t apicId = 1; apicId < acpiController.getApic()->getCpuCount(); apicId++) {
            kprint("Waking up cpu %i\n", apicId);
            sendIpi(apicId, 0x500);
            msleep(20);

            sendIpi(apicId, 0x600 | ((uint32_t)((uint64_t)__ap_startup_code_real_mode_address >> 12)));
            msleep(20);
        }

        *bspdone_ptr = 1;
    });
}
