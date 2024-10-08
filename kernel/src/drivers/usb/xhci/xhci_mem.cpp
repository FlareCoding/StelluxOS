#include "xhci_mem.h"
#include <paging/tlb.h>
#include <kprint.h>

uint64_t xhciMapMmio(uint64_t pciBarAddress) {
    const size_t mmioRegionPageCount = 10;
    uint64_t virtualBase = (uint64_t)zallocPages(mmioRegionPageCount);

    // Map a conservatively large space for xHCI registers
    for (size_t offset = 0; offset < mmioRegionPageCount * PAGE_SIZE; offset += PAGE_SIZE) {
        void* mmioPage = (void*)(pciBarAddress + offset);
        void* vaddr = (void*)(virtualBase + offset);
        paging::mapPage(vaddr, mmioPage, KERNEL_PAGE, PAGE_ATTRIB_CACHE_DISABLED, paging::getCurrentTopLevelPageTable());
    }

    paging::flushTlbAll();
    return virtualBase;
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
