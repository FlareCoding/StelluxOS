#include "xhci_mem.h"
#include <paging/tlb.h>

uint64_t xhciMapMmio(uint64_t pciBarAddress) {
    // Map a conservatively large space for xHCI registers
    for (size_t offset = 0; offset < 0x10000; offset += PAGE_SIZE) {
        void* mmioPage = (void*)(pciBarAddress + offset);
        paging::mapPage(mmioPage, mmioPage, KERNEL_PAGE, PAGE_ATTRIB_CACHE_DISABLED, paging::g_kernelRootPageTable);
    }

    paging::flushTlbAll();
    return pciBarAddress;
}

