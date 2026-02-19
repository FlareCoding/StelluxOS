#ifndef STELLUX_MM_KVA_INTERNAL_H
#define STELLUX_MM_KVA_INTERNAL_H

#include "mm/kva.h"
#include "common/ds/rb_tree.h"
#include "mm/pmm_types.h"

namespace kva {

constexpr size_t PAGE_SIZE = pmm::PAGE_SIZE;

// Internal range node representing a free or used VA region.
// 96 bytes on 64-bit. When in the node pool freelist, pool_next is active
// and the node is not in any tree. Otherwise addr_link is active.
struct range_node {
    uintptr_t start;       // reserved base (inclusive)
    uintptr_t end;         // reserved end (exclusive)
    uintptr_t usable_base; // start + guard_pre * PAGE_SIZE
    uint16_t  guard_pre;
    uint16_t  guard_post;
    tag       alloc_tag;
    bool      is_free;
    uint8_t   pmm_order; // 0=non-contiguous/MMIO, 1-18=contiguous PMM order

    union {
        range_node* pool_next; // when in freelist
        rbt::node   addr_link; // when in free_by_addr or used_by_addr
    };

    rbt::node size_link; // when free, in free_by_size
};

inline size_t range_size(const range_node& n) {
    return n.end - n.start;
}

inline size_t usable_size(const range_node& n) {
    return (n.end - n.start) -
           static_cast<size_t>(n.guard_pre + n.guard_post) * PAGE_SIZE;
}

// Comparator for free_by_addr: ordered by reserved start address.
struct free_addr_cmp {
    bool operator()(const range_node& a, const range_node& b) const {
        return a.start < b.start;
    }
};

// Comparator for free_by_size: ordered by range size, tiebreak by start.
struct free_size_cmp {
    bool operator()(const range_node& a, const range_node& b) const {
        size_t as = a.end - a.start;
        size_t bs = b.end - b.start;
        if (as != bs) return as < bs;
        return a.start < b.start;
    }
};

// Comparator for used_by_addr: ordered by usable base address.
struct used_addr_cmp {
    bool operator()(const range_node& a, const range_node& b) const {
        return a.usable_base < b.usable_base;
    }
};

using free_addr_tree = rbt::tree<range_node, &range_node::addr_link, free_addr_cmp>;
using free_size_tree = rbt::tree<range_node, &range_node::size_link, free_size_cmp>;
using used_addr_tree = rbt::tree<range_node, &range_node::addr_link, used_addr_cmp>;

struct node_pool {
    range_node* freelist = nullptr;
    size_t      pages_allocated = 0;
};

} // namespace kva

#endif // STELLUX_MM_KVA_INTERNAL_H
