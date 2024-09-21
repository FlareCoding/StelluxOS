#include "xhci_mem.h"
#include <paging/tlb.h>
#include <kprint.h>

uint64_t xhciMapMmio(uint64_t pciBarAddress) {
    // Map a conservatively large space for xHCI registers
    for (size_t offset = 0; offset < 0x10000; offset += PAGE_SIZE) {
        void* mmioPage = (void*)(pciBarAddress + offset);
        paging::mapPage(mmioPage, mmioPage, KERNEL_PAGE, PAGE_ATTRIB_CACHE_DISABLED, paging::g_kernelRootPageTable);
    }

    paging::flushTlbAll();
    return pciBarAddress;
}

void* allocXhciMemory(size_t size, size_t alignment, size_t boundary) {
    // Allocate extra memory to ensure we can align the block within the boundary
    size_t totalSize = size + boundary - 1;
    void* memblock = kzmallocAligned(totalSize, alignment);

    if (!memblock) {
        kprint("[XHCI] ======= MEMORY ALLOCATION PROBLEM =======\n");
        while (true);
    }

    // Align the memory block to the specified boundary
    size_t alignedAddress = ((size_t)memblock + boundary - 1) & ~(boundary - 1);
    void* aligned = (void*)alignedAddress;

    // Mark the aligned memory block as uncacheable
    paging::markPageUncacheable(aligned);

    return aligned;
}
