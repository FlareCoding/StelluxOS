#include "entry_params.h"
#include <memory/kmemory.h>
#include <graphics/kdisplay.h>
#include <gdt/gdt.h>
#include <paging/phys_addr_translation.h>
#include <paging/page_frame_allocator.h>
#include <kprint.h>

EXTERN_C void _kentry(KernelEntryParams* params);
extern uint64_t __kern_phys_base;

void _kentry(KernelEntryParams* params) {
    // First thing we have to take care of
    // is setting up the Global Descriptor Table.
    intializeAndInstallGDT();

    // Immediately update the kernel physical base
    __kern_phys_base = reinterpret_cast<uint64_t>(params->kernelElfSegments[0].physicalBase);

    // Initialize display and graphics context
    Display::initialize(&params->graphicsFramebuffer, params->textRenderingFont);

    kprintInfo("The kernel is loaded at:\n");
    kprintInfo("    Physical : 0x%llx\n", (uint64_t)params->kernelElfSegments[0].physicalBase);
    kprintInfo("    Virtual  : 0x%llx\n\n", (uint64_t)params->kernelElfSegments[0].virtualBase);

    paging::PageFrameAllocator& globalPageFrameAllocator = paging::getGlobalPageFrameAllocator();
    globalPageFrameAllocator.initializeFromMemoryMap(
        params->efiMemoryMap.base,
        params->efiMemoryMap.descriptorSize,
        params->efiMemoryMap.descriptorCount
    );

    for (int i = 0; i < 11; i++) {
        void* page = globalPageFrameAllocator.requestFreePage();
        kprint("requested page: 0x%llx  (backed by 0x%llx)\n", page, __pa(page));
    }

    uint64_t* testPage = (uint64_t*)globalPageFrameAllocator.requestFreePage();
    *testPage = 4554;

    kprint("Reading 0x%llx --> %i\n", testPage, *testPage);
    kprint("Reading 0x%llx --> %i\n", __pa(testPage), *((uint64_t*)__pa(testPage)));

    while (1) {
        __asm__ volatile("hlt");
    }
}
