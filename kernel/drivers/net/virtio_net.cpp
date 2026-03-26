#include "drivers/net/virtio_net.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "hw/mmio.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"
#include "net/net.h"
#include "net/ipv4.h"

namespace drivers {

using namespace virtio;

// Parse virtio PCI capabilities from the PCI config space capability list.
// Virtio uses vendor-specific caps (id=0x09) with a cfg_type field.
int32_t virtio_net_driver::parse_virtio_caps() {
    string::memset(&m_pci_cfg, 0, sizeof(m_pci_cfg));

    // Walk the PCI capability list
    uint16_t status = 0;
    RUN_ELEVATED(status = m_dev->config_read16(pci::CFG_STATUS));
    if (!(status & pci::STS_CAPABILITIES)) {
        log::error("virtio-net: device has no PCI capabilities");
        return -1;
    }

    uint8_t cap_ptr = 0;
    RUN_ELEVATED(cap_ptr = m_dev->config_read8(pci::CFG_CAP_PTR));
    cap_ptr &= 0xFC;

    uint32_t visited = 0;
    while (cap_ptr != 0 && visited < 48) {
        uint8_t cap_id = 0;
        RUN_ELEVATED(cap_id = m_dev->config_read8(cap_ptr));

        if (cap_id == 0x09) { // Vendor-specific = virtio
            uint8_t cfg_type = 0;
            uint8_t bar = 0;
            uint32_t offset = 0;
            uint32_t length = 0;

            RUN_ELEVATED({
                cfg_type = m_dev->config_read8(cap_ptr + 3);
                bar = m_dev->config_read8(cap_ptr + 4);
                offset = m_dev->config_read32(cap_ptr + 8);
                length = m_dev->config_read32(cap_ptr + 12);
            });

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                m_pci_cfg.common_bar = bar;
                m_pci_cfg.common_offset = offset;
                m_pci_cfg.common_length = length;
                m_pci_cfg.has_common = true;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                m_pci_cfg.notify_bar = bar;
                m_pci_cfg.notify_offset = offset;
                m_pci_cfg.notify_length = length;
                m_pci_cfg.has_notify = true;
                // Read the notify_off_multiplier (4 bytes after the standard cap)
                RUN_ELEVATED(
                    m_pci_cfg.notify_off_multiplier = m_dev->config_read32(cap_ptr + 16)
                );
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                m_pci_cfg.isr_bar = bar;
                m_pci_cfg.isr_offset = offset;
                m_pci_cfg.has_isr = true;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                m_pci_cfg.device_bar = bar;
                m_pci_cfg.device_offset = offset;
                m_pci_cfg.device_length = length;
                m_pci_cfg.has_device = true;
                break;
            }
        }

        uint8_t next = 0;
        RUN_ELEVATED(next = m_dev->config_read8(cap_ptr + 1));
        cap_ptr = next & 0xFC;
        visited++;
    }

    if (!m_pci_cfg.has_common) {
        log::error("virtio-net: missing common config capability");
        return -1;
    }

    return 0;
}

