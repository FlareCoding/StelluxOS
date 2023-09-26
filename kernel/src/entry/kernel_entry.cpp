#include "entry_params.h"
#include <memory/kmemory.h>
#include <graphics/kdisplay.h>
#include <gdt/gdt.h>
#include <paging/phys_addr_translation.h>
#include <paging/page.h>
#include <ports/serial.h>
#include <arch/x86/cpuid.h>
#include <paging/tlb.h>
#include <interrupts/idt.h>
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

    kprint("paging::g_kernelRootPageTable    : 0x%llx\n", paging::g_kernelRootPageTable);
    kprint("cr3                              : 0x%llx\n\n", __pa(paging::getCurrentTopLevelPageTable()));

    for (int i = 0; i < 162; i++) {
        void* page = globalPageFrameAllocator.requestFreePage();

        if (i > 156) {
            kprint("requested page [%i]: 0x%llx  (backed by 0x%llx)\n", i, page, __pa(page));
        }
    }

    uint64_t* testPage = (uint64_t*)globalPageFrameAllocator.requestFreePage();
    kprint("testPage: 0x%llx backed by 0x%llx\n", testPage, __pa(testPage));
    *testPage = 4554;

    kprint("Reading 0x%llx --> %i\n", testPage, *testPage);
    kprint("Reading 0x%llx --> %i\n\n", __pa(testPage), *((uint64_t*)__pa(testPage)));

    char vendorName[13];
    cpuid_readVendorId(vendorName);
    kprintInfo("CPU Vendor: %s\n", vendorName);
    kprintWarn("Is 5-level paging supported? %i\n\n", cpuid_isLa57Supported());

    int* scratchpad = (int*)zallocPage();
    kprint("scratchpad is at 0x%llx\n", scratchpad);

    *scratchpad = 8;
    kprint("Value of scratchpad: %i\n", *scratchpad);

    paging::pte_t* scratchpadPte = paging::getPteForAddr(scratchpad, paging::g_kernelRootPageTable);
    paging::dbgPrintPte(scratchpadPte);
    kprint("\n");

    scratchpadPte->present = 0;
    paging::flushTlbAll();

    uint64_t val = *scratchpad;
    (void)val;

    while (1) {
        __asm__ volatile("hlt");
    }
}
