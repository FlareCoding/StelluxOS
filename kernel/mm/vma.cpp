#include "mm/vma.h"

#include "common/string.h"
#include "mm/heap.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/shmem.h"

namespace mm {

namespace {

constexpr uint32_t MM_MAP_ALLOWED_FLAGS =
    MM_MAP_SHARED | MM_MAP_PRIVATE | MM_MAP_ANONYMOUS | MM_MAP_FIXED |
    MM_MAP_FIXED_NOREPLACE | MM_MAP_STACK;

inline bool is_page_aligned(uintptr_t value) {
    return (value & (pmm::PAGE_SIZE - 1)) == 0;
}

inline bool range_from_len(uintptr_t start, size_t length, uintptr_t& end_out) {
    if (length == 0) {
        return false;
    }

    uintptr_t end = start + length;
    if (end < start) {
        return false;
    }

    end_out = end;
    return true;
}

inline bool ranges_overlap(uintptr_t a_start, uintptr_t a_end,
                           uintptr_t b_start, uintptr_t b_end) {
    return a_start < b_end && b_start < a_end;
}

inline paging::page_flags_t prot_to_page_flags(uint32_t prot) {
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

vma* alloc_vma(uintptr_t start, uintptr_t end, uint32_t prot, uint32_t flags) {
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

void free_vma(vma* node) {
    if (node) {
        heap::kfree_delete(node);
    }
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

void coalesce_all_locked(mm_context* mm_ctx) {
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

vma* split_vma_locked(mm_context* mm_ctx, vma* node, uintptr_t split_addr) {
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
        if (!paging::is_mapped(vaddr, mm_ctx->pt_root)) {
            return MM_CTX_ERR_NOT_MAPPED;
        }
        if (paging::set_page_flags(vaddr, page_flags, mm_ctx->pt_root) != paging::OK) {
            return MM_CTX_ERR_MAP_FAILED;
        }
    }
    return MM_CTX_OK;
}

} // namespace

__PRIVILEGED_CODE void mm_context::ref_destroy(mm_context* self) {
    if (!self) {
        return;
    }

    sync::mutex_lock(self->lock);
    while (vma* node = self->vmas.min()) {
        if (node->flags & (VMA_FLAG_SHARED | VMA_FLAG_DEVICE)) {
            unmap_pages_only(self, node->start, node->end);
        } else {
            unmap_and_free_pages(self, node->start, node->end);
        }
        self->vmas.remove(*node);
        free_vma(node);
    }
    sync::mutex_unlock(self->lock);

    paging::destroy_user_pt_root(self->pt_root);
    self->pt_root = 0;
    heap::kfree_delete(self);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE mm_context* mm_context_create() {
    mm_context* mm_ctx = heap::kalloc_new<mm_context>();
    if (!mm_ctx) {
        return nullptr;
    }

    mm_ctx->pt_root = paging::create_user_pt_root();
    if (mm_ctx->pt_root == 0) {
        heap::kfree_delete(mm_ctx);
        return nullptr;
    }

    mm_ctx->mmap_base = MMAP_BASE_DEFAULT;
    mm_ctx->mmap_end = USER_STACK_TOP -
        (USER_STACK_PAGES + USER_STACK_GUARD_PAGES) * pmm::PAGE_SIZE;
    if (mm_ctx->mmap_end <= mm_ctx->mmap_base) {
        paging::destroy_user_pt_root(mm_ctx->pt_root);
        heap::kfree_delete(mm_ctx);
        return nullptr;
    }

    mm_ctx->lock.init();
    return mm_ctx;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mm_context_add_ref(mm_context* mm_ctx) {
    if (mm_ctx) {
        mm_ctx->add_ref();
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mm_context_release(mm_context* mm_ctx) {
    if (!mm_ctx) {
        return;
    }

    if (mm_ctx->release()) {
        mm_context::ref_destroy(mm_ctx);
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

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t mm_context_add_vma(
    mm_context* mm_ctx,
    uintptr_t start,
    size_t length,
    uint32_t prot,
    uint32_t vma_flags
) {
    if (!mm_ctx || (prot & ~MM_PROT_MASK) != 0) {
        return MM_CTX_ERR_INVALID_ARG;
    }
    if (!is_page_aligned(start)) {
        return MM_CTX_ERR_INVALID_ARG;
    }

    size_t aligned_len = pmm::page_align_up(length);
    uintptr_t end = 0;
    if (!range_from_len(start, aligned_len, end)) {
        return MM_CTX_ERR_INVALID_ARG;
    }

    sync::mutex_lock(mm_ctx->lock);

    vma* node = alloc_vma(start, end, prot, vma_flags);
    if (!node) {
        sync::mutex_unlock(mm_ctx->lock);
        return MM_CTX_ERR_NO_MEM;
    }

    if (!vma_insert_locked(mm_ctx, node)) {
        free_vma(node);
        sync::mutex_unlock(mm_ctx->lock);
        return MM_CTX_ERR_EXISTS;
    }

    coalesce_all_locked(mm_ctx);
    sync::mutex_unlock(mm_ctx->lock);
    return MM_CTX_OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t mm_context_map_anonymous(
    mm_context* mm_ctx,
    uintptr_t addr,
    size_t length,
    uint32_t prot,
    uint32_t map_flags,
    uintptr_t* out_addr
) {
    if (!mm_ctx || !out_addr) {
        return MM_CTX_ERR_INVALID_ARG;
    }
    if ((prot & ~MM_PROT_MASK) != 0) {
        return MM_CTX_ERR_INVALID_ARG;
    }
    if ((map_flags & ~MM_MAP_ALLOWED_FLAGS) != 0) {
        return MM_CTX_ERR_INVALID_ARG;
    }
    if (!(map_flags & MM_MAP_PRIVATE) || !(map_flags & MM_MAP_ANONYMOUS)) {
        return MM_CTX_ERR_INVALID_ARG;
    }

    size_t aligned_len = pmm::page_align_up(length);
    if (aligned_len == 0) {
        return MM_CTX_ERR_INVALID_ARG;
    }

    const bool fixed = (map_flags & (MM_MAP_FIXED | MM_MAP_FIXED_NOREPLACE)) != 0;
    const bool no_replace = (map_flags & MM_MAP_FIXED_NOREPLACE) != 0;
    const bool stack_map = (map_flags & MM_MAP_STACK) != 0;

    uintptr_t start = 0;
    uintptr_t end = 0;

    if (fixed) {
        if (!is_page_aligned(addr)) {
            return MM_CTX_ERR_INVALID_ARG;
        }
        start = addr;
        if (!range_from_len(start, aligned_len, end)) {
            return MM_CTX_ERR_INVALID_ARG;
        }

        if (!stack_map && (start < mm_ctx->mmap_base || end > mm_ctx->mmap_end)) {
            return MM_CTX_ERR_NO_VIRT;
        }
    }

    sync::mutex_lock(mm_ctx->lock);

    if (fixed) {
        if (no_replace && vma_find_overlap_locked(mm_ctx, start, end)) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_EXISTS;
        }

        if (!no_replace) {
            int32_t rc = unmap_range_locked(mm_ctx, start, end);
            if (rc != MM_CTX_OK) {
                sync::mutex_unlock(mm_ctx->lock);
                return rc;
            }
        }
    } else {
        start = vma_find_gap_topdown_locked(mm_ctx, aligned_len);
        if (start == 0) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_NO_VIRT;
        }
        end = start + aligned_len;
    }

    paging::page_flags_t page_flags = prot_to_page_flags(prot);
    uintptr_t mapped_end = start;
    for (uintptr_t vaddr = start; vaddr < end; vaddr += pmm::PAGE_SIZE) {
        pmm::phys_addr_t phys = pmm::alloc_page();
        if (phys == 0) {
            rollback_new_pages(mm_ctx, start, mapped_end);
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_NO_MEM;
        }

        string::memset(paging::phys_to_virt(phys), 0, pmm::PAGE_SIZE);
        if (paging::map_page(vaddr, phys, page_flags, mm_ctx->pt_root) != paging::OK) {
            pmm::free_page(phys);
            rollback_new_pages(mm_ctx, start, mapped_end);
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_MAP_FAILED;
        }
        mapped_end = vaddr + pmm::PAGE_SIZE;
    }

    uint32_t vma_flags = VMA_FLAG_PRIVATE | VMA_FLAG_ANONYMOUS;
    if (stack_map) {
        vma_flags |= VMA_FLAG_STACK;
    }

    vma* node = alloc_vma(start, end, prot, vma_flags);
    if (!node) {
        rollback_new_pages(mm_ctx, start, end);
        sync::mutex_unlock(mm_ctx->lock);
        return MM_CTX_ERR_NO_MEM;
    }

    if (!vma_insert_locked(mm_ctx, node)) {
        free_vma(node);
        rollback_new_pages(mm_ctx, start, end);
        sync::mutex_unlock(mm_ctx->lock);
        return MM_CTX_ERR_EXISTS;
    }

    coalesce_all_locked(mm_ctx);
    sync::mutex_unlock(mm_ctx->lock);

    *out_addr = start;
    return MM_CTX_OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t mm_context_unmap(
    mm_context* mm_ctx,
    uintptr_t addr,
    size_t length
) {
    if (!mm_ctx || !is_page_aligned(addr) || length == 0) {
        return MM_CTX_ERR_INVALID_ARG;
    }

    size_t aligned_len = pmm::page_align_up(length);
    uintptr_t end = 0;
    if (!range_from_len(addr, aligned_len, end)) {
        return MM_CTX_ERR_INVALID_ARG;
    }

    sync::mutex_lock(mm_ctx->lock);
    int32_t rc = unmap_range_locked(mm_ctx, addr, end);
    sync::mutex_unlock(mm_ctx->lock);
    return rc;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t mm_context_mprotect(
    mm_context* mm_ctx,
    uintptr_t addr,
    size_t length,
    uint32_t prot
) {
    if (!mm_ctx || !is_page_aligned(addr) || length == 0) {
        return MM_CTX_ERR_INVALID_ARG;
    }
    if ((prot & ~MM_PROT_MASK) != 0) {
        return MM_CTX_ERR_INVALID_ARG;
    }

    size_t aligned_len = pmm::page_align_up(length);
    uintptr_t end = 0;
    if (!range_from_len(addr, aligned_len, end)) {
        return MM_CTX_ERR_INVALID_ARG;
    }

    sync::mutex_lock(mm_ctx->lock);

    if (!range_fully_mapped_locked(mm_ctx, addr, end)) {
        sync::mutex_unlock(mm_ctx->lock);
        return MM_CTX_ERR_NOT_MAPPED;
    }

    vma* at_start = vma_find_locked(mm_ctx, addr);
    if (at_start && at_start->start < addr && addr < at_start->end) {
        if (!split_vma_locked(mm_ctx, at_start, addr)) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_NO_MEM;
        }
    }

    vma* at_end = vma_find_locked(mm_ctx, end - 1);
    if (at_end && at_end->start < end && end < at_end->end) {
        if (!split_vma_locked(mm_ctx, at_end, end)) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_NO_MEM;
        }
    }

    vma probe{};
    probe.start = addr;
    probe.end = addr;
    probe.prot = 0;
    probe.flags = 0;

    vma* cur = mm_ctx->vmas.lower_bound(probe);
    vma* pred = cur ? mm_ctx->vmas.prev(*cur) : mm_ctx->vmas.max();
    if (pred && pred->end > addr) {
        cur = pred;
    }

    while (cur && cur->start < end) {
        vma* next = mm_ctx->vmas.next(*cur);
        uintptr_t range_start = (cur->start > addr) ? cur->start : addr;
        uintptr_t range_end = (cur->end < end) ? cur->end : end;

        int32_t rc = apply_page_protection(mm_ctx, range_start, range_end, prot);
        if (rc != MM_CTX_OK) {
            sync::mutex_unlock(mm_ctx->lock);
            return rc;
        }

        cur->prot = prot;
        cur = next;
    }

    coalesce_all_locked(mm_ctx);
    sync::mutex_unlock(mm_ctx->lock);
    return MM_CTX_OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t mm_context_map_shared(
    mm_context* mm_ctx,
    shmem* backing,
    uint64_t offset,
    size_t length,
    uint32_t prot,
    uint32_t map_flags,
    uintptr_t addr,
    uintptr_t* out_addr
) {
    if (!mm_ctx || !backing || !out_addr) {
        return MM_CTX_ERR_INVALID_ARG;
    }
    if ((prot & ~MM_PROT_MASK) != 0) {
        return MM_CTX_ERR_INVALID_ARG;
    }
    if (!(map_flags & MM_MAP_SHARED)) {
        return MM_CTX_ERR_INVALID_ARG;
    }

    size_t aligned_len = pmm::page_align_up(length);
    if (aligned_len == 0) {
        return MM_CTX_ERR_INVALID_ARG;
    }
    if (offset % pmm::PAGE_SIZE != 0) {
        return MM_CTX_ERR_INVALID_ARG;
    }

    const bool fixed = (map_flags & (MM_MAP_FIXED | MM_MAP_FIXED_NOREPLACE)) != 0;
    const bool no_replace = (map_flags & MM_MAP_FIXED_NOREPLACE) != 0;

    uintptr_t start = 0;
    uintptr_t end = 0;

    sync::mutex_lock(mm_ctx->lock);

    if (fixed) {
        if (!is_page_aligned(addr)) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_INVALID_ARG;
        }
        start = addr;
        if (!range_from_len(start, aligned_len, end)) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_INVALID_ARG;
        }
        if (start < mm_ctx->mmap_base || end > mm_ctx->mmap_end) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_NO_VIRT;
        }

        if (no_replace && vma_find_overlap_locked(mm_ctx, start, end)) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_EXISTS;
        }
        if (!no_replace) {
            int32_t rc = unmap_range_locked(mm_ctx, start, end);
            if (rc != MM_CTX_OK) {
                sync::mutex_unlock(mm_ctx->lock);
                return rc;
            }
        }
    } else {
        start = vma_find_gap_topdown_locked(mm_ctx, aligned_len);
        if (start == 0) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_NO_VIRT;
        }
        end = start + aligned_len;
    }

    sync::mutex_lock(backing->lock);

    size_t backed_size = backing->m_page_count * pmm::PAGE_SIZE;
    if (aligned_len > backed_size || offset > backed_size - aligned_len) {
        sync::mutex_unlock(backing->lock);
        sync::mutex_unlock(mm_ctx->lock);
        return MM_CTX_ERR_INVALID_ARG;
    }

    paging::page_flags_t page_flags = prot_to_page_flags(prot);
    size_t pages = aligned_len / pmm::PAGE_SIZE;
    size_t page_offset = static_cast<size_t>(offset / pmm::PAGE_SIZE);

    for (size_t i = 0; i < pages; i++) {
        pmm::phys_addr_t phys = shmem_get_page_locked(backing, page_offset + i);
        if (phys == 0) {
            unmap_pages_only(mm_ctx, start, start + i * pmm::PAGE_SIZE);
            sync::mutex_unlock(backing->lock);
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_NO_MEM;
        }

        uintptr_t vaddr = start + i * pmm::PAGE_SIZE;
        if (paging::map_page(vaddr, phys, page_flags, mm_ctx->pt_root) != paging::OK) {
            unmap_pages_only(mm_ctx, start, vaddr);
            sync::mutex_unlock(backing->lock);
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_MAP_FAILED;
        }
    }

    sync::mutex_unlock(backing->lock);

    vma* node = alloc_vma(start, end, prot, VMA_FLAG_SHARED);
    if (!node) {
        unmap_pages_only(mm_ctx, start, end);
        sync::mutex_unlock(mm_ctx->lock);
        return MM_CTX_ERR_NO_MEM;
    }

    backing->add_ref();
    node->shmem_backing = rc::strong_ref<shmem>::adopt(backing);
    node->backing_offset = offset;

    if (!vma_insert_locked(mm_ctx, node)) {
        unmap_pages_only(mm_ctx, start, end);
        free_vma(node);
        sync::mutex_unlock(mm_ctx->lock);
        return MM_CTX_ERR_EXISTS;
    }

    coalesce_all_locked(mm_ctx);
    sync::mutex_unlock(mm_ctx->lock);

    *out_addr = start;
    return MM_CTX_OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t mm_context_map_device(
    mm_context* mm_ctx,
    pmm::phys_addr_t phys_base,
    size_t length,
    uint32_t prot,
    uint32_t cache_type,
    uint32_t map_flags,
    uintptr_t addr,
    uintptr_t* out_addr
) {
    if (!mm_ctx || !out_addr) {
        return MM_CTX_ERR_INVALID_ARG;
    }
    if ((prot & ~MM_PROT_MASK) != 0) {
        return MM_CTX_ERR_INVALID_ARG;
    }
    if (!is_page_aligned(phys_base)) {
        return MM_CTX_ERR_INVALID_ARG;
    }

    size_t aligned_len = pmm::page_align_up(length);
    if (aligned_len == 0) {
        return MM_CTX_ERR_INVALID_ARG;
    }

    const bool fixed = (map_flags & (MM_MAP_FIXED | MM_MAP_FIXED_NOREPLACE)) != 0;
    const bool no_replace = (map_flags & MM_MAP_FIXED_NOREPLACE) != 0;

    uintptr_t start = 0;
    uintptr_t end = 0;

    sync::mutex_lock(mm_ctx->lock);

    if (fixed) {
        if (!is_page_aligned(addr)) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_INVALID_ARG;
        }
        start = addr;
        if (!range_from_len(start, aligned_len, end)) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_INVALID_ARG;
        }
        if (start < mm_ctx->mmap_base || end > mm_ctx->mmap_end) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_NO_VIRT;
        }

        if (no_replace && vma_find_overlap_locked(mm_ctx, start, end)) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_EXISTS;
        }
        if (!no_replace) {
            int32_t rc = unmap_range_locked(mm_ctx, start, end);
            if (rc != MM_CTX_OK) {
                sync::mutex_unlock(mm_ctx->lock);
                return rc;
            }
        }
    } else {
        start = vma_find_gap_topdown_locked(mm_ctx, aligned_len);
        if (start == 0) {
            sync::mutex_unlock(mm_ctx->lock);
            return MM_CTX_ERR_NO_VIRT;
        }
        end = start + aligned_len;
    }

    paging::page_flags_t page_flags = prot_to_page_flags(prot) | cache_type;
    size_t pages = aligned_len / pmm::PAGE_SIZE;

    if (paging::map_pages(start, phys_base, page_flags, pages, mm_ctx->pt_root) != paging::OK) {
        sync::mutex_unlock(mm_ctx->lock);
        return MM_CTX_ERR_MAP_FAILED;
    }

    vma* node = alloc_vma(start, end, prot, VMA_FLAG_DEVICE);
    if (!node) {
        unmap_pages_only(mm_ctx, start, end);
        sync::mutex_unlock(mm_ctx->lock);
        return MM_CTX_ERR_NO_MEM;
    }

    if (!vma_insert_locked(mm_ctx, node)) {
        unmap_pages_only(mm_ctx, start, end);
        free_vma(node);
        sync::mutex_unlock(mm_ctx->lock);
        return MM_CTX_ERR_EXISTS;
    }

    coalesce_all_locked(mm_ctx);
    sync::mutex_unlock(mm_ctx->lock);

    *out_addr = start;
    return MM_CTX_OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE size_t mm_context_vma_count(mm_context* mm_ctx) {
    if (!mm_ctx) {
        return 0;
    }

    sync::mutex_lock(mm_ctx->lock);
    size_t count = mm_ctx->vmas.size();
    sync::mutex_unlock(mm_ctx->lock);
    return count;
}

} // namespace mm
