// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/completion_notifier.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"

namespace drivers::nvidia {

static_assert(COMPLETION_NOTIFIER_BYTES == pmm::PAGE_SIZE, "the notifier is one page");

int32_t completion_notifier::allocate(const dma::device_profile& card) {
    dma::buffer page = {};
    int32_t rc = dma::OK;
    RUN_ELEVATED(rc = dma::alloc_pages(card, 1, dma::caching::cached_when_coherent, paging::PAGE_USER, page));
    if (rc != dma::OK) {
        log::error("nvidia: allocating the core channel's completion notifier failed: %d", rc);
        return ERR_NO_MEMORY;
    }

    m_page = page;

    return OK;
}

uint64_t completion_notifier::physical_address() const {
    return m_page.phys;
}

} // namespace drivers::nvidia
