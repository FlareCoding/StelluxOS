#ifndef STELLUX_MM_VMA_H
#define STELLUX_MM_VMA_H

#include "common/types.h"
#include "common/rb_tree.h"
#include "mm/paging.h"
#include "mm/pmm_types.h"
#include "mm/shmem.h"
#include "sync/mutex.h"
#include "rc/ref_counted.h"
#include "rc/strong_ref.h"

namespace mm {

constexpr int32_t MM_CTX_OK               = 0;
constexpr int32_t MM_CTX_ERR_INVALID_ARG  = -1;
constexpr int32_t MM_CTX_ERR_NO_MEM       = -2;
constexpr int32_t MM_CTX_ERR_NO_VIRT      = -3;
constexpr int32_t MM_CTX_ERR_EXISTS       = -4;
constexpr int32_t MM_CTX_ERR_MAP_FAILED   = -5;
constexpr int32_t MM_CTX_ERR_NOT_MAPPED   = -6;

constexpr uint32_t MM_PROT_READ    = (1u << 0);
constexpr uint32_t MM_PROT_WRITE   = (1u << 1);
constexpr uint32_t MM_PROT_EXEC    = (1u << 2);
constexpr uint32_t MM_PROT_MASK    = MM_PROT_READ | MM_PROT_WRITE | MM_PROT_EXEC;

constexpr uint32_t MM_MAP_SHARED           = 0x00000001u;
constexpr uint32_t MM_MAP_PRIVATE          = 0x00000002u;
constexpr uint32_t MM_MAP_FIXED            = 0x00000010u;
constexpr uint32_t MM_MAP_ANONYMOUS        = 0x00000020u;
constexpr uint32_t MM_MAP_STACK            = 0x00020000u;
constexpr uint32_t MM_MAP_FIXED_NOREPLACE  = 0x00100000u;
constexpr uint32_t MM_MAP_LAZY             = 0x00200000u;

constexpr uint32_t MM_MAP_ALLOWED_FLAGS =
    MM_MAP_SHARED | MM_MAP_PRIVATE | MM_MAP_ANONYMOUS | MM_MAP_FIXED |
    MM_MAP_FIXED_NOREPLACE | MM_MAP_STACK | MM_MAP_LAZY;

constexpr uint32_t VMA_FLAG_PRIVATE   = (1u << 0);
constexpr uint32_t VMA_FLAG_ANONYMOUS = (1u << 1);
constexpr uint32_t VMA_FLAG_ELF       = (1u << 2);
constexpr uint32_t VMA_FLAG_STACK     = (1u << 3);
constexpr uint32_t VMA_FLAG_SHARED    = (1u << 4);
constexpr uint32_t VMA_FLAG_DEVICE    = (1u << 5);

constexpr uintptr_t MMAP_BASE_DEFAULT = 0x00000080000000ULL;
constexpr uintptr_t USER_STACK_TOP    = 0x00007FFFFFF00000ULL;
constexpr size_t    USER_STACK_PAGES  = 8; // 32 KiB
constexpr size_t    USER_STACK_MAX_PAGES = 2048; // 8 MiB max stack space via lazy on-demand paging
constexpr size_t    USER_STACK_GUARD_PAGES = 1;

struct mm_context;

struct vma {
    uintptr_t start;
    uintptr_t end;
    uint32_t  prot;
    uint32_t  flags;
    rbt::node addr_link;
    rc::strong_ref<shmem> shmem_backing;
    uint64_t              backing_offset;
};

struct vma_addr_cmp {
    bool operator()(const vma& a, const vma& b) const {
        if (a.start != b.start) {
            return a.start < b.start;
        }
        return a.end < b.end;
    }
};

using vma_tree = rbt::tree<vma, &vma::addr_link, vma_addr_cmp>;

/**
 * @brief Check whether an address is aligned to the system page size.
 */
[[nodiscard]] inline bool is_page_aligned(uintptr_t value) {
    return (value & (pmm::PAGE_SIZE - 1)) == 0;
}

/**
 * @brief Compute the inclusive-end address of a [start, start+length) range.
 * Rejects zero length and any overflow.
 * @return true and writes end into end_out on success, false otherwise.
 */
[[nodiscard]] inline bool range_from_len(uintptr_t start, size_t length, uintptr_t& end_out) {
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

/**
 * @brief Convert MM_PROT_* protection bits into architecture-specific page-table flags.
 */
[[nodiscard]] paging::page_flags_t prot_to_page_flags(uint32_t prot);

/**
 * @brief Allocate a VMA node and initialize its fields.
 * Does not insert into any tree.
 * @return New VMA on success, nullptr on allocation failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE vma* alloc_vma(
    uintptr_t start, uintptr_t end, uint32_t prot, uint32_t flags);

/**
 * @brief Free a VMA node previously returned by alloc_vma.
 * No-op on nullptr. Does not detach from any tree.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void free_vma(vma* node);

/**
 * @brief Find VMA containing address.
 * Caller must hold mm_ctx->lock.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE vma* vma_find_locked(mm_context* mm_ctx, uintptr_t addr);

/**
 * @brief Find first VMA overlapping [start, end).
 * Caller must hold mm_ctx->lock.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE vma* vma_find_overlap_locked(
    mm_context* mm_ctx, uintptr_t start, uintptr_t end);

/**
 * @brief Insert VMA into address tree if it does not overlap neighbors.
 * Caller must hold mm_ctx->lock.
 * @return true on success, false if overlap/duplicate prevents insertion.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE bool vma_insert_locked(mm_context* mm_ctx, vma* node);

/**
 * @brief Remove VMA from address tree.
 * Caller must hold mm_ctx->lock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void vma_remove_locked(mm_context* mm_ctx, vma& node);

/**
 * @brief Find top-down gap in [mmap_base, mmap_end) with at least length bytes.
 * Caller must hold mm_ctx->lock.
 * @return Gap start address or 0 if none found.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE uintptr_t vma_find_gap_topdown_locked(
    mm_context* mm_ctx, size_t length);

/**
 * @brief Unmap [start, end) from a user mm_context, freeing physical pages.
 * Iterates page-by-page, pages that aren't mapped are skipped.
 * Used for anonymous/stack mappings the kernel owns.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void unmap_and_free_pages(
    mm_context* mm_ctx, uintptr_t start, uintptr_t end);

/**
 * @brief Unmap [start, end) from a user mm_context without freeing pages.
 * For shared and device mappings whose physical pages are owned elsewhere.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void unmap_pages_only(
    mm_context* mm_ctx, uintptr_t start, uintptr_t end);

/**
 * @brief Roll back a partially-completed eager allocation.
 * Equivalent to unmap_and_free_pages over the [start, mapped_end) prefix.
 * Used when an mm_context_map_* call needs to undo work after an error.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void rollback_new_pages(
    mm_context* mm_ctx, uintptr_t start, uintptr_t mapped_end);

/**
 * @brief Merge adjacent VMAs with identical prot/flags/backing.
 * Idempotent. Caller must hold mm_ctx->lock.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void coalesce_all_locked(mm_context* mm_ctx);

/**
 * @brief Split a VMA at split_addr into two adjacent VMAs.
 * The left part keeps the original node, the right part is freshly allocated
 * and inserted into the tree. Caller must hold mm_ctx->lock.
 * @return The newly-allocated right-hand VMA, or nullptr on allocation failure
 *         or out-of-range split_addr.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE vma* split_vma_locked(
    mm_context* mm_ctx, vma* node, uintptr_t split_addr);

/**
 * @brief Unmap every VMA overlapping [start, end), splitting at the edges.
 * Frees pages for owned VMAs and releases backing refs for shared/device VMAs.
 * Idempotent over already-unmapped regions. Caller must hold mm_ctx->lock.
 * @return MM_CTX_OK on success, MM_CTX_ERR_NO_MEM if an edge split fails.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t unmap_range_locked(
    mm_context* mm_ctx, uintptr_t start, uintptr_t end);

/**
 * @brief Check whether every page in [start, end) is covered by a VMA.
 * Caller must hold mm_ctx->lock.
 */
[[nodiscard]] bool range_fully_mapped_locked(
    mm_context* mm_ctx, uintptr_t start, uintptr_t end);

/**
 * @brief Apply new protection bits to the existing PTEs for [start, end).
 * Does not change VMA records, caller is responsible for VMA updates.
 * @return MM_CTX_OK on success, MM_CTX_ERR_NOT_MAPPED if any page is unmapped,
 *         MM_CTX_ERR_MAP_FAILED on PTE-update failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t apply_page_protection(
    mm_context* mm_ctx, uintptr_t start, uintptr_t end, uint32_t prot);

} // namespace mm

#endif // STELLUX_MM_VMA_H
