#include "mm/page_quarantine.h"
#include "mm/kva.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "sched/sched.h"
#include "sched/task_exec_core.h"
#include "sync/atomic.h"
#include "hw/cpu.h"
#include "common/logging.h"

namespace page_quarantine {

// Bounds how long memory is held when nobody is waiting for it
constexpr uint64_t DRAIN_INTERVAL_MS = 10;

// A batch spanning more than this is cheaper to cover with a full flush
constexpr size_t RANGE_FLUSH_LIMIT = 32 * pmm::PAGE_SIZE;

__PRIVILEGED_DATA static pmm::phys_addr_t g_kernel_root = 0;
__PRIVILEGED_DATA static sync::atomic<size_t> g_held_pages{0};

// One drain at a time. The drainer never waits for a drain in progress, which
// may belong to a task that cannot run while the drainer spins, and which
// takes everything pending anyway.
__PRIVILEGED_DATA static sync::atomic<uint32_t> g_drain_busy{0};

// Span of the batch being drained, owned by the drain in progress
__PRIVILEGED_DATA static uintptr_t g_batch_lo = 0;
__PRIVILEGED_DATA static uintptr_t g_batch_hi = 0;

__PRIVILEGED_CODE static size_t mapping_size(uintptr_t virt) {
    paging::page_flags_t pf = paging::get_page_flags(virt, g_kernel_root);
    if (pf & paging::PAGE_HUGE_1GB) {
        return paging::PAGE_SIZE_1GB;
    }

    if (pf & paging::PAGE_LARGE_2MB) {
        return paging::PAGE_SIZE_2MB;
    }

    return paging::PAGE_SIZE_4KB;
}

// Invalidates every mapping of one range on this CPU, leaving the frames in
// the entries, and widens the batch span to cover it.
__PRIVILEGED_CODE static void retire_range(const kva::allocation& range) {
    uintptr_t pos = range.base;
    uintptr_t end = range.base + range.size;
    while (pos < end) {
        size_t step = mapping_size(pos);
        paging::unmap_page_keep_frame(pos, g_kernel_root);
        pos += step;
    }

    if (range.base < g_batch_lo) {
        g_batch_lo = range.base;
    }

    if (end > g_batch_hi) {
        g_batch_hi = end;
    }
}

// Returns the frames of one flushed range to the PMM. MMIO and caller-owned
// physical memory never belonged to the range.
__PRIVILEGED_CODE static void reclaim_range(const kva::allocation& range) {
    bool frees_frames = range.alloc_tag != kva::tag::mmio && range.alloc_tag != kva::tag::phys_map;
    pmm::phys_addr_t contiguous_base = 0;

    uintptr_t pos = range.base;
    uintptr_t end = range.base + range.size;
    while (pos < end) {
        pmm::phys_addr_t phys = 0;
        size_t mapped = paging::PAGE_SIZE_4KB;
        int32_t rc = paging::take_kept_frame(pos, g_kernel_root, &phys, &mapped);
        pos += mapped;

        if (rc != paging::OK || !frees_frames) {
            continue;
        }

        if (range.pmm_order == 0) {
            pmm::free_page(phys);
        } else if (contiguous_base == 0) {
            contiguous_base = phys;
        }
    }

    if (contiguous_base != 0) {
        pmm::free_pages(contiguous_base, range.pmm_order);
    }

    g_held_pages.fetch_sub_relaxed(range.size / pmm::PAGE_SIZE);
}

__PRIVILEGED_CODE static bool try_claim_drain() {
    uint32_t idle = 0;
    return g_drain_busy.cmpxchg_strong_acquire(idle, 1);
}

__PRIVILEGED_CODE static void release_drain() {
    g_drain_busy.store_release(0);
}

// The caller owns the drain. Every retired range is invalidated before the
// one system-wide flush, and nothing is returned before it. A retired page
// table may live on as a cached walk entry for any address it covered, so
// only a full flush clears the way for freeing it.
__PRIVILEGED_CODE static void flush_and_reclaim_retired() {
    kva::retired_batch batch = kva::take_retired();
    paging::retired_tables tables = paging::take_retired_tables();
    if (batch.empty() && tables.empty()) {
        return;
    }

    g_batch_lo = ~uintptr_t{0};
    g_batch_hi = 0;
    kva::for_each_retired(batch, retire_range);

    if (tables.empty() && g_batch_hi - g_batch_lo <= RANGE_FLUSH_LIMIT) {
        paging::flush_tlb_range(g_batch_lo, g_batch_hi);
    } else {
        paging::flush_tlb_all();
    }

    kva::for_each_retired(batch, reclaim_range);
    kva::release_retired(batch);
    paging::free_retired_tables(tables);

    // Reclaiming may have emptied tables of its own, they wait for one more flush
    tables = paging::take_retired_tables();
    if (!tables.empty()) {
        paging::flush_tlb_all();
        paging::free_retired_tables(tables);
    }
}

__PRIVILEGED_CODE static void drainer_main(void*) {
    while (true) {
        if (try_claim_drain()) {
            flush_and_reclaim_retired();
            release_drain();
        }

        sched::sleep_ms(DRAIN_INTERVAL_MS);
    }
}

__PRIVILEGED_CODE void init(pmm::phys_addr_t kernel_root) {
    g_kernel_root = kernel_root;
}

__PRIVILEGED_CODE int32_t start() {
    sched::task* drainer = sched::create_kernel_task(
        drainer_main, nullptr, "vmreclaimd", sched::TASK_FLAG_ELEVATED);
    if (!drainer) {
        return ERR_NO_MEM;
    }

    sched::enqueue(drainer);
    return OK;
}

__PRIVILEGED_CODE int32_t admit(uintptr_t addr) {
    kva::allocation range;
    if (kva::retire(addr, range) != kva::OK) {
        return ERR_NOT_FOUND;
    }

    size_t pages = range.size / pmm::PAGE_SIZE;
    size_t held = g_held_pages.fetch_add_relaxed(pages) + pages;
    if (held >= DRAIN_THRESHOLD_PAGES && cpu::irqs_enabled()) {
        drain();
    }

    return OK;
}

__PRIVILEGED_CODE void drain() {
    if (!cpu::irqs_enabled()) {
        log::fatal("page_quarantine: drained with interrupts disabled");
    }

    // Yielding lets a drain that was preempted on this CPU finish first
    while (!try_claim_drain()) {
        sched::yield();
    }

    flush_and_reclaim_retired();
    release_drain();
}

} // namespace page_quarantine
