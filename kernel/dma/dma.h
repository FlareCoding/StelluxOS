#ifndef STELLUX_DMA_DMA_H
#define STELLUX_DMA_DMA_H

#include "common/types.h"
#include "mm/pmm_types.h"
#include "mm/paging_types.h"
#include "sync/spinlock.h"

/*
 * The DMA layer allocates memory that devices read and write directly. Each
 * allocation names its device with a `device_profile`: how many physical
 * address bits the device can drive, and whether it is coherent, meaning the
 * CPU caches take part in its accesses so cached memory is safe to share with
 * it. The caller picks the size, whether a lowered driver may touch the
 * buffer, and whether it may be cached. The layer places the buffer below the
 * device's address limit, maps it cached only when caching is allowed and the
 * device is coherent, and reports that choice in `buffer::cached`. The caller
 * owns each buffer until it frees it. A `pool` hands out small objects from
 * uncached pages it allocates up front.
 *
 * Drivers cannot build a coherent profile, because only the bus layer knows
 * whether a device is coherent. Code that always allocates uncached memory
 * builds its profile with `uncached_profile()`, since uncached memory is
 * correct for every device.
 */

namespace dma {

constexpr int32_t OK              = 0;
constexpr int32_t ERR_INVALID_ARG = -1;
constexpr int32_t ERR_NO_MEM      = -2;
constexpr int32_t ERR_NOT_FOUND   = -3;
constexpr int32_t ERR_FULL        = -4;

struct buffer {
    uintptr_t        virt;
    pmm::phys_addr_t phys;
    size_t           size;
    bool             cached; // whether the CPU mapping is cached, which other mappings must match
};

// A device's address limit and whether it is coherent
class device_profile {
public:
    uint8_t address_bits() const { return m_address_bits; }
    bool coherent() const { return m_coherent; }

private:
    friend struct device_profile_test_access;
    friend device_profile uncached_profile(uint8_t address_bits);

    // Coherence is claimed only by code that knows the platform's answer
    device_profile(uint8_t address_bits, bool coherent)
        : m_address_bits(address_bits), m_coherent(coherent) {}

    uint8_t m_address_bits;
    bool    m_coherent;
};

/**
 * @brief Describe a device by its address limit alone, for code that only
 * allocates uncached memory. The profile is never coherent.
 * @param address_bits Physical address bits the device can drive, 32 to 64.
 */
device_profile uncached_profile(uint8_t address_bits);

enum class caching : uint8_t {
    uncached,             // every CPU access goes to memory
    cached_when_coherent, // for buffers the CPU reads or writes in bulk, such as a scanout
};

/**
 * @brief Allocate zeroed, physically contiguous pages that `device` reaches.
 * The caller owns the buffer and releases it with `free_pages`.
 * @param device The device that will access the pages.
 * @param pages Number of pages, at least one.
 * @param mode Whether the buffer may be cached.
 * @param access 0, or `PAGE_USER` for a driver that touches the buffer while lowered.
 * @param out Receives the buffer, with `cached` set to the layer's choice.
 * @return OK on success, ERR_INVALID_ARG for zero pages, an address limit
 *         outside 32 to 64 bits or an access flag other than `PAGE_USER`,
 *         ERR_NO_MEM when no contiguous run is free. A limit of 33 to 63 bits
 *         is checked after allocating from any zone, so it can also fail with
 *         ERR_NO_MEM while memory below the limit is free.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE int32_t alloc_pages(
    const device_profile& device,
    size_t pages,
    caching mode,
    paging::page_flags_t access,
    buffer& out
);

/**
 * Free a DMA page-level allocation and zero the buffer. An empty buffer is ignored.
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
     * Initialize the pool with uncached backing pages that `device` reaches.
     * A pool must be destroyed before it is initialized again.
     * @param device The device that will access the objects.
     * @param object_size Bytes per object, from one to a page.
     * @param alignment Object alignment, a power of two up to a page.
     * @param capacity Number of objects, at least one.
     * @return OK on success, ERR_INVALID_ARG for an initialized pool, a size,
     *         alignment or capacity outside those bounds, objects spaced under
     *         64 bytes apart, more than 65535 backing pages or a profile
     *         `alloc_pages` refuses, ERR_NO_MEM when memory runs out or
     *         `alloc_pages` returns it.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t init(
        const device_profile& device,
        size_t object_size,
        size_t alignment,
        size_t capacity
    );

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
