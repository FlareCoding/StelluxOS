// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/completion_notifier.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "hw/mmio.h"
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

void completion_notifier::clear() {
    mmio::write32(m_page.virt, 0);
}

uint32_t completion_notifier::status_word() const {
    return mmio::read32(m_page.virt);
}

int32_t completion_notifier::wait_finished(const display_channel& core, uint64_t deadline_ns) const {
    while (true) {
        uint32_t word = status_word();
        if (word >> NOTIFIER_STATUS_SHIFT == NOTIFIER_STATUS_FINISHED) {
            return OK;
        }

        if (clock::now_ns() <= deadline_ns) {
            continue;
        }

        if (core.idle()) {
            log::warn("nvidia: the core channel's completion notifier reads 0x%08X, a lost notification", word);
            return OK;
        }

        log::error("nvidia: the core channel's completion notifier did not read finished: 0x%08X", word);
        return ERR_TIMEOUT;
    }
}

} // namespace drivers::nvidia
