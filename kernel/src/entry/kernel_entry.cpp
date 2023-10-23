#include "entry_params.h"
#include <memory/kmemory.h>
#include <graphics/kdisplay.h>
#include <gdt/gdt.h>
#include <paging/phys_addr_translation.h>
#include <paging/page.h>
#include <ports/serial.h>
#include <paging/tlb.h>
#include <interrupts/idt.h>
#include <arch/x86/cpuid.h>
#include <arch/x86/msr.h>
#include <arch/x86/apic.h>
#include <arch/x86/xsdt.h>
#include <arch/x86/per_cpu_data.h>
#include <sched/sched.h>
#include <syscall/syscalls.h>
#include <kelevate/kelevate.h>
#include <kprint.h>

EXTERN_C __PRIVILEGED_CODE void _kentry(KernelEntryParams* params);
extern uint64_t __kern_phys_base;

extern uint64_t __ksymstart;
extern uint64_t __ksymend;

KernelEntryParams g_entry_params;
char __usermode_kernel_entry_stack[0x8000];

void _kuser_entry();

__PRIVILEGED_CODE void _kentry(KernelEntryParams* params) {
    // Setup kernel stack
    uint64_t kernelStackTop = reinterpret_cast<uint64_t>(params->kernelStack) + PAGE_SIZE;
    asm volatile ("mov %0, %%rsp" :: "r"(kernelStackTop));

    // Copy the kernel parameters to an unprivileged region
    memcpy(&g_entry_params, params, sizeof(KernelEntryParams));

    // First thing we have to take care of
    // is setting up the Global Descriptor Table.
    initializeAndInstallGDT(params->kernelStack);
    
    // Enable the syscall functionality
    enableSyscallInterface();

    // Immediately update the kernel physical base
    __kern_phys_base = reinterpret_cast<uint64_t>(params->kernelElfSegments[0].physicalBase);

    // Initialize serial port (for headless output)
    initializeSerialPort(SERIAL_PORT_BASE_COM1);

    // Initialize display and graphics context
    Display::initialize(&params->graphicsFramebuffer, params->textRenderingFont);

    char vendorName[13];
    cpuid_readVendorId(vendorName);
    kprintInfo("===== Stellux Kernel =====\n");
    kprintInfo("CPU Vendor: %s\n", vendorName);
    kprintWarn("Is 5-level paging supported? %i\n\n", cpuid_isLa57Supported());

    __call_lowered_entry(_kuser_entry, __usermode_kernel_entry_stack);
}

void _kuser_entry() {
    // Setup interrupts
    setup_interrupt_descriptor_table();

    RUN_ELEVATED({
        load_idtr();
        enableInterrupts();
    });

    // Setup page frame allocator and lock pages with used resources
    paging::PageFrameAllocator& globalPageFrameAllocator = paging::getGlobalPageFrameAllocator();
    
    RUN_ELEVATED({
        globalPageFrameAllocator.initializeFromMemoryMap(
            g_entry_params.efiMemoryMap.base,
            g_entry_params.efiMemoryMap.descriptorSize,
            g_entry_params.efiMemoryMap.descriptorCount
        );
    });

    globalPageFrameAllocator.lockPage(&g_entry_params);
    globalPageFrameAllocator.lockPages(&__ksymstart, (&__ksymend - &__ksymstart) / PAGE_SIZE + 1);
    globalPageFrameAllocator.lockPage(g_entry_params.textRenderingFont);
    globalPageFrameAllocator.lockPages(g_entry_params.kernelElfSegments, (&__ksymend - &__ksymstart) / PAGE_SIZE + 1);
    globalPageFrameAllocator.lockPages(
        g_entry_params.graphicsFramebuffer.base,
        g_entry_params.graphicsFramebuffer.size / PAGE_SIZE + 1
    );

    uint64_t* addr = (uint64_t*)zallocPage();
    *addr = 4554;

    uint64_t cr3;
    RUN_ELEVATED({
        asm ("mov %%cr3, %0" : "=r"(cr3));
    });

    kuPrint("cr3: 0x%llx\n", cr3);
    kuPrint("Value at addr (0x%llx) is %llu\n\n", addr, *addr);

    while (1) {
        __asm__ volatile("nop");
    }
}
