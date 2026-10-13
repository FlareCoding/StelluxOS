#include "dma/dma.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "mm/paging_types.h"
#include "common/logging.h"
#include "common/string.h"
#include "hw/cache.h"

namespace dma {

// The first zone ends at 4 GiB, so no zone serves a device that reaches less
constexpr uint8_t DMA32_ADDRESS_BITS = 32;
constexpr uint8_t MAX_ADDRESS_BITS   = 64;

constexpr uint16_t SLAB_BITMAP_BITS = 64;
[[maybe_unused]] constexpr uint8_t DEBUG_FREE_FILL = 0xDE;

static inline bool is_power_of_2(size_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

static inline size_t align_up(size_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

static inline uint64_t make_bitmap(uint16_t count) {
    if (count >= SLAB_BITMAP_BITS) return ~0ULL;
    return (1ULL << count) - 1;
}

static bool reaches(uint8_t address_bits, pmm::phys_addr_t end) {
    if (address_bits == MAX_ADDRESS_BITS) {
        return true;
    }

    return end <= (1ULL << address_bits);
}

device_profile uncached_profile(uint8_t address_bits) {
    return device_profile(address_bits, false);
}

[[nodiscard]] __PRIVILEGED_CODE int32_t alloc_pages(
    const device_profile& device,
    size_t pages,
    caching mode,
    paging::page_flags_t access,
    buffer& out
) {
    if (pages == 0) {
        return ERR_INVALID_ARG;
    }

    uint8_t address_bits = device.address_bits();
    bool is_valid_limit = address_bits >= DMA32_ADDRESS_BITS && address_bits <= MAX_ADDRESS_BITS;
    bool is_valid_access = (access & ~paging::PAGE_USER) == 0;
    if (!is_valid_limit || !is_valid_access) {
        return ERR_INVALID_ARG;
    }

    bool cached = mode == caching::cached_when_coherent && device.coherent();
    paging::page_flags_t memory_type = cached ? paging::PAGE_NORMAL : paging::PAGE_DMA;
    pmm::zone_mask_t zone = address_bits == DMA32_ADDRESS_BITS ? pmm::ZONE_DMA32 : pmm::ZONE_ANY;
    size_t size = pages * pmm::PAGE_SIZE;

    uintptr_t virt = 0;
    pmm::phys_addr_t phys = 0;
    int32_t rc = vmm::alloc_contiguous(
        pages,
        zone,
        paging::PAGE_KERNEL_RW | memory_type | access,
        vmm::ALLOC_ZERO,
        kva::tag::dma,
        virt,
        phys
    );
    if (rc != vmm::OK) {
        return ERR_NO_MEM;
    }

    // A 32-bit limit is the only one with a zone of its own, so other limits are checked here
    if (!reaches(address_bits, phys + size)) {
        vmm::free(virt);
        return ERR_NO_MEM;
    }

    // Earlier cached writes to these pages, the zeroing included, leave dirty lines that could later
    // overwrite an uncached buffer. A cached buffer goes only to a coherent device, which sees them.
    if (!cached) {
        cache::clean_dcache_poc(virt, size);
    }

    out = {virt, phys, size, cached};

    return OK;
}

__PRIVILEGED_CODE void free_pages(buffer& buf) {
    if (buf.virt != 0) {
        vmm::free(buf.virt);
    }
    buf.virt = 0;
    buf.phys = 0;
    buf.size = 0;
    buf.cached = false;
}

__PRIVILEGED_CODE int32_t pool::init(
    const device_profile& device,
    size_t object_size,
    size_t alignment,
    size_t capacity
) {
    if (m_initialized) {
        return ERR_INVALID_ARG;
    }

    if (object_size == 0 || capacity == 0) {
        return ERR_INVALID_ARG;
    }

    if (object_size > pmm::PAGE_SIZE) {
        return ERR_INVALID_ARG;
    }

    if (!is_power_of_2(alignment) || alignment > pmm::PAGE_SIZE) {
        return ERR_INVALID_ARG;
    }

    size_t stride = align_up(object_size, alignment);
    if (stride > pmm::PAGE_SIZE) {
        return ERR_INVALID_ARG;
    }

    uint16_t objs_per_page = static_cast<uint16_t>(pmm::PAGE_SIZE / stride);
    if (objs_per_page == 0) {
        return ERR_INVALID_ARG;
    }

    if (objs_per_page > SLAB_BITMAP_BITS) {
        return ERR_INVALID_ARG;
    }

    size_t computed_slab_count = (capacity + objs_per_page - 1) / objs_per_page;
    if (computed_slab_count > 0xFFFF) {
        return ERR_INVALID_ARG;
    }

    uint16_t slab_count = static_cast<uint16_t>(computed_slab_count);

    slab* slabs = static_cast<slab*>(
        heap::kalloc(static_cast<size_t>(slab_count) * sizeof(slab))
    );
    if (!slabs) {
        return ERR_NO_MEM;
    }

    size_t remaining = capacity;
    for (uint16_t i = 0; i < slab_count; i++) {
        buffer page_buf = {};
        int32_t rc = alloc_pages(device, 1, caching::uncached, 0, page_buf);
        if (rc != OK) {
            for (uint16_t j = 0; j < i; j++) {
                buffer rollback = {slabs[j].virt_base, slabs[j].phys_base, pmm::PAGE_SIZE, false};
                free_pages(rollback);
            }
            heap::kfree(slabs);
            return rc;
        }

        uint16_t slab_cap = (remaining < objs_per_page)
            ? static_cast<uint16_t>(remaining) : objs_per_page;

        slabs[i].virt_base = page_buf.virt;
        slabs[i].phys_base = page_buf.phys;
        slabs[i].free_bitmap = make_bitmap(slab_cap);
        slabs[i].free_count = slab_cap;
        slabs[i].capacity = slab_cap;

        remaining -= slab_cap;
    }

    m_lock = sync::SPINLOCK_INIT;
    m_slabs = slabs;
    m_slab_count = slab_count;

    m_obj_size = object_size;
    m_obj_stride = stride;
    m_objs_per_page = objs_per_page;

    m_total_capacity = capacity;
    m_used_count = 0;
    m_initialized = true;

    return OK;
}

[[nodiscard]] __PRIVILEGED_CODE int32_t pool::alloc(buffer& out) {
    if (!m_initialized) {
        return ERR_INVALID_ARG;
    }

    {
        sync::irq_lock_guard guard(m_lock);

        for (uint16_t i = 0; i < m_slab_count; i++) {
            slab& s = m_slabs[i];
            if (s.free_count == 0) continue;

            uint16_t idx = static_cast<uint16_t>(__builtin_ctzll(s.free_bitmap));
            s.free_bitmap &= ~(1ULL << idx);
            s.free_count--;
            m_used_count++;

            out.virt = s.virt_base + static_cast<size_t>(idx) * m_obj_stride;
            out.phys = s.phys_base + static_cast<size_t>(idx) * m_obj_stride;
            out.size = m_obj_size;
            out.cached = false;

            // Zeroing happens outside the lock (below)
            goto zero_and_return;
        }

        return ERR_FULL;
    }

zero_and_return:
    string::memset(reinterpret_cast<void*>(out.virt), 0, m_obj_size);
    return OK;
}

__PRIVILEGED_CODE void pool::free(const buffer& buf) {
    sync::irq_lock_guard guard(m_lock);

    for (uint16_t i = 0; i < m_slab_count; i++) {
        slab& s = m_slabs[i];
        if (buf.virt < s.virt_base || buf.virt >= s.virt_base + pmm::PAGE_SIZE) {
            continue;
        }

        size_t offset = buf.virt - s.virt_base;
        if (offset % m_obj_stride != 0) {
            log::fatal("dma::pool::free: unaligned pointer 0x%lx (stride=%lu)",
                       buf.virt, m_obj_stride);
        }

        uint16_t idx = static_cast<uint16_t>(offset / m_obj_stride);
        if (idx >= s.capacity) {
            log::fatal("dma::pool::free: index %u out of range (capacity=%u)",
                       idx, s.capacity);
        }

        if (s.free_bitmap & (1ULL << idx)) {
            log::fatal("dma::pool::free: double-free at index %u in slab %u", idx, i);
        }

        s.free_bitmap |= (1ULL << idx);
        s.free_count++;
        m_used_count--;

#ifdef DEBUG
        string::memset(reinterpret_cast<void*>(buf.virt), DEBUG_FREE_FILL, m_obj_size);
#endif
        return;
    }

    log::fatal("dma::pool::free: pointer 0x%lx does not belong to this pool", buf.virt);
}

__PRIVILEGED_CODE void pool::destroy() {
    if (!m_initialized) return;

    sync::irq_state irq = sync::spin_lock_irqsave(m_lock);

    if (m_used_count > 0) {
        log::fatal("dma::pool::destroy: %lu objects still allocated", m_used_count);
    }

    for (uint16_t i = 0; i < m_slab_count; i++) {
        buffer page_buf = {m_slabs[i].virt_base, m_slabs[i].phys_base, pmm::PAGE_SIZE, false};
        free_pages(page_buf);
    }

    heap::kfree(m_slabs);

    m_slabs = nullptr;
    m_slab_count = 0;

    m_obj_size = 0;
    m_obj_stride = 0;
    m_objs_per_page = 0;

    m_total_capacity = 0;
    m_used_count = 0;
    m_initialized = false;

    sync::spin_unlock_irqrestore(m_lock, irq);
    m_lock = sync::SPINLOCK_INIT;
}

} // namespace dma
