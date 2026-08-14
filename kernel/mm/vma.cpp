#include "mm/vma.h"

#include "common/string.h"
#include "mm/mm.h"
#include "mm/heap.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/shmem.h"

namespace mm {

inline bool ranges_overlap(uintptr_t a_start, uintptr_t a_end,
                           uintptr_t b_start, uintptr_t b_end) {
    return a_start < b_end && b_start < a_end;
}

inline bool vma_can_merge(const vma& left, const vma& right) {
    if (left.end != right.start || left.prot != right.prot ||
        left.flags != right.flags) {
        return false;
    }
    if ((left.flags & VMA_FLAG_SHARED) || (right.flags & VMA_FLAG_SHARED)) {
        if (left.shmem_backing.ptr() != right.shmem_backing.ptr()) {
            return false;
        }
        if (left.backing_offset + (left.end - left.start) != right.backing_offset) {
            return false;
        }
    }
    return true;
}

paging::page_flags_t prot_to_page_flags(uint32_t prot) {
    paging::page_flags_t flags = 0;
    if (prot != 0) {
        flags |= paging::PAGE_USER;
    }
    if (prot & MM_PROT_READ) {
        flags |= paging::PAGE_READ;
    }
    if (prot & MM_PROT_WRITE) {
        flags |= paging::PAGE_WRITE;
    }
    if (prot & MM_PROT_EXEC) {
        flags |= paging::PAGE_EXEC;
    }
    return flags;
}

__PRIVILEGED_CODE vma* alloc_vma(uintptr_t start, uintptr_t end, uint32_t prot, uint32_t flags) {
    vma* node = heap::kalloc_new<vma>();
    if (!node) {
        return nullptr;
    }

    node->start = start;
    node->end = end;
    node->prot = prot;
    node->flags = flags;
    node->addr_link = {};
    node->backing_offset = 0;
    return node;
}

__PRIVILEGED_CODE void free_vma(vma* node) {
    if (node) {
        heap::kfree_delete(node);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE vma* vma_find_locked(mm_context* mm_ctx, uintptr_t addr) {
    vma probe{};
    probe.start = addr;
    probe.end = addr;
    probe.prot = 0;
    probe.flags = 0;

    vma* lb = mm_ctx->vmas.lower_bound(probe);
    if (lb && lb->start == addr) {
        return lb;
    }

    vma* pred = lb ? mm_ctx->vmas.prev(*lb) : mm_ctx->vmas.max();
    if (pred && pred->start <= addr && addr < pred->end) {
        return pred;
    }

    if (lb && lb->start <= addr && addr < lb->end) {
        return lb;
    }

    return nullptr;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE vma* vma_find_overlap_locked(
    mm_context* mm_ctx, uintptr_t start, uintptr_t end
) {
    if (start >= end) {
        return nullptr;
    }

    vma probe{};
    probe.start = start;
    probe.end = start;
    probe.prot = 0;
    probe.flags = 0;

    vma* lb = mm_ctx->vmas.lower_bound(probe);
    vma* pred = lb ? mm_ctx->vmas.prev(*lb) : mm_ctx->vmas.max();

    if (pred && ranges_overlap(pred->start, pred->end, start, end)) {
        return pred;
    }
    if (lb && ranges_overlap(lb->start, lb->end, start, end)) {
        return lb;
    }

    return nullptr;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool vma_insert_locked(mm_context* mm_ctx, vma* node) {
    if (!node || node->start >= node->end) {
        return false;
    }
    if (!is_page_aligned(node->start) || !is_page_aligned(node->end)) {
        return false;
    }

    vma probe{};
    probe.start = node->start;
    probe.end = node->start;
    probe.prot = 0;
    probe.flags = 0;

    vma* lb = mm_ctx->vmas.lower_bound(probe);
    vma* pred = lb ? mm_ctx->vmas.prev(*lb) : mm_ctx->vmas.max();

    if (pred && pred->end > node->start) {
        return false;
    }
    if (lb && lb->start < node->end) {
        return false;
    }

    return mm_ctx->vmas.insert(node);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void vma_remove_locked(mm_context* mm_ctx, vma& node) {
    mm_ctx->vmas.remove(node);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uintptr_t vma_find_gap_topdown_locked(mm_context* mm_ctx, size_t length) {
    if (length == 0 || !is_page_aligned(length)) {
        return 0;
    }

    uintptr_t cursor = mm_ctx->mmap_end;
    for (vma* node = mm_ctx->vmas.max(); node; node = mm_ctx->vmas.prev(*node)) {
        if (node->end <= mm_ctx->mmap_base || node->start >= mm_ctx->mmap_end) {
            continue;
        }

        uintptr_t clipped_start = node->start;
        uintptr_t clipped_end = node->end;
        if (clipped_start < mm_ctx->mmap_base) {
            clipped_start = mm_ctx->mmap_base;
        }
        if (clipped_end > mm_ctx->mmap_end) {
            clipped_end = mm_ctx->mmap_end;
        }

        if (cursor > clipped_end && (cursor - clipped_end) >= length) {
            return cursor - length;
        }

        if (clipped_start < cursor) {
            cursor = clipped_start;
        }
    }

    if (cursor > mm_ctx->mmap_base && (cursor - mm_ctx->mmap_base) >= length) {
        return cursor - length;
    }

    return 0;
}

__PRIVILEGED_CODE void unmap_and_free_pages(mm_context* mm_ctx, uintptr_t start, uintptr_t end) {
    for (uintptr_t vaddr = start; vaddr < end; vaddr += pmm::PAGE_SIZE) {
        if (!paging::is_mapped(vaddr, mm_ctx->pt_root)) {
            continue;
        }

        pmm::phys_addr_t phys = paging::get_physical(vaddr, mm_ctx->pt_root);
        paging::unmap_page(vaddr, mm_ctx->pt_root);
        if (phys != 0) {
            pmm::free_page(phys);
        }
    }
}

__PRIVILEGED_CODE void unmap_pages_only(mm_context* mm_ctx, uintptr_t start, uintptr_t end) {
    for (uintptr_t vaddr = start; vaddr < end; vaddr += pmm::PAGE_SIZE) {
        if (!paging::is_mapped(vaddr, mm_ctx->pt_root)) {
            continue;
        }
        paging::unmap_page(vaddr, mm_ctx->pt_root);
    }
}

__PRIVILEGED_CODE void rollback_new_pages(mm_context* mm_ctx, uintptr_t start, uintptr_t mapped_end) {
    unmap_and_free_pages(mm_ctx, start, mapped_end);
}

__PRIVILEGED_CODE void coalesce_all_locked(mm_context* mm_ctx) {
    vma* cur = mm_ctx->vmas.min();
    while (cur) {
        vma* next = mm_ctx->vmas.next(*cur);
        if (next && vma_can_merge(*cur, *next)) {
            cur->end = next->end;
            mm_ctx->vmas.remove(*next);
            free_vma(next);
            continue;
        }
        cur = next;
    }
}

__PRIVILEGED_CODE vma* split_vma_locked(mm_context* mm_ctx, vma* node, uintptr_t split_addr) {
    if (!node) {
        return nullptr;
    }
    if (split_addr <= node->start || split_addr >= node->end) {
        return nullptr;
    }

    vma* right = alloc_vma(split_addr, node->end, node->prot, node->flags);
    if (!right) {
        return nullptr;
    }

    right->shmem_backing = node->shmem_backing;
    right->backing_offset = node->backing_offset + (split_addr - node->start);

    uintptr_t old_end = node->end;
    node->end = split_addr;
    if (!vma_insert_locked(mm_ctx, right)) {
        node->end = old_end;
        free_vma(right);
        return nullptr;
    }

    return right;
}

__PRIVILEGED_CODE int32_t unmap_range_locked(mm_context* mm_ctx, uintptr_t start, uintptr_t end) {
    for (;;) {
        vma* overlap = vma_find_overlap_locked(mm_ctx, start, end);
        if (!overlap || overlap->start >= end) {
            break;
        }

        if (start > overlap->start) {
            overlap = split_vma_locked(mm_ctx, overlap, start);
            if (!overlap) {
                return MM_CTX_ERR_NO_MEM;
            }
        }

        if (end < overlap->end) {
            if (!split_vma_locked(mm_ctx, overlap, end)) {
                return MM_CTX_ERR_NO_MEM;
            }
        }

        if (overlap->flags & (VMA_FLAG_SHARED | VMA_FLAG_DEVICE)) {
            unmap_pages_only(mm_ctx, overlap->start, overlap->end);
        } else {
            unmap_and_free_pages(mm_ctx, overlap->start, overlap->end);
        }
        mm_ctx->vmas.remove(*overlap);
        free_vma(overlap);
    }

    coalesce_all_locked(mm_ctx);
    return MM_CTX_OK;
}

bool range_fully_mapped_locked(mm_context* mm_ctx, uintptr_t start, uintptr_t end) {
    uintptr_t cur = start;
    while (cur < end) {
        vma* node = vma_find_locked(mm_ctx, cur);
        if (!node || node->start > cur) {
            return false;
        }
        cur = (node->end < end) ? node->end : end;
    }
    return true;
}

__PRIVILEGED_CODE int32_t apply_page_protection(
    mm_context* mm_ctx, uintptr_t start, uintptr_t end, uint32_t prot
) {
    paging::page_flags_t page_flags = prot_to_page_flags(prot);
    for (uintptr_t vaddr = start; vaddr < end; vaddr += pmm::PAGE_SIZE) {
        // Absent pages take the VMA's protection when they fault in
        if (!paging::is_mapped(vaddr, mm_ctx->pt_root)) {
            continue;
        }
        if (paging::set_page_flags(vaddr, page_flags, mm_ctx->pt_root) != paging::OK) {
            return MM_CTX_ERR_MAP_FAILED;
        }
    }
    return MM_CTX_OK;
}

} // namespace mm
