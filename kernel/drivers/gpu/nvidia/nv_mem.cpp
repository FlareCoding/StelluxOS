#include "drivers/gpu/nvidia/nv_mem.h"
#include "mm/vmm.h"
#include "mm/kva.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"

namespace nvidia {

constexpr uint64_t DMA_ADDR_LIMIT = (1ULL << GSP_DMA_ADDR_WIDTH); // 47-bit window

int32_t dma_alloc(size_t bytes, bool uncached, dma_buffer& out) {
    out.cpu_va = 0;
    out.phys = 0;
    out.size = 0;
    if (bytes == 0) {
        return ERR_MEM_ALLOC;
    }

    const size_t pages = (bytes + paging::PAGE_SIZE_4KB - 1) / paging::PAGE_SIZE_4KB;
    // PAGE_USER: the GSP driver runs as an unprivileged (ring-3) pci_driver task, so its DMA buffers
    // must be user-accessible (matches xhci_mem.cpp's dma::alloc_pages(..., PAGE_USER)). The alloc
    // itself is privileged (RUN_ELEVATED below); the resulting mapping is user-readable/writable.
    const paging::page_flags_t flags =
        paging::PAGE_READ | paging::PAGE_WRITE | paging::PAGE_USER |
        (uncached ? paging::PAGE_DMA : paging::PAGE_NORMAL);

    uintptr_t        va = 0;
    pmm::phys_addr_t phys = 0;
    int32_t rc = vmm::ERR_NO_MEM;
    RUN_ELEVATED(rc = vmm::alloc_contiguous(pages, pmm::ZONE_ANY, flags, vmm::ALLOC_ZERO,
                                            kva::tag::generic, va, phys));
    if (rc != vmm::OK) {
        log::error("nvidia: mem: dma_alloc(%lu bytes / %lu pages) failed rc=%d",
                   static_cast<unsigned long>(bytes), static_cast<unsigned long>(pages), rc);
        return ERR_MEM_ALLOC;
    }

    if (static_cast<uint64_t>(phys) >= DMA_ADDR_LIMIT) {
        log::error("nvidia: mem: phys 0x%lx exceeds %u-bit DMA window",
                   static_cast<unsigned long>(phys), GSP_DMA_ADDR_WIDTH);
        RUN_ELEVATED(vmm::free(va));
        return ERR_MEM_DMA_RANGE;
    }

    out.cpu_va = va;
    out.phys = phys;
    out.size = bytes;
    return MEM_OK;
}

void dma_free(dma_buffer& buf) {
    if (buf.cpu_va) {
        RUN_ELEVATED(vmm::free(buf.cpu_va));
    }
    buf.cpu_va = 0;
    buf.phys = 0;
    buf.size = 0;
}

} // namespace nvidia
