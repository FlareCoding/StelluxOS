#ifndef STELLUX_DRIVERS_NET_VIRTIO_QUEUE_H
#define STELLUX_DRIVERS_NET_VIRTIO_QUEUE_H

#include "common/types.h"
#include "mm/pmm_types.h"

namespace drivers::virtio {

// Virtqueue descriptor flags
constexpr uint16_t VRING_DESC_F_NEXT     = 1;
constexpr uint16_t VRING_DESC_F_WRITE    = 2; // buffer is device-writable
constexpr uint16_t VRING_DESC_F_INDIRECT = 4;

// Virtqueue descriptor
struct vring_desc {
    uint64_t addr;   // physical address
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

// Available ring header
struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[]; // variable length
} __attribute__((packed));

// Used ring element
struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

// Used ring header
struct vring_used {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem ring[]; // variable length
} __attribute__((packed));

constexpr uint16_t VIRTQ_MAX_SIZE = 256;

/**
 * Split virtqueue implementation.
 * Allocates descriptor table, available ring, and used ring in one
 * contiguous DMA allocation with proper alignment per the virtio spec.
 */
class virtqueue {
public:
    virtqueue() = default;

    /**
     * Initialize the virtqueue. Allocates DMA memory for descriptor table,
     * available ring, and used ring.
     * @param queue_size Number of descriptors (must be power of 2).
     * @param queue_index Virtio queue index (0=RX, 1=TX, etc.) for spec-compliant notification.
     * @return 0 on success, negative on failure.
     */
    int32_t init(uint16_t queue_size, uint16_t queue_index = 0);

    /**
     * Add a single buffer (one descriptor) to the available ring.
     * @param phys_addr Physical address of the buffer.
     * @param len       Length of the buffer.
     * @param flags     Descriptor flags (VRING_DESC_F_WRITE for device-writable).
     * @return Descriptor index on success, -1 on failure (queue full).
     */
    int32_t add_buf(uint64_t phys_addr, uint32_t len, uint16_t flags);

    /**
     * Add a chained two-descriptor buffer (header + data).
     * @return First descriptor index, or -1 on failure.
     */
    int32_t add_buf_chain(uint64_t hdr_phys, uint32_t hdr_len,
                          uint64_t data_phys, uint32_t data_len,
                          uint16_t hdr_flags, uint16_t data_flags);

    /**
     * Get the next used buffer.
     * @param out_id   Receives the descriptor index.
     * @param out_len  Receives the number of bytes written by the device.
     * @return true if a used buffer was available, false otherwise.
     */
    bool get_used(uint16_t* out_id, uint32_t* out_len);

    /**
     * Return a descriptor (or chain) to the free list.
     */
    void free_desc(uint16_t id);

    /**
     * Check if there are used buffers to process.
     */
    bool has_used() const;

    /**
     * Notify the device by writing to the doorbell.
     * @param notify_addr Virtual address of the notification register.
     */
    void kick(uintptr_t notify_addr);

    // Accessors for device configuration
    uint16_t size() const { return m_size; }
    pmm::phys_addr_t desc_phys() const { return m_desc_phys; }
    pmm::phys_addr_t avail_phys() const { return m_avail_phys; }
    pmm::phys_addr_t used_phys() const { return m_used_phys; }

private:
    uint16_t alloc_desc();

    uint16_t          m_size = 0;
    uint16_t          m_queue_index = 0;
    vring_desc*       m_desc = nullptr;
    vring_avail*      m_avail = nullptr;
    vring_used*       m_used = nullptr;
    uint16_t          m_last_used_idx = 0;
    uint16_t          m_free_head = 0;
    uint16_t          m_free_count = 0;

    // DMA allocation tracking
    uintptr_t         m_dma_vaddr = 0;
    pmm::phys_addr_t  m_dma_phys = 0;
    size_t            m_dma_size = 0;

    // Physical addresses for device configuration
    pmm::phys_addr_t  m_desc_phys = 0;
    pmm::phys_addr_t  m_avail_phys = 0;
    pmm::phys_addr_t  m_used_phys = 0;
};

} // namespace drivers::virtio

#endif // STELLUX_DRIVERS_NET_VIRTIO_QUEUE_H