int32_t virtio_net_driver::map_config_regions() {
    // Map the BAR that contains the common config
    uintptr_t bar_va = 0;
    int32_t rc = map_bar(m_pci_cfg.common_bar, bar_va);
    if (rc != 0) {
        log::error("virtio-net: failed to map BAR %u", m_pci_cfg.common_bar);
        return rc;
    }

    m_common_cfg = reinterpret_cast<volatile virtio_pci_common_cfg*>(
        bar_va + m_pci_cfg.common_offset);

    // If notify is on the same BAR, compute its address
    if (m_pci_cfg.has_notify) {
        uintptr_t notify_bar_va = 0;
        if (m_pci_cfg.notify_bar == m_pci_cfg.common_bar) {
            notify_bar_va = bar_va;
        } else {
            rc = map_bar(m_pci_cfg.notify_bar, notify_bar_va);
            if (rc != 0) {
                log::error("virtio-net: failed to map notify BAR %u", m_pci_cfg.notify_bar);
                return rc;
            }
        }
        m_notify_base = notify_bar_va + m_pci_cfg.notify_offset;
        m_notify_off_multiplier = m_pci_cfg.notify_off_multiplier;
    }

    // ISR config
    if (m_pci_cfg.has_isr) {
        uintptr_t isr_bar_va = 0;
        if (m_pci_cfg.isr_bar == m_pci_cfg.common_bar) {
            isr_bar_va = bar_va;
        } else {
            rc = map_bar(m_pci_cfg.isr_bar, isr_bar_va);
            if (rc != 0) {
                log::error("virtio-net: failed to map ISR BAR %u", m_pci_cfg.isr_bar);
                return rc;
            }
        }
        m_isr_addr = isr_bar_va + m_pci_cfg.isr_offset;
    }

    // Device-specific config (MAC address, etc.)
    if (m_pci_cfg.has_device) {
        uintptr_t dev_bar_va = 0;
        if (m_pci_cfg.device_bar == m_pci_cfg.common_bar) {
            dev_bar_va = bar_va;
        } else {
            rc = map_bar(m_pci_cfg.device_bar, dev_bar_va);
            if (rc != 0) {
                log::error("virtio-net: failed to map device BAR %u", m_pci_cfg.device_bar);
                return rc;
            }
        }
        m_device_cfg = reinterpret_cast<volatile virtio_net_config*>(
            dev_bar_va + m_pci_cfg.device_offset);
    }

    return 0;
}

