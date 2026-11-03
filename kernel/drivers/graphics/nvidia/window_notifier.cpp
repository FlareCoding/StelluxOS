// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/window_notifier.h"
#include "drivers/graphics/nvidia/completion_notifier.h"
#include "drivers/graphics/nvidia/latch_rules.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "hw/mmio.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"

namespace drivers::nvidia {

static_assert(WINDOW_NOTIFIER_BYTES == pmm::PAGE_SIZE, "the notifier is one page");
static_assert(NOTIFIER_SLOTS * WINDOW_NOTIFIER_SLOT_BYTES <= WINDOW_NOTIFIER_BYTES, "the slots fit the page");

int32_t window_notifier::allocate(const dma::device_profile& card) {
    dma::buffer page = {};
    int32_t rc = dma::OK;
    RUN_ELEVATED(rc = dma::alloc_pages(card, 1, dma::caching::cached_when_coherent, paging::PAGE_USER, page));
    if (rc != dma::OK) {
        log::error("nvidia: allocating the window notifier failed: %d", rc);
        return ERR_NO_MEMORY;
    }

    m_page = page;
    return OK;
}

uint64_t window_notifier::physical_address() const {
    return m_page.phys;
}

void window_notifier::clear(uint32_t slot) {
    mmio::write32(m_page.virt + slot * WINDOW_NOTIFIER_SLOT_BYTES, 0);
}

bool window_notifier::written(uint32_t slot) const {
    uint32_t status = mmio::read32(m_page.virt + slot * WINDOW_NOTIFIER_SLOT_BYTES) >> NOTIFIER_STATUS_SHIFT;
    return status == NOTIFIER_STATUS_BEGUN || status == NOTIFIER_STATUS_FINISHED;
}

} // namespace drivers::nvidia
