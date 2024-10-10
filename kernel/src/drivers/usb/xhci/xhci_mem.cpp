#include "xhci_mem.h"
#include <paging/tlb.h>
#include <kelevate/kelevate.h>
#include <kprint.h>

uint64_t xhciMapMmio(uint64_t pciBarAddress) {
    const size_t mmioRegionPageCount = 10;
    uint64_t virtualBase = (uint64_t)zallocPages(mmioRegionPageCount);
    
    RUN_ELEVATED({
        paging::mapPages((void*)virtualBase, (void*)pciBarAddress, mmioRegionPageCount, USERSPACE_PAGE, PAGE_ATTRIB_CACHE_DISABLED, paging::getCurrentTopLevelPageTable());
    });

    return virtualBase;
}

void* allocXhciMemory(size_t size, size_t alignment, size_t boundary) {
    // Allocate extra memory to ensure we can align the block within the boundary
    size_t totalSize = size + boundary - 1;
    void* memblock = kzmallocAligned(totalSize, alignment);

    if (!memblock) {
        kprintf("[XHCI] ======= MEMORY ALLOCATION PROBLEM =======\n");
        while (true);
    }

    // Align the memory block to the specified boundary
    size_t alignedAddress = ((size_t)memblock + boundary - 1) & ~(boundary - 1);
    void* aligned = (void*)alignedAddress;

    // Mark the aligned memory block as uncacheable
    RUN_ELEVATED({
        paging::markPageUncacheable(aligned);
    });

    return aligned;
}
