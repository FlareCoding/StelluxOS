#ifndef STELLUX_MM_MM_H
#define STELLUX_MM_MM_H

#include "mm/vma.h"

namespace mm {

constexpr int32_t OK  = 0;
constexpr int32_t ERR = -1;

// Page-fault classification, arch-translated by the trap handlers
constexpr uint64_t PF_FLAG_PRESENT     = (1u << 0); // page was present, so must be a protection violation
constexpr uint64_t PF_FLAG_WRITE       = (1u << 1); // write access violation
constexpr uint64_t PF_FLAG_INSTRUCTION = (1u << 2); // instruction fetch (NX violation)

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
 * @brief Initialize the memory management subsystem.
 * Calls PMM, VA layout, KVA, and VMM init in order.
 * Must be called after boot_services::init() and arch::early_init().
 * @return OK on success, ERR on failure (sub-step failure is logged).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * @brief Attempt to resolve a userland page fault via on-demand paging.
 * @param mm_ctx Address-space context of the faulting task.
 * @param fault_address Linear address that triggered the fault
 *                      (x86 CR2, aarch64 FAR_EL1).
 * @param pf_flags Generic page fault flags describing the fault.
 * @return true if the fault was resolved and the instruction may safely retry,
 *         false if the access was invalid.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool handle_user_pf(
    mm_context* mm_ctx,
    uintptr_t fault_address,
    uint64_t pf_flags
);

/**
 * @brief handle_user_pf with mm_ctx->lock already held by the caller.
 * Never blocks, so it is safe from interrupt context.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool handle_user_pf_locked(
    mm_context* mm_ctx,
    uintptr_t fault_address,
    uint64_t pf_flags
);

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
 * Pages are not owned by the kernel - they are not freed on unmap.
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

#endif // STELLUX_MM_MM_H
