#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/va_layout.h"
#include "mm/kva.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "mm/paging.h"
#include "mm/shmem.h"
#include "common/string.h"
#include "common/logging.h"

namespace mm {

    /**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
void mm_context::ref_destroy(mm_context* self) {
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
__PRIVILEGED_CODE int32_t init() {
    if (pmm::init() != pmm::OK) {
        log::error("mm: pmm init failed");
        return ERR;
    }

    if (init_va_layout() != VA_LAYOUT_OK) {
        log::error("mm: va_layout init failed");
        return ERR;
    }

    if (kva::init() != kva::OK) {
        log::error("mm: kva init failed");
        return ERR;
    }

    if (vmm::init() != vmm::OK) {
        log::error("mm: vmm init failed");
        return ERR;
    }

    if (heap::init() != heap::OK) {
        log::error("mm: heap init failed");
        return ERR;
    }

    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
bool handle_user_pf(
    mm_context* mm_ctx,
    uintptr_t fault_address,
    uint64_t pf_flags
) {
    if (!mm_ctx) {
        return false;
    }

    sync::mutex_lock(mm_ctx->lock);
    bool resolved = handle_user_pf_locked(mm_ctx, fault_address, pf_flags);
    sync::mutex_unlock(mm_ctx->lock);
    return resolved;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE
bool handle_user_pf_locked(
    mm_context* mm_ctx,
    uintptr_t fault_address,
    uint64_t pf_flags
) {
    // Protection violations on present pages are never recoverable here
    if (pf_flags & PF_FLAG_PRESENT) {
        return false;
    }

    uintptr_t page_addr = fault_address & ~(pmm::PAGE_SIZE - 1);

    // Get the virtual memory area for this page
    vma* vm = vma_find_locked(mm_ctx, page_addr);

    // Demand paging serves anonymous memory, including stacks. Regions
    // with no access rights are pure reservations and never fault in.
    if (!vm || !(vm->flags & (VMA_FLAG_ANONYMOUS | VMA_FLAG_STACK))) {
        return false;
    }
    if (vm->prot == 0) {
        return false;
    }

    // Reject access types the region forbids before committing a page
    if ((pf_flags & PF_FLAG_WRITE) && !(vm->prot & MM_PROT_WRITE)) {
        return false;
    }
    if ((pf_flags & PF_FLAG_INSTRUCTION) && !(vm->prot & MM_PROT_EXEC)) {
        return false;
    }

    // Concurrent page fault on same page won by another CPU, retry
    if (paging::get_physical(page_addr, mm_ctx->pt_root) != 0) {
        return true;
    }

    // Allocate a new physical page to back the memory
    pmm::phys_addr_t phys = pmm::alloc_page();
    if (phys == 0) {
        return false; // OOM - my favorite thing
    }

    // Zero out the memory
    string::memset(paging::phys_to_virt(phys), 0, pmm::PAGE_SIZE);

    // Setup the PTE with appropriate protection bits
    paging::page_flags_t pagefl = prot_to_page_flags(vm->prot);
    if (paging::map_page(page_addr, phys, pagefl, mm_ctx->pt_root) != paging::OK) {
        pmm::free_page(phys);
        return false;
    }

    return true;
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
        (USER_STACK_MAX_PAGES + USER_STACK_GUARD_PAGES) * pmm::PAGE_SIZE;
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

    // Don't eagerly allocate pages lazy pages that will get populated through on-demand faults
    if (!(map_flags & MM_MAP_LAZY)) {
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
