#include <modules/usb/xhci/xhci_mem.h>
#include <memory/vmm.h>
#include <memory/allocators/dma_allocator.h>
#include <serial/serial.h>
#include <dynpriv/dynpriv.h>

__PRIVILEGED_CODE
uintptr_t xhci_map_mmio(uint64_t pci_bar_address, uint32_t bar_size) {
    const size_t page_count = bar_size / PAGE_SIZE;

    void* vbase = vmm::map_contiguous_physical_pages(
        pci_bar_address,
        page_count,
        DEFAULT_UNPRIV_PAGE_FLAGS | PTE_PCD
    );

    return reinterpret_cast<uintptr_t>(vbase);
}

void* alloc_xhci_memory(size_t size, size_t alignment, size_t boundary) {
    auto& dma = allocators::dma_allocator::get();

    void* memblock = nullptr;
    RUN_ELEVATED({
        memblock = dma.allocate(size, alignment, boundary);
    });

    if (!memblock) {
        serial::printf("[XHCI] ======= MEMORY ALLOCATION FAILED =======\n");
        while (true);
    }

    return memblock;
}

void free_xhci_memory(void* ptr) {
    auto& dma = allocators::dma_allocator::get();

    RUN_ELEVATED({
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
