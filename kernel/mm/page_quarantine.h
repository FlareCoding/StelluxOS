#ifndef STELLUX_MM_PAGE_QUARANTINE_H
#define STELLUX_MM_PAGE_QUARANTINE_H

#include "common/types.h"
#include "mm/pmm_types.h"

namespace page_quarantine {

constexpr int32_t OK            = 0;
constexpr int32_t ERR_NO_MEM    = -1; // the drainer task could not be created
constexpr int32_t ERR_NOT_FOUND = -2; // not a live allocation, or freed already

// A freer that can wait drains once this much is held, so the footprint
// never depends on the drainer being scheduled
constexpr size_t DRAIN_THRESHOLD_PAGES = 2048;

/**
 * The page quarantine holds freed kernel memory until every CPU has dropped
 * its translations for it. Another CPU may keep translating a freed page
 * long after the freeing CPU flushed, so neither the address nor the frames
 * behind it may be reused before a system-wide TLB flush covers the range.
 * `admit`, called by `vmm::free`, retires the range in the KVA allocator,
 * whose per-allocation node is the only record: nothing is allocated and
 * nothing waits, so admission is safe from any context, including under
 * locks taken with interrupts disabled, and there is no capacity to exhaust.
 * A drain invalidates the mappings of every retired range while leaving each
 * frame recorded in its invalid page table entry, performs one system-wide
 * flush, and only then returns frames to the PMM and addresses to the KVA
 * allocator. Page tables emptied by any unmap are retired the same way and
 * freed by a drain after a full flush, so no CPU can walk a reused table.
 * The `vmreclaimd` task drains periodically and a freer that can wait drains
 * once the backlog grows past a threshold, which bounds how much memory is
 * held. Correctness never depends on either: memory left in quarantine is
 * merely unavailable until the next drain, and `vmm` drains on demand when
 * an allocation would otherwise fail.
 */

/**
 * @brief Prepare the quarantine. Call from `vmm::init`, before any free.
 * @param kernel_root Page table root that kernel mappings live in.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void init(pmm::phys_addr_t kernel_root);

/**
 * @brief Start `vmreclaimd`. Call once the scheduler runs. Memory admitted
 * earlier waits in the quarantine until the first pass.
 * @return OK on success, ERR_NO_MEM if the task cannot be created.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t start();

/**
 * @brief Retire the allocation containing `addr`. Its address and frames stay
 * out of circulation until a drain has flushed every CPU.
 * @param addr Any address within the allocation.
 * @return OK, or ERR_NOT_FOUND when `addr` is not a live allocation.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t admit(uintptr_t addr);

/**
 * @brief Flush every retired range on every CPU and return the ranges and
 * their frames to the allocators. On return, everything admitted before the
 * call is reusable. Requires interrupts enabled, like the system-wide flush
 * it performs.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void drain();

} // namespace page_quarantine

#endif // STELLUX_MM_PAGE_QUARANTINE_H
