#ifndef STELLUX_DRIVERS_NET_VIRTIO_NET_H
#define STELLUX_DRIVERS_NET_VIRTIO_NET_H

#include "drivers/pci_driver.h"
#include "drivers/net/virtio_pci.h"
#include "drivers/net/virtio_queue.h"
#include "net/net.h"
#include "common/string.h"
#include "sync/spinlock.h"

namespace drivers {

class virtio_net_driver : public pci_driver {
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
        // Zero netif including any padding
        uint8_t* p = reinterpret_cast<uint8_t*>(&m_netif);
        for (size_t i = 0; i < sizeof(m_netif); i++) p[i] = 0;
        m_vq_lock = sync::SPINLOCK_INIT;
    }

    int32_t attach() override;
    int32_t detach() override;
    void run() override;

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
    // then delivered to the protocol stack without the lock held.
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
    static int32_t tx_callback(net::netif* iface, const uint8_t* frame, size_t len);
    static bool link_callback(net::netif* iface);
    static void poll_callback(net::netif* iface);
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

    // Spinlock protecting all virtqueue and buffer pool state.
    // Must be held by poll_callback, run(), tx_callback, and any
    // code that touches m_rxq, m_txq, m_rx_bufs, or m_tx_bufs.
    sync::spinlock m_vq_lock;

    // Network interface
    net::netif m_netif;

    // Feature flags
    bool m_has_mac = false;
    bool m_has_status = false;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_NET_VIRTIO_NET_H
