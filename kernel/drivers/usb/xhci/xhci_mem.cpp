#include "drivers/usb/xhci/xhci_mem.h"
#include "dma/dma.h"
#include "mm/paging.h"
#include "mm/pmm_types.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"

namespace drivers::xhci {

void* alloc_xhci_memory(size_t size) {
    if (size == 0) {
        log::error("xhci_mem: alloc with size 0");
        return nullptr;
    }

    size_t pages = (size + paging::PAGE_SIZE_4KB - 1) / paging::PAGE_SIZE_4KB;

    dma::buffer buf = {};
    int32_t rc = 0;
    RUN_ELEVATED({
        rc = dma::alloc_pages(pages, buf, pmm::ZONE_ANY, paging::PAGE_USER);
    });

    if (rc != dma::OK) {
        log::error("xhci_mem: DMA alloc failed (%lu bytes, %lu pages): %d",
                   size, pages, rc);
        return nullptr;
    }

    return reinterpret_cast<void*>(buf.virt);
}

void free_xhci_memory(void* ptr) {
    if (!ptr) {
        return;
    }

    dma::buffer buf = { reinterpret_cast<uintptr_t>(ptr), 0, 0 };
    RUN_ELEVATED({
        dma::free_pages(buf);
    });
}

uintptr_t xhci_get_physical_addr(void* vaddr) {
    uintptr_t paddr = 0;
    RUN_ELEVATED({
        paddr = paging::get_physical(
            reinterpret_cast<uintptr_t>(vaddr),
            paging::get_kernel_pt_root());
    });
    return paddr;
}

} // namespace drivers::xhci