void virtio_net_driver::write_status(uint8_t status) {
    m_common_cfg->device_status = status;
    // Read back to ensure write is flushed
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

uint8_t virtio_net_driver::read_status() {
    return m_common_cfg->device_status;
}

int32_t virtio_net_driver::negotiate_features() {
    // Read device features (select page 0 for bits 0-31)
    m_common_cfg->device_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t features_lo = m_common_cfg->device_feature;

    // Read device features (select page 1 for bits 32-63)
    m_common_cfg->device_feature_select = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t features_hi = m_common_cfg->device_feature;

    uint64_t device_features = (static_cast<uint64_t>(features_hi) << 32) | features_lo;

    // Select features we want
    uint64_t driver_features = 0;
    if (device_features & VIRTIO_NET_F_MAC) {
        driver_features |= VIRTIO_NET_F_MAC;
        m_has_mac = true;
    }
    if (device_features & VIRTIO_NET_F_STATUS) {
        driver_features |= VIRTIO_NET_F_STATUS;
        m_has_status = true;
    }
    // Always negotiate VERSION_1 if available
    if (device_features & VIRTIO_F_VERSION_1) {
        driver_features |= VIRTIO_F_VERSION_1;
    }

    log::info("virtio-net: device features=0x%lx, driver features=0x%lx",
              device_features, driver_features);

    // Write driver features
    m_common_cfg->driver_feature_select = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    m_common_cfg->driver_feature = static_cast<uint32_t>(driver_features & 0xFFFFFFFF);

    m_common_cfg->driver_feature_select = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    m_common_cfg->driver_feature = static_cast<uint32_t>(driver_features >> 32);

    return 0;
}

int32_t virtio_net_driver::read_mac() {
    if (!m_device_cfg || !m_has_mac) {
        // Generate a random-ish MAC
        m_netif.mac[0] = 0x52;
        m_netif.mac[1] = 0x54;
        m_netif.mac[2] = 0x00;
        m_netif.mac[3] = 0x12;
        m_netif.mac[4] = 0x34;
        m_netif.mac[5] = 0x56;
        return 0;
    }

    for (int i = 0; i < 6; i++) {
        m_netif.mac[i] = m_device_cfg->mac[i];
    }

    log::info("virtio-net: MAC %02x:%02x:%02x:%02x:%02x:%02x",
              m_netif.mac[0], m_netif.mac[1], m_netif.mac[2],
              m_netif.mac[3], m_netif.mac[4], m_netif.mac[5]);
    return 0;
}

int32_t virtio_net_driver::init_queues() {
    // Initialize RX queue (queue index 0)
    m_common_cfg->queue_select = VIRTIO_NET_QUEUE_RX;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint16_t rx_size = m_common_cfg->queue_size;
    if (rx_size == 0) {
        log::error("virtio-net: RX queue size is 0");
        return -1;
    }
    if (rx_size > virtio::VIRTQ_MAX_SIZE) {
        rx_size = virtio::VIRTQ_MAX_SIZE;
    }
    // Set the queue size we want
    m_common_cfg->queue_size = rx_size;

    int32_t rc = m_rxq.init(rx_size, VIRTIO_NET_QUEUE_RX);
    if (rc != 0) {
        log::error("virtio-net: RX virtqueue init failed");
        return rc;
    }

    // Tell device where the rings are
    m_common_cfg->queue_desc = m_rxq.desc_phys();
    m_common_cfg->queue_avail = m_rxq.avail_phys();
    m_common_cfg->queue_used = m_rxq.used_phys();
    m_common_cfg->queue_enable = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    uint16_t rx_notify_off = m_common_cfg->queue_notify_off;
    m_rx_notify_addr = m_notify_base +
        static_cast<uintptr_t>(rx_notify_off) * m_notify_off_multiplier;

    // Initialize TX queue (queue index 1)
    m_common_cfg->queue_select = VIRTIO_NET_QUEUE_TX;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint16_t tx_size = m_common_cfg->queue_size;
    if (tx_size == 0) {
        log::error("virtio-net: TX queue size is 0");
        return -1;
    }
    if (tx_size > virtio::VIRTQ_MAX_SIZE) {
        tx_size = virtio::VIRTQ_MAX_SIZE;
    }
    m_common_cfg->queue_size = tx_size;

    rc = m_txq.init(tx_size, VIRTIO_NET_QUEUE_TX);
    if (rc != 0) {
        log::error("virtio-net: TX virtqueue init failed");
        return rc;
    }

    m_common_cfg->queue_desc = m_txq.desc_phys();
    m_common_cfg->queue_avail = m_txq.avail_phys();
    m_common_cfg->queue_used = m_txq.used_phys();
    m_common_cfg->queue_enable = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    uint16_t tx_notify_off = m_common_cfg->queue_notify_off;
    m_tx_notify_addr = m_notify_base +
        static_cast<uintptr_t>(tx_notify_off) * m_notify_off_multiplier;

    log::info("virtio-net: RX queue (%u entries), TX queue (%u entries)",
              rx_size, tx_size);

    // Allocate RX buffers
    for (uint16_t i = 0; i < RX_BUF_COUNT; i++) {
        m_rx_bufs[i].vaddr = 0;
        m_rx_bufs[i].phys = 0;
        m_rx_bufs[i].desc_id = -1;
        m_rx_bufs[i].delivering = false;

        int32_t alloc_rc = 0;
        RUN_ELEVATED(
            alloc_rc = vmm::alloc_contiguous(
                1, pmm::ZONE_DMA32,
                paging::PAGE_READ | paging::PAGE_WRITE | paging::PAGE_DMA,
                vmm::ALLOC_ZERO, kva::tag::generic,
                m_rx_bufs[i].vaddr, m_rx_bufs[i].phys)
        );
        if (alloc_rc != vmm::OK) {
            log::error("virtio-net: failed to allocate RX buffer %u", i);
            return -1;
        }
    }

    // Allocate TX buffers
    for (uint16_t i = 0; i < TX_BUF_COUNT; i++) {
        m_tx_bufs[i].vaddr = 0;
        m_tx_bufs[i].phys = 0;
        m_tx_bufs[i].desc_id = -1;
        m_tx_bufs[i].in_use = false;

        int32_t alloc_rc = 0;
        RUN_ELEVATED(
            alloc_rc = vmm::alloc_contiguous(
                1, pmm::ZONE_DMA32,
                paging::PAGE_READ | paging::PAGE_WRITE | paging::PAGE_DMA,
                vmm::ALLOC_ZERO, kva::tag::generic,
                m_tx_bufs[i].vaddr, m_tx_bufs[i].phys)
        );
        if (alloc_rc != vmm::OK) {
            log::error("virtio-net: failed to allocate TX buffer %u", i);
            return -1;
        }
    }

    return 0;
}

void virtio_net_driver::fill_rx_queue() {
    for (uint16_t i = 0; i < RX_BUF_COUNT; i++) {
        if (m_rx_bufs[i].desc_id >= 0) continue; // already posted

        int32_t desc_id = m_rxq.add_buf(
            m_rx_bufs[i].phys, RX_BUF_SIZE, VRING_DESC_F_WRITE);
        if (desc_id < 0) break;
        m_rx_bufs[i].desc_id = static_cast<int16_t>(desc_id);
    }

    m_rxq.kick(m_rx_notify_addr);
}

int32_t virtio_net_driver::attach() {
    log::info("virtio-net: attaching to %02x:%02x.%x",
              m_dev->bus(), m_dev->slot(), m_dev->func());

    // Enable the device
    RUN_ELEVATED({
        m_dev->enable();
        m_dev->enable_bus_mastering();
    });

    // Parse virtio PCI capabilities
    int32_t rc = parse_virtio_caps();
    if (rc != 0) return rc;

    // Map config regions
    rc = map_config_regions();
    if (rc != 0) return rc;

    // Reset device
    write_status(0);
    while (read_status() != 0) { }

    // Acknowledge and identify as a driver
    write_status(VIRTIO_STATUS_ACKNOWLEDGE);
    write_status(read_status() | VIRTIO_STATUS_DRIVER);

    // Negotiate features
    rc = negotiate_features();
    if (rc != 0) {
        write_status(read_status() | VIRTIO_STATUS_FAILED);
        return rc;
    }

    // Finalize feature negotiation
    write_status(read_status() | VIRTIO_STATUS_FEATURES_OK);
    if (!(read_status() & VIRTIO_STATUS_FEATURES_OK)) {
        log::error("virtio-net: device did not accept features");
        write_status(read_status() | VIRTIO_STATUS_FAILED);
        return -1;
    }

    // Read MAC address and initialize virtqueues
    rc = read_mac();
    if (rc != 0) return rc;

    rc = init_queues();
    if (rc != 0) {
        write_status(read_status() | VIRTIO_STATUS_FAILED);
        return rc;
    }

    fill_rx_queue();

    // Set up MSI-X interrupts (fall back to MSI, then polling)
    int32_t irq_rc = setup_msix(2);
    if (irq_rc != 0) {
        irq_rc = setup_msi(1);
    }
    if (irq_rc != 0) {
        log::warn("virtio-net: MSI setup failed, will use polling");
    }

    // Assign MSI-X vectors to queues
    if (m_dev->get_msi_state().mode == pci::MSI_MODE_MSIX) {
        m_common_cfg->msix_config = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        m_common_cfg->queue_select = VIRTIO_NET_QUEUE_RX;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        m_common_cfg->queue_msix_vector = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

        m_common_cfg->queue_select = VIRTIO_NET_QUEUE_TX;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        m_common_cfg->queue_msix_vector = (m_dev->get_msi_state().vector_count > 1) ? 1 : 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }

    // Mark device as ready
    write_status(read_status() | VIRTIO_STATUS_DRIVER_OK);

    log::info("virtio-net: DRIVER_OK, device is live");

    // Register with network stack
    string::memcpy(m_netif.name, "eth0", 5);
    m_netif.transmit = tx_callback;
    m_netif.link_up = link_callback;
    m_netif.poll = poll_callback;
    m_netif.driver_data = this;

    net::register_netif(&m_netif);

    // Static IP configuration for QEMU user-mode networking
    net::configure(&m_netif,
                   net::ipv4_addr(10, 0, 2, 15),     // IP
                   net::ipv4_addr(255, 255, 255, 0),  // Netmask
                   net::ipv4_addr(10, 0, 2, 2));      // Gateway

    return 0;
}

int32_t virtio_net_driver::detach() {
    // Reset device
    write_status(0);
    return pci_driver::detach();
}

__PRIVILEGED_CODE void virtio_net_driver::on_interrupt(uint32_t vector) {
    (void)vector;
    // Read ISR status to acknowledge the interrupt
    if (m_isr_addr) {
        mmio::read8(m_isr_addr);
    }
}

void virtio_net_driver::drain_rx_locked(rx_batch& batch) {
    // Drain used buffers from the virtqueue into a local batch.
    // Caller must hold m_vq_lock.
    batch.count = 0;

    uint16_t desc_id;
    uint32_t len;

    while (batch.count < RX_BATCH_MAX && m_rxq.get_used(&desc_id, &len)) {
        // Find which buffer this descriptor belongs to
        int buf_idx = -1;
        for (uint16_t i = 0; i < RX_BUF_COUNT; i++) {
            if (m_rx_bufs[i].desc_id == static_cast<int16_t>(desc_id)) {
                buf_idx = static_cast<int>(i);
                break;
            }
        }

        if (buf_idx < 0) {
            log::warn("virtio-net: RX used desc %u not found in buffer table", desc_id);
            m_rxq.free_desc(desc_id);
            continue;
        }

        // The buffer contains: virtio_net_hdr + Ethernet frame
        uintptr_t buf_vaddr = m_rx_bufs[buf_idx].vaddr;
        size_t hdr_size = sizeof(virtio_net_hdr);

        // Return descriptor to free list
        m_rxq.free_desc(desc_id);
        m_rx_bufs[buf_idx].desc_id = -1;

        if (len > hdr_size) {
            // Mark buffer as being delivered so replenish_rx skips it
            m_rx_bufs[buf_idx].delivering = true;
            batch.entries[batch.count].data =
                reinterpret_cast<const uint8_t*>(buf_vaddr + hdr_size);
            batch.entries[batch.count].len = len - hdr_size;
            batch.entries[batch.count].buf_idx = static_cast<uint16_t>(buf_idx);
            batch.count++;
        }
    }
}

void virtio_net_driver::deliver_rx_batch(rx_batch& batch) {
    // Deliver drained frames to the protocol stack.
    // Called without m_vq_lock held so protocol processing can
    // call back into tx_callback (e.g. ARP replies) without deadlock.
    for (uint16_t i = 0; i < batch.count; i++) {
        net::rx_frame(&m_netif, batch.entries[i].data, batch.entries[i].len);
        // Clear delivering flag so replenish_rx can re-post this buffer
        m_rx_bufs[batch.entries[i].buf_idx].delivering = false;
    }
}

void virtio_net_driver::replenish_rx() {
    bool posted_any = false;
    for (uint16_t i = 0; i < RX_BUF_COUNT; i++) {
        if (m_rx_bufs[i].desc_id >= 0) continue; // already posted
        if (m_rx_bufs[i].delivering) continue;    // being read by deliver_rx_batch

        int32_t desc_id = m_rxq.add_buf(
            m_rx_bufs[i].phys, RX_BUF_SIZE, VRING_DESC_F_WRITE);
        if (desc_id < 0) break;
        m_rx_bufs[i].desc_id = static_cast<int16_t>(desc_id);
        posted_any = true;
    }

    if (posted_any) {
        m_rxq.kick(m_rx_notify_addr);
    }
}

void virtio_net_driver::process_tx_completions() {
    uint16_t desc_id;
    uint32_t len;

    while (m_txq.get_used(&desc_id, &len)) {
        // Free the descriptor chain in the virtqueue
        m_txq.free_desc(desc_id);

        // Find the TX buffer that was assigned this descriptor
        for (uint16_t i = 0; i < TX_BUF_COUNT; i++) {
            if (m_tx_bufs[i].in_use &&
                m_tx_bufs[i].desc_id == static_cast<int16_t>(desc_id)) {
                m_tx_bufs[i].in_use = false;
                m_tx_bufs[i].desc_id = -1;
                break;
            }
        }
    }
}

void virtio_net_driver::run() {
    log::info("virtio-net: driver task running");

    // Check MSI mode once at start (elevated because m_dev is in privileged memory)
    bool has_msi = false;
    RUN_ELEVATED(has_msi = m_dev->get_msi_state().mode != pci::MSI_MODE_NONE);

    while (true) {
        // Wait for interrupt or poll periodically
        if (has_msi) {
            wait_for_event();
        } else {
            RUN_ELEVATED(sched::sleep_ms(1));
        }

        // Drain RX under lock, deliver frames without the lock so
        // protocol handlers can transmit, then re-lock to replenish.
        rx_batch batch;
        RUN_ELEVATED({
            sync::irq_lock_guard guard(m_vq_lock);
            drain_rx_locked(batch);
            process_tx_completions();
        });
        RUN_ELEVATED(deliver_rx_batch(batch));
        RUN_ELEVATED({
            sync::irq_lock_guard guard(m_vq_lock);
            replenish_rx();
        });
    }
}

int32_t virtio_net_driver::tx_callback(net::netif* iface, const uint8_t* frame, size_t len) {
    if (!iface || !frame || len == 0) return -1;

    auto* drv = static_cast<virtio_net_driver*>(iface->driver_data);
    if (!drv) return -1;

    int32_t result = -1;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(drv->m_vq_lock);

        // Find a free TX buffer
        int32_t buf_idx = -1;
        for (uint16_t i = 0; i < TX_BUF_COUNT; i++) {
            if (!drv->m_tx_bufs[i].in_use) {
                buf_idx = static_cast<int32_t>(i);
                break;
            }
        }

        if (buf_idx < 0) {
            // Process completions and try again
            drv->process_tx_completions();
            for (uint16_t i = 0; i < TX_BUF_COUNT; i++) {
                if (!drv->m_tx_bufs[i].in_use) {
                    buf_idx = static_cast<int32_t>(i);
                    break;
                }
            }
        }

        if (buf_idx >= 0) {
            auto& buf = drv->m_tx_bufs[buf_idx];

            // Prepare the buffer: virtio_net_hdr + frame data
            size_t hdr_size = sizeof(virtio_net_hdr);
            if (hdr_size + len <= TX_BUF_SIZE) {
                auto* nethdr = reinterpret_cast<virtio_net_hdr*>(buf.vaddr);
                string::memset(nethdr, 0, hdr_size);
                nethdr->gso_type = VIRTIO_NET_HDR_GSO_NONE;

                string::memcpy(reinterpret_cast<uint8_t*>(buf.vaddr + hdr_size), frame, len);

                int32_t rc = drv->m_txq.add_buf(buf.phys, static_cast<uint32_t>(hdr_size + len), 0);
                if (rc >= 0) {
                    buf.in_use = true;
                    buf.desc_id = static_cast<int16_t>(rc);
                    drv->m_txq.kick(drv->m_tx_notify_addr);
                    result = 0;
                }
            }
        }
    });

    return result;
}

