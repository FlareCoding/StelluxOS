#include <drivers/usb/xhci/xhci_mem.h>
#include <drivers/usb/xhci/xhci_log.h>
#include <memory/vmm.h>
#include <memory/allocators/dma_allocator.h>
#include <serial/serial.h>
#include <dynpriv/dynpriv.h>

__PRIVILEGED_CODE
uintptr_t xhci_map_mmio(uint64_t pci_bar_address, uint32_t bar_size) {
    size_t page_count = bar_size / PAGE_SIZE;

    /*
     * Some devices have been seen to report the BAR size as 0x10 for XHCI,
     * however most commonly xhci controller takes up at most 4-5 pages.
     */
    if (page_count == 0) {
        page_count = 5;
    }

    void* vbase = vmm::map_contiguous_physical_pages(
        pci_bar_address,
        page_count,
        DEFAULT_UNPRIV_PAGE_FLAGS | PTE_PCD
    );

    return reinterpret_cast<uintptr_t>(vbase);
}

void* alloc_xhci_memory(size_t size, size_t alignment, size_t boundary) {
    if (size == 0) {
        xhci_error("Attempted DMA allocation with size 0!\n");
        while (true);
    }

    if (alignment == 0) {
        xhci_error("Attempted DMA allocation with alignment 0!\n");
        while (true);
    }

    if (boundary == 0) {
        xhci_error("Attempted DMA allocation with boundary 0!\n");
        while (true);
    }

    void* memblock = nullptr;
    RUN_ELEVATED({
        auto& dma = allocators::dma_allocator::get();
        memblock = dma.allocate(size, alignment, boundary);
    });

    if (!memblock) {
        xhci_error("======= MEMORY ALLOCATION FAILED =======\n");
        while (true);
    }

    zeromem(memblock, size);
    return memblock;
}

void free_xhci_memory(void* ptr) {
    RUN_ELEVATED({
        auto& dma = allocators::dma_allocator::get();
        dma.free(ptr);
    });
}

uintptr_t xhci_get_physical_addr(void* vaddr) {
    uintptr_t paddr;
    RUN_ELEVATED({
        paddr = paging::get_physical_address(vaddr);
    });
    return paddr;
}
