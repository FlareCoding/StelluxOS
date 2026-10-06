#ifndef STELLUX_DRIVERS_NET_VIRTIO_NET_H
#define STELLUX_DRIVERS_NET_VIRTIO_NET_H

#include "drivers/pci_driver.h"
#include "drivers/virtio/virtio_pci.h"
#include "drivers/virtio/virtqueue.h"
#include "net/interface.h"
#include "common/string.h"
#include "sync/spinlock.h"

namespace drivers::virtio {

// Virtio-net feature bits
constexpr uint64_t VIRTIO_NET_F_MAC         = (1ULL << 5);
constexpr uint64_t VIRTIO_NET_F_STATUS      = (1ULL << 16);
constexpr uint64_t VIRTIO_NET_F_MRG_RXBUF   = (1ULL << 15);

// Virtio-net device config (mapped via BAR at device config offset)
struct virtio_net_config {
    uint8_t  mac[6];
    uint16_t status;
} __attribute__((packed));

// Virtio-net header prepended to every packet (legacy format, 10 bytes,
// modern with VIRTIO_F_VERSION_1 uses 12 bytes with num_buffers)
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers; // only present with VIRTIO_F_VERSION_1
} __attribute__((packed));

constexpr uint8_t VIRTIO_NET_HDR_F_NEEDS_CSUM = 1;
constexpr uint8_t VIRTIO_NET_HDR_GSO_NONE     = 0;

// Virtio-net queue indices
constexpr uint16_t VIRTIO_NET_QUEUE_RX = 0;
constexpr uint16_t VIRTIO_NET_QUEUE_TX = 1;

} // namespace drivers::virtio

namespace drivers {

class virtio_net_driver : public pci_driver, public net::interface {
public:
    virtio_net_driver(pci::device* dev)
        : pci_driver("virtio_net", dev)
        , m_common_cfg(nullptr)
        , m_notify_base(0)
        , m_notify_off_multiplier(0)
        , m_isr_addr(0)
        , m_device_cfg(nullptr)
        , m_rx_notify_addr(0)
        , m_tx_notify_addr(0) {
        m_vq_lock = sync::SPINLOCK_INIT;
    }

    int32_t attach() override;
    int32_t detach() override;
    void run() override;

    int32_t transmit(net::packet* pkt) override;

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void on_interrupt(uint32_t vector) override;

private:
    // Virtio initialization helpers
    int32_t parse_virtio_caps();
    int32_t map_config_regions();
    int32_t negotiate_features();
    int32_t init_queues();
    void fill_rx_queue();
    int32_t read_mac();

    // Batch of received frames drained from the virtqueue under lock,
    // then released without the lock held.
    static constexpr uint16_t RX_BATCH_MAX = 16;
    struct rx_batch_entry {
        const uint8_t* data;
        size_t len;
        uint16_t buf_idx; // index into m_rx_bufs for clearing delivering flag
    };

    struct rx_batch {
        rx_batch_entry entries[RX_BATCH_MAX];
        uint16_t count;
    };

    // Packet I/O
    bool link_up();
    void drain_rx_locked(rx_batch& batch);     // requires m_vq_lock
    void deliver_rx_batch(rx_batch& batch);    // called without m_vq_lock
    void process_tx_completions();             // under lock
    void replenish_rx();                       // under lock

    // Virtio config access
    void write_status(uint8_t status);
    uint8_t read_status();

    // Parsed config locations
    virtio::virtio_pci_config  m_pci_cfg;

    // Mapped MMIO pointers
    volatile virtio::virtio_pci_common_cfg* m_common_cfg;
    uintptr_t m_notify_base;
    uint32_t  m_notify_off_multiplier;
    uintptr_t m_isr_addr;
    volatile virtio::virtio_net_config* m_device_cfg;

    // Notification addresses for each queue
    uintptr_t m_rx_notify_addr;
    uintptr_t m_tx_notify_addr;

    // Virtqueues
    virtio::virtqueue m_rxq;
    virtio::virtqueue m_txq;

    // RX buffer pool: pre-allocated DMA buffers for receiving packets
    static constexpr uint16_t RX_BUF_COUNT = 64;
    static constexpr size_t RX_BUF_SIZE = 2048; // enough for MTU + headers
    struct rx_buf_info {
        uintptr_t vaddr;
        pmm::phys_addr_t phys;
        int16_t desc_id;  // virtqueue descriptor currently using this buf, or -1
        bool delivering;  // true while frame data is being read by deliver_rx_batch
    };
    rx_buf_info m_rx_bufs[RX_BUF_COUNT];

    // TX buffer pool
    static constexpr uint16_t TX_BUF_COUNT = 64;
    static constexpr size_t TX_BUF_SIZE = 2048;
    struct tx_buf_info {
        uintptr_t vaddr;
        pmm::phys_addr_t phys;
        int16_t desc_id; // virtqueue descriptor assigned to this buf, or -1
        bool in_use;
    };
    tx_buf_info m_tx_bufs[TX_BUF_COUNT];

    // Protects all virtqueue and buffer pool state (m_rxq, m_txq, m_rx_bufs,
    // m_tx_bufs), held by run() and transmit().
    sync::spinlock m_vq_lock;

    // Feature flags
    bool m_has_mac = false;
    bool m_has_status = false;
    bool m_version_1 = false;

    // Virtio-net header size: 12 bytes with VIRTIO_F_VERSION_1, 10 without
    size_t m_net_hdr_size = 10;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_NET_VIRTIO_NET_H
