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
#include <arch/x86/apic.h>
#include <kprint.h>

EXTERN_C void _kentry(KernelEntryParams* params);
extern uint64_t __kern_phys_base;
extern Point g_cursorLocation;

extern uint64_t __ksymstart;
extern uint64_t __ksymend;

void _kentry(KernelEntryParams* params) {
    // First thing we have to take care of
    // is setting up the Global Descriptor Table.
    intializeAndInstallGDT();

    // Immediately update the kernel physical base
    __kern_phys_base = reinterpret_cast<uint64_t>(params->kernelElfSegments[0].physicalBase);

    // Initialize serial port (for headless output)
    initializeSerialPort(SERIAL_PORT_BASE_COM1);

    // Initialize display and graphics context
    Display::initialize(&params->graphicsFramebuffer, params->textRenderingFont);

    paging::PageFrameAllocator& globalPageFrameAllocator = paging::getGlobalPageFrameAllocator();
    globalPageFrameAllocator.initializeFromMemoryMap(
        params->efiMemoryMap.base,
        params->efiMemoryMap.descriptorSize,
        params->efiMemoryMap.descriptorCount
    );

    // Lock pages with used resources
    globalPageFrameAllocator.lockPhysicalPage(params);
    globalPageFrameAllocator.lockPages(&__ksymstart, (&__ksymend - &__ksymstart) / PAGE_SIZE + 1);
    globalPageFrameAllocator.lockPage(&params->textRenderingFont);
    globalPageFrameAllocator.lockPages(&params->kernelElfSegments, (&__ksymend - &__ksymstart) / PAGE_SIZE + 1);
    globalPageFrameAllocator.lockPages(
        params->graphicsFramebuffer.base,
        params->graphicsFramebuffer.size / PAGE_SIZE + 1
    );

    // Update the root pml4 page table
    paging::g_kernelRootPageTable = paging::getCurrentTopLevelPageTable();

    // // Initialize and identity mapping the base address of Local APIC
	// initializeApic();
    //void* apicBase = getApicBase();

    // Map LAPIC into kernel address space
	// for (uint64_t i = (uint64_t)apicBase; i < (uint64_t)apicBase + PAGE_SIZE; i += PAGE_SIZE) {
    //     void* apicPhysicalAddr = __pa(apicBase);
    //     kprint("Mapping LAPIC 0x%llx --> 0x%llx\n", apicBase, apicPhysicalAddr);
	// 	paging::mapPage(apicBase, apicPhysicalAddr, paging::g_kernelRootPageTable, globalPageFrameAllocator);
	// }

    // Lock the LAPIC page
    //globalPageFrameAllocator.lockPage(apicBase);

    // Final LAPIC vector configuration
    //configureApicVector();

    // Setup kernel stack
    void* _globalKernelStack = zallocPage();
    uint64_t _kernelStackTop = reinterpret_cast<uint64_t>(_globalKernelStack) + PAGE_SIZE;
    asm volatile ("mov %0, %%rsp" :: "r"(_kernelStackTop));
    
    // Set up the interrupt descriptor table and enable interrupts
    initializeAndInstallIdt();
    enableInterrupts();

    kprintInfo("System total memory : %llu MB\n", globalPageFrameAllocator.getTotalSystemMemory() / 1024 / 1024);
    kprintInfo("System free memory  : %llu MB\n", globalPageFrameAllocator.getFreeSystemMemory() / 1024 / 1024);
    kprintInfo("System used memory  : %llu MB\n", globalPageFrameAllocator.getUsedSystemMemory() / 1024 / 1024);

    kprintInfo("The kernel is loaded at:\n");
    kprintInfo("    Physical : 0x%llx\n", (uint64_t)params->kernelElfSegments[0].physicalBase);
    kprintInfo("    Virtual  : 0x%llx\n\n", (uint64_t)params->kernelElfSegments[0].virtualBase);

    char vendorName[13];
    cpuid_readVendorId(vendorName);
    kprintInfo("CPU Vendor: %s\n", vendorName);
    kprintWarn("Is 5-level paging supported? %i\n\n", cpuid_isLa57Supported());

    // Initialize and identity mapping the base address of Local APIC
	initializeApic();
    configureApicTimerIrq(IRQ0);

    while (1) {
        __asm__ volatile("hlt");
    }
}