void virtio_net_driver::poll_callback(net::netif* iface) {
    if (!iface) return;
    auto* drv = static_cast<virtio_net_driver*>(iface->driver_data);
    if (!drv) return;

    rx_batch batch;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(drv->m_vq_lock);
        drv->drain_rx_locked(batch);
        drv->process_tx_completions();
    });
    RUN_ELEVATED(drv->deliver_rx_batch(batch));
    RUN_ELEVATED({
        sync::irq_lock_guard guard(drv->m_vq_lock);
        drv->replenish_rx();
    });
}

bool virtio_net_driver::link_callback(net::netif* iface) {
    if (!iface) return false;
    auto* drv = static_cast<virtio_net_driver*>(iface->driver_data);
    if (!drv) return false;

    bool up = true;
    if (drv->m_has_status && drv->m_device_cfg) {
        RUN_ELEVATED(up = (drv->m_device_cfg->status & 1) != 0);
    }
    return up;
}

// PCI driver registration
REGISTER_PCI_DRIVER(virtio_net_driver,
    PCI_MATCH(VIRTIO_VENDOR_ID, PCI_MATCH_ANY, 0x02, 0x00, PCI_MATCH_ANY_8),
    PCI_DRIVER_FACTORY(virtio_net_driver));

} // namespace drivers
