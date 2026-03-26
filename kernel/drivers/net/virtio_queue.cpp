#include "drivers/net/virtio_queue.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "mm/pmm_types.h"
#include "hw/mmio.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

namespace drivers::virtio {

// Alignment requirements from the virtio spec
constexpr size_t VRING_AVAIL_ALIGN = 2;
constexpr size_t VRING_USED_ALIGN  = 4;

static size_t align_up(size_t val, size_t align) {
    return (val + align - 1) & ~(align - 1);
}

int32_t virtqueue::init(uint16_t queue_size) {
    if (queue_size == 0 || (queue_size & (queue_size - 1)) != 0) {
        log::error("virtqueue: size %u is not a power of 2", queue_size);
        return -1;
    }
    if (queue_size > VIRTQ_MAX_SIZE) {
        queue_size = VIRTQ_MAX_SIZE;
    }

    m_size = queue_size;

    // Calculate memory layout sizes
    size_t desc_size = static_cast<size_t>(queue_size) * sizeof(vring_desc);
    size_t avail_size = sizeof(uint16_t) * 2 + sizeof(uint16_t) * queue_size + sizeof(uint16_t);
    size_t used_size = sizeof(uint16_t) * 2 + sizeof(vring_used_elem) * queue_size + sizeof(uint16_t);

    // Calculate offsets with alignment
    size_t avail_offset = align_up(desc_size, VRING_AVAIL_ALIGN);
    size_t used_offset = align_up(avail_offset + avail_size, VRING_USED_ALIGN);
    size_t total_size = used_offset + used_size;

    // Round up to pages
    size_t pages = (total_size + pmm::PAGE_SIZE - 1) / pmm::PAGE_SIZE;

    // Allocate contiguous DMA memory
    int32_t rc = 0;
    RUN_ELEVATED(
        rc = vmm::alloc_contiguous(
            pages,
            pmm::ZONE_DMA32,
            paging::PAGE_READ | paging::PAGE_WRITE | paging::PAGE_DMA,
            vmm::ALLOC_ZERO,
            kva::tag::generic,
            m_dma_vaddr,
            m_dma_phys)
    );

    if (rc != vmm::OK) {
        log::error("virtqueue: DMA alloc failed (%d)", rc);
        return -1;
    }

    m_dma_size = pages * pmm::PAGE_SIZE;

    // Set up pointers
    m_desc = reinterpret_cast<vring_desc*>(m_dma_vaddr);
    m_avail = reinterpret_cast<vring_avail*>(m_dma_vaddr + avail_offset);
    m_used = reinterpret_cast<vring_used*>(m_dma_vaddr + used_offset);

    // Physical addresses for device configuration
    m_desc_phys = m_dma_phys;
    m_avail_phys = m_dma_phys + avail_offset;
    m_used_phys = m_dma_phys + used_offset;

    // Initialize free list: chain all descriptors
    for (uint16_t i = 0; i < queue_size; i++) {
        m_desc[i].addr = 0;
        m_desc[i].len = 0;
        m_desc[i].flags = 0;
        m_desc[i].next = i + 1;
    }
    m_desc[queue_size - 1].next = 0xFFFF; // end of list sentinel

    m_free_head = 0;
    m_free_count = queue_size;
    m_last_used_idx = 0;
    m_avail->flags = 0;
    m_avail->idx = 0;

    return 0;
}

uint16_t virtqueue::alloc_desc() {
    if (m_free_count == 0) {
        return 0xFFFF;
    }

    uint16_t idx = m_free_head;
    m_free_head = m_desc[idx].next;
    m_free_count--;
    m_desc[idx].next = 0;
    return idx;
}

void virtqueue::free_desc(uint16_t id) {
    if (id >= m_size) return;

    // Walk chain if needed
    uint16_t current = id;
    while (true) {
        uint16_t next = m_desc[current].next;
        bool has_next = (m_desc[current].flags & VRING_DESC_F_NEXT) != 0;

        m_desc[current].addr = 0;
        m_desc[current].len = 0;
        m_desc[current].flags = 0;
        m_desc[current].next = m_free_head;
        m_free_head = current;
        m_free_count++;

        if (!has_next) break;
        current = next;
    }
}

int32_t virtqueue::add_buf(uint64_t phys_addr, uint32_t len, uint16_t flags) {
    uint16_t idx = alloc_desc();
    if (idx == 0xFFFF) {
        return -1;
    }

    m_desc[idx].addr = phys_addr;
    m_desc[idx].len = len;
    m_desc[idx].flags = flags;
    m_desc[idx].next = 0;

    // Add to available ring
    uint16_t avail_idx = m_avail->idx;
    m_avail->ring[avail_idx % m_size] = idx;

    // Memory barrier: ensure descriptor is visible before updating idx
    __atomic_thread_fence(__ATOMIC_RELEASE);
    m_avail->idx = avail_idx + 1;

    return static_cast<int32_t>(idx);
}

int32_t virtqueue::add_buf_chain(uint64_t hdr_phys, uint32_t hdr_len,
                                  uint64_t data_phys, uint32_t data_len,
                                  uint16_t hdr_flags, uint16_t data_flags) {
    if (m_free_count < 2) {
        return -1;
    }

    uint16_t head = alloc_desc();
    uint16_t tail = alloc_desc();
    if (head == 0xFFFF || tail == 0xFFFF) {
        if (head != 0xFFFF) free_desc(head);
        if (tail != 0xFFFF) free_desc(tail);
        return -1;
    }

    // Header descriptor
    m_desc[head].addr = hdr_phys;
    m_desc[head].len = hdr_len;
    m_desc[head].flags = hdr_flags | VRING_DESC_F_NEXT;
    m_desc[head].next = tail;

    // Data descriptor
    m_desc[tail].addr = data_phys;
    m_desc[tail].len = data_len;
    m_desc[tail].flags = data_flags;
    m_desc[tail].next = 0;

    // Add to available ring
    uint16_t avail_idx = m_avail->idx;
    m_avail->ring[avail_idx % m_size] = head;

    __atomic_thread_fence(__ATOMIC_RELEASE);
    m_avail->idx = avail_idx + 1;

    return static_cast<int32_t>(head);
}

bool virtqueue::has_used() const {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return m_last_used_idx != m_used->idx;
}

bool virtqueue::get_used(uint16_t* out_id, uint32_t* out_len) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (m_last_used_idx == m_used->idx) {
        return false;
    }

    uint16_t ring_idx = m_last_used_idx % m_size;
    *out_id = static_cast<uint16_t>(m_used->ring[ring_idx].id);
    *out_len = m_used->ring[ring_idx].len;
    m_last_used_idx++;

    return true;
}

void virtqueue::kick(uintptr_t notify_addr) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    mmio::write16(notify_addr, 0); // queue index is encoded in address
}

} // namespace drivers::virtio
