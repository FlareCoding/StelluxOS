#ifndef STELLUX_DMA_DMA_H
#define STELLUX_DMA_DMA_H

#include "common/types.h"
#include "mm/pmm_types.h"
#include "sync/spinlock.h"

namespace dma {

constexpr int32_t OK              = 0;
constexpr int32_t ERR_INVALID_ARG = -1;
constexpr int32_t ERR_NO_MEM      = -2;
constexpr int32_t ERR_NOT_FOUND   = -3;
constexpr int32_t ERR_FULL        = -4;

struct buffer {
    uintptr_t virt;
    pmm::phys_addr_t phys;
    size_t size;
};

/**
 * Allocate physically contiguous DMA-safe pages.
 * Memory is zeroed and mapped non-cacheable on both architectures.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE
int32_t alloc_pages(size_t pages, buffer& out,
                    pmm::zone_mask_t zone = pmm::ZONE_ANY);

/**
 * Free a DMA page-level allocation. Zeroes the buffer struct.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void free_pages(buffer& buf);

/**
 * Pre-allocated pool of fixed-size DMA objects backed by non-cacheable pages.
 * Objects within a page never cross a page boundary.
 */
class pool {
public:
    /**
     * Initialize the pool with pre-allocated backing pages.
     * Fatals if called on an already-initialized pool (call destroy() first).
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t init(size_t object_size, size_t alignment,
                                   size_t capacity,
                                   pmm::zone_mask_t zone = pmm::ZONE_ANY);

    /**
     * Allocate one zeroed object from the pool.
     * @note Privilege: **required**
     */
    [[nodiscard]] __PRIVILEGED_CODE int32_t alloc(buffer& out);

    /**
     * Return an object to the pool. Fatals on invalid or double-free.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void free(const buffer& buf);

    /**
     * Destroy the pool and release all backing pages.
     * Fatals if any objects are still allocated. No-op if not initialized.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void destroy();

    size_t object_size() const { return m_obj_size; }
    size_t capacity() const { return m_total_capacity; }
    size_t used_count() const { return m_used_count; }

private:
    struct slab {
        uintptr_t virt_base;
        pmm::phys_addr_t phys_base;
        uint64_t free_bitmap;
        uint16_t free_count;
        uint16_t capacity;
    };

    slab*            m_slabs;
    uint16_t         m_slab_count;
    size_t           m_obj_size;
    size_t           m_obj_stride;
    uint16_t         m_objs_per_page;
    size_t           m_total_capacity;
    size_t           m_used_count;
    sync::spinlock   m_lock;
    bool             m_initialized;
};

} // namespace dma

#endif // STELLUX_DMA_DMA_H
