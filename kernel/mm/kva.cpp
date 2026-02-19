/*
 * Kernel Virtual Address allocator.
 *
 * Manages the VA range between HHDM and kernel image. Tracks free/used
 * regions with three RB-trees (free-by-addr, free-by-size, used-by-addr).
 * Does not touch page tables — only bookkeeping.
 */

#include "mm/kva.h"
#include "mm/kva_internal.h"
#include "mm/va_layout.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "core/utils/logging.h"
#include "core/utils/memory.h"

namespace kva {

__PRIVILEGED_DATA static free_addr_tree g_free_by_addr;
__PRIVILEGED_DATA static free_size_tree g_free_by_size;
__PRIVILEGED_DATA static used_addr_tree g_used_by_addr;
__PRIVILEGED_DATA static node_pool g_pool;
__PRIVILEGED_DATA static bool g_initialized = false;

static inline uintptr_t align_up(uintptr_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

static inline uintptr_t align_down(uintptr_t val, size_t align) {
    return val & ~(align - 1);
}

static inline bool is_power_of_2(size_t val) {
    return val != 0 && (val & (val - 1)) == 0;
}

// Allocate a range_node from the pool. Returns nullptr on OOM.
__PRIVILEGED_CODE static range_node* pool_alloc() {
    if (g_pool.freelist) {
        range_node* n = g_pool.freelist;
        g_pool.freelist = n->pool_next;
        memory::memset(n, 0, sizeof(range_node));
        return n;
    }

    pmm::phys_addr_t phys = pmm::alloc_page();
    if (phys == 0) return nullptr;

    auto* page = static_cast<range_node*>(paging::phys_to_virt(phys));
    size_t count = PAGE_SIZE / sizeof(range_node);

    for (size_t i = 0; i < count; i++) {
        memory::memset(&page[i], 0, sizeof(range_node));
        page[i].pool_next = (i + 1 < count) ? &page[i + 1] : nullptr;
    }

    g_pool.freelist = page[1].pool_next ? &page[1] : nullptr;
    g_pool.pages_allocated++;

    memory::memset(&page[0], 0, sizeof(range_node));
    return &page[0];
}

// Return a range_node to the pool freelist.
__PRIVILEGED_CODE static void pool_free(range_node* n) {
    memory::memset(n, 0, sizeof(range_node));
    n->pool_next = g_pool.freelist;
    g_pool.freelist = n;
}

// Insert a free range into both free trees.
__PRIVILEGED_CODE static void insert_into_free_trees(range_node* n) {
    n->is_free = true;
    n->usable_base = n->start;
    n->guard_pre = 0;
    n->guard_post = 0;
    n->alloc_tag = tag::generic;
    n->pmm_order = 0;
    (void)g_free_by_addr.insert(n);
    (void)g_free_by_size.insert(n);
}

// Remove a free range from both free trees.
__PRIVILEGED_CODE static void remove_from_free_trees(range_node* n) {
    g_free_by_size.remove(*n);
    g_free_by_addr.remove(*n);
}

// Populate an allocation struct from a used range_node.
__PRIVILEGED_CODE static void populate_allocation(const range_node* n, allocation& out) {
    out.base = n->usable_base;
    out.size = usable_size(*n);
    out.reserved_base = n->start;
    out.reserved_size = n->end - n->start;
    out.guard_pre = n->guard_pre;
    out.guard_post = n->guard_post;
    out.alloc_tag = n->alloc_tag;
    out.pmm_order = n->pmm_order;
}

// Find the free range in free_by_addr that contains [base, base+size).
// Returns nullptr if no single free range contains the requested span.
__PRIVILEGED_CODE static range_node* find_containing_free(uintptr_t base, size_t size) {
    range_node probe{};
    probe.start = base;

    range_node* lb = g_free_by_addr.lower_bound(probe);

    // Check lb itself (handles case where base == free.start)
    if (lb && lb->start <= base && lb->end >= base + size) {
        return lb;
    }

    // Check predecessor
    range_node* pred = lb ? g_free_by_addr.prev(*lb) : g_free_by_addr.max();
    if (pred && pred->start <= base && pred->end >= base + size) {
        return pred;
    }

    return nullptr;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init() {
    const mm::va_layout& layout = mm::get_va_layout();

    if (layout.kva_base >= layout.kva_end) {
        return ERR_INVALID_ARG;
    }
    if ((layout.kva_base & (PAGE_SIZE - 1)) != 0 ||
        (layout.kva_end & (PAGE_SIZE - 1)) != 0) {
        return ERR_ALIGNMENT;
    }

    range_node* initial = pool_alloc();
    if (!initial) return ERR_NO_MEM;

    initial->start = layout.kva_base;
    initial->end = layout.kva_end;
    insert_into_free_trees(initial);

    g_initialized = true;
    return OK;
}

/**
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t alloc(
    size_t      size,
    size_t      align,
    uint16_t    guard_pre,
    uint16_t    guard_post,
    placement   place,
    tag         t,
    uint8_t     pmm_order,
    allocation& out
) {
    if (size == 0) return ERR_INVALID_ARG;
    if (!is_power_of_2(align) || align < PAGE_SIZE) return ERR_ALIGNMENT;
    if (pmm_order > pmm::MAX_ORDER) return ERR_INVALID_ARG;

    size_t usable = align_up(size, PAGE_SIZE);
    size_t guard_bytes = static_cast<size_t>(guard_pre + guard_post) * PAGE_SIZE;
    size_t reserved = usable + guard_bytes;

    // Search free_by_size for best fit
    range_node size_probe{};
    size_probe.start = 0;
    size_probe.end = reserved;

    range_node* candidate = g_free_by_size.lower_bound(size_probe);

    uintptr_t alloc_start = 0;
    uintptr_t alloc_end = 0;

    while (candidate) {
        if (place == placement::low) {
            alloc_start = align_up(candidate->start, align);
        } else {
            if (candidate->end < reserved) {
                candidate = g_free_by_size.next(*candidate);
                continue;
            }
            alloc_start = align_down(candidate->end - reserved, align);
        }
        alloc_end = alloc_start + reserved;

        if (alloc_start >= candidate->start && alloc_end <= candidate->end) {
            break; // fits
        }
        candidate = g_free_by_size.next(*candidate);
    }

    if (!candidate) return ERR_NO_VIRT;

    // Pre-allocate split nodes before modifying any trees
    bool need_prefix = (alloc_start > candidate->start);
    bool need_suffix = (alloc_end < candidate->end);
    range_node* prefix_node = nullptr;
    range_node* suffix_node = nullptr;

    if (need_prefix) {
        prefix_node = pool_alloc();
        if (!prefix_node) return ERR_NO_MEM;
    }
    if (need_suffix) {
        suffix_node = pool_alloc();
        if (!suffix_node) {
            if (prefix_node) pool_free(prefix_node);
            return ERR_NO_MEM;
        }
    }

    // Safe to modify trees now — all nodes are secured
    uintptr_t old_start = candidate->start;
    uintptr_t old_end = candidate->end;
    remove_from_free_trees(candidate);

    // Split prefix
    if (need_prefix) {
        prefix_node->start = old_start;
        prefix_node->end = alloc_start;
        insert_into_free_trees(prefix_node);
    }

    // Split suffix
    if (need_suffix) {
        suffix_node->start = alloc_end;
        suffix_node->end = old_end;
        insert_into_free_trees(suffix_node);
    }

    // Convert candidate to used
    candidate->start = alloc_start;
    candidate->end = alloc_end;
    candidate->usable_base = alloc_start + static_cast<uintptr_t>(guard_pre) * PAGE_SIZE;
    candidate->guard_pre = guard_pre;
    candidate->guard_post = guard_post;
    candidate->alloc_tag = t;
    candidate->pmm_order = pmm_order;
    candidate->is_free = false;
    (void)g_used_by_addr.insert(candidate);

    populate_allocation(candidate, out);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t free(uintptr_t base) {
    // Find the used range by usable_base
    range_node probe{};
    probe.usable_base = base;

    range_node* node = g_used_by_addr.find(probe);
    if (!node) return ERR_NOT_FOUND;

    // Remove from used tree
    g_used_by_addr.remove(*node);

    // Prepare as free range (clear allocation metadata)
    uintptr_t freed_start = node->start;
    uintptr_t freed_end = node->end;
    node->guard_pre = 0;
    node->guard_post = 0;
    node->alloc_tag = tag::generic;
    node->pmm_order = 0;
    node->is_free = true;
    node->usable_base = freed_start;
    node->start = freed_start;
    node->end = freed_end;

    // Coalesce with predecessor: find free range whose end == freed_start
    range_node addr_probe{};
    addr_probe.start = freed_start;
    range_node* lb = g_free_by_addr.lower_bound(addr_probe);
    range_node* pred = lb ? g_free_by_addr.prev(*lb) : g_free_by_addr.max();
    if (pred && pred->end == freed_start) {
        remove_from_free_trees(pred);
        node->start = pred->start;
        pool_free(pred);
    }

    // Coalesce with successor: find free range whose start == freed_end
    range_node succ_probe{};
    succ_probe.start = node->end;
    range_node* succ = g_free_by_addr.find(succ_probe);
    if (succ) {
        remove_from_free_trees(succ);
        node->end = succ->end;
        pool_free(succ);
    }

    // Update usable_base to match merged start
    node->usable_base = node->start;
    insert_into_free_trees(node);

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t reserve(uintptr_t base, size_t size, tag t) {
    if (size == 0) return ERR_INVALID_ARG;
    if ((base & (PAGE_SIZE - 1)) != 0) return ERR_ALIGNMENT;
    size = align_up(size, PAGE_SIZE);
    if (base + size < base) return ERR_INVALID_ARG; // overflow

    range_node* containing = find_containing_free(base, size);
    if (!containing) return ERR_NOT_FOUND;

    uintptr_t alloc_start = base;
    uintptr_t alloc_end = base + size;

    // Pre-allocate split nodes
    bool need_prefix = (alloc_start > containing->start);
    bool need_suffix = (alloc_end < containing->end);
    range_node* prefix_node = nullptr;
    range_node* suffix_node = nullptr;

    if (need_prefix) {
        prefix_node = pool_alloc();
        if (!prefix_node) {
            return ERR_NO_MEM;
        }
    }
    if (need_suffix) {
        suffix_node = pool_alloc();
        if (!suffix_node) {
            if (prefix_node) {
                pool_free(prefix_node);
            }
            return ERR_NO_MEM;
        }
    }

    uintptr_t old_start = containing->start;
    uintptr_t old_end = containing->end;
    remove_from_free_trees(containing);

    if (need_prefix) {
        prefix_node->start = old_start;
        prefix_node->end = alloc_start;
        insert_into_free_trees(prefix_node);
    }

    if (need_suffix) {
        suffix_node->start = alloc_end;
        suffix_node->end = old_end;
        insert_into_free_trees(suffix_node);
    }

    // Convert to used
    containing->start = alloc_start;
    containing->end = alloc_end;
    containing->usable_base = alloc_start;
    containing->guard_pre = 0;
    containing->guard_post = 0;
    containing->alloc_tag = t;
    containing->pmm_order = 0;
    containing->is_free = false;
    (void)g_used_by_addr.insert(containing);

    return OK;
}

/**
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t query(uintptr_t addr, allocation& out) {
    range_node probe{};
    probe.usable_base = addr;

    range_node* lb = g_used_by_addr.lower_bound(probe);

    // Check lb
    if (lb && lb->start <= addr && addr < lb->end) {
        populate_allocation(lb, out);
        return OK;
    }

    // Check predecessor
    range_node* pred = lb ? g_used_by_addr.prev(*lb) : g_used_by_addr.max();
    if (pred && pred->start <= addr && addr < pred->end) {
        populate_allocation(pred, out);
        return OK;
    }

    return ERR_NOT_FOUND;
}

__PRIVILEGED_CODE static const char* tag_name(tag t) {
    switch (t) {
        case tag::generic:            return "generic";
        case tag::privileged_heap:    return "privileged_heap";
        case tag::unprivileged_heap:  return "unprivileged_heap";
        case tag::privileged_stack:   return "privileged_stack";
        case tag::unprivileged_stack: return "unprivileged_stack";
        case tag::mmio:               return "mmio";
        case tag::boot:               return "boot";
        default:                      return "unknown";
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dump_state() {
    log::info("kva: state dump");

    size_t free_count = 0;
    size_t free_bytes = 0;
    for (auto& n : g_free_by_addr) {
        log::info("  free:  0x%016lx - 0x%016lx (%lu pages)",
                  n.start, n.end, (n.end - n.start) / PAGE_SIZE);
        free_count++;
        free_bytes += n.end - n.start;
    }

    size_t used_count = 0;
    size_t used_bytes = 0;
    for (auto& n : g_used_by_addr) {
        log::info("  used:  base=0x%016lx size=%lu pages tag=%s guards=%u/%u",
                  n.usable_base, usable_size(n) / PAGE_SIZE,
                  tag_name(n.alloc_tag), n.guard_pre, n.guard_post);
        used_count++;
        used_bytes += n.end - n.start;
    }

    log::info("  summary: %lu free ranges (%lu MB), %lu used ranges (%lu MB), pool pages=%lu",
              free_count, free_bytes / (1024 * 1024),
              used_count, used_bytes / (1024 * 1024),
              g_pool.pages_allocated);
}

} // namespace kva
