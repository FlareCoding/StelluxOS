#ifndef STELLUX_MM_VMA_H
#define STELLUX_MM_VMA_H

#include "common/types.h"
#include "common/rb_tree.h"
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

constexpr uint32_t VMA_FLAG_PRIVATE   = (1u << 0);
constexpr uint32_t VMA_FLAG_ANONYMOUS = (1u << 1);
constexpr uint32_t VMA_FLAG_ELF       = (1u << 2);
constexpr uint32_t VMA_FLAG_STACK     = (1u << 3);
constexpr uint32_t VMA_FLAG_SHARED    = (1u << 4);
constexpr uint32_t VMA_FLAG_DEVICE    = (1u << 5);

constexpr uintptr_t MMAP_BASE_DEFAULT = 0x00000080000000ULL;
constexpr uintptr_t USER_STACK_TOP    = 0x00007FFFFFF00000ULL;
constexpr size_t    USER_STACK_PAGES  = 8; // 32 KiB
constexpr size_t    USER_STACK_GUARD_PAGES = 1;

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

struct mm_context final : rc::ref_counted<mm_context> {
    pmm::phys_addr_t pt_root;
    uintptr_t        mmap_base;
    uintptr_t        mmap_end;
    sync::mutex      lock;
    vma_tree         vmas;

    /**
     * @brief Destroy mm_context and reclaim all mapped resources.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static void ref_destroy(mm_context* self);
};

/**
 * @brief Create a user address-space context with a new user page-table root.
 * @return New mm_context on success, nullptr on failure.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE mm_context* mm_context_create();

/**
 * @brief Increment mm_context reference count.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mm_context_add_ref(mm_context* mm_ctx);

/**
 * @brief Decrement mm_context reference count and destroy on last reference.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void mm_context_release(mm_context* mm_ctx);

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
 * @brief Track an already-mapped user range as a VMA.
 * Does not map physical pages.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t mm_context_add_vma(
    mm_context* mm_ctx,
    uintptr_t start,
    size_t length,
    uint32_t prot,
    uint32_t vma_flags
);

/**
 * @brief Map anonymous pages into a user mm_context and track as VMA.
 * Supports fixed and non-fixed allocation modes.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t mm_context_map_anonymous(
    mm_context* mm_ctx,
    uintptr_t addr,
    size_t length,
    uint32_t prot,
    uint32_t map_flags,
    uintptr_t* out_addr
);

/**
 * @brief Unmap [addr, addr+length) from a user mm_context.
 * Idempotent when the range is already unmapped.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t mm_context_unmap(
    mm_context* mm_ctx,
    uintptr_t addr,
    size_t length
);

/**
 * @brief Change protection of an existing mapped range.
 * Returns ERR_NOT_MAPPED when any part of the range is unmapped.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t mm_context_mprotect(
    mm_context* mm_ctx,
    uintptr_t addr,
    size_t length,
    uint32_t prot
);

/**
 * @brief Map a shmem backing into a user mm_context with MAP_SHARED semantics.
 * Pages come from the backing; they are not allocated per-mapping.
 * @param backing Shmem backing. Must have sufficient size for offset+length.
 * @param offset Byte offset into backing (must be page-aligned).
 * @param length Number of bytes to map (rounded up to page boundary).
 * @param prot MM_PROT_READ / MM_PROT_WRITE / MM_PROT_EXEC.
 * @param map_flags MM_MAP_SHARED, optionally MM_MAP_FIXED / MM_MAP_FIXED_NOREPLACE.
 * @param addr Hint or fixed address.
 * @param out_addr Receives the mapped virtual address.
 * @return MM_CTX_OK on success, error code on failure.
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
);

/**
 * @brief Map a contiguous physical address range into a user mm_context.
 * Pages are not owned by the kernel — they are not freed on unmap.
 * Useful for framebuffers, MMIO regions, and other device memory.
 * @param phys_base Physical base address (must be page-aligned).
 * @param length Number of bytes to map (rounded up to page boundary).
 * @param prot MM_PROT_READ / MM_PROT_WRITE / MM_PROT_EXEC.
 * @param cache_type Paging memory type (e.g. paging::PAGE_WC, paging::PAGE_DEVICE).
 * @param map_flags MM_MAP_SHARED, optionally MM_MAP_FIXED / MM_MAP_FIXED_NOREPLACE.
 * @param addr Hint or fixed address.
 * @param out_addr Receives the mapped virtual address.
 * @return MM_CTX_OK on success, error code on failure.
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
);

/**
 * @brief Return current VMA count.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE size_t mm_context_vma_count(mm_context* mm_ctx);

} // namespace mm

#endif // STELLUX_MM_VMA_H
