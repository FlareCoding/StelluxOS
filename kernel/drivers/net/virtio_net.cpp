#include "drivers/net/virtio_net.h"
#include "mm/vmm.h"
#include "net/packet.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"
#include "common/logging.h"

namespace drivers {

using namespace virtio;

int32_t virtio_net_driver::read_mac() {
    volatile virtio_net_config* cfg = device_config<virtio_net_config>();
    if (!cfg || !m_has_mac) {
        // Device provides no MAC, use a fixed locally administered address.
        m_mac = {{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};
        return 0;
    }

    for (size_t i = 0; i < net::eth::MAC_ADDR_LEN; i++) {
        m_mac.bytes[i] = cfg->mac[i];
    }

    log::info("virtio-net: MAC %02x:%02x:%02x:%02x:%02x:%02x",
              m_mac.bytes[0], m_mac.bytes[1], m_mac.bytes[2],
              m_mac.bytes[3], m_mac.bytes[4], m_mac.bytes[5]);
    return 0;
}

int32_t virtio_net_driver::init_queues() {
    int32_t rc = setup_queue(VIRTIO_NET_QUEUE_RX, m_rxq, &m_rx_notify_addr);
    if (rc != 0) {
        return rc;
    }

    rc = setup_queue(VIRTIO_NET_QUEUE_TX, m_txq, &m_tx_notify_addr);
    if (rc != 0) {
        return rc;
    }

    log::info("virtio-net: RX queue (%u entries), TX queue (%u entries)",
              m_rxq.size(), m_txq.size());

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
                paging::PAGE_READ | paging::PAGE_WRITE | paging::PAGE_USER | paging::PAGE_DMA,
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
                paging::PAGE_READ | paging::PAGE_WRITE | paging::PAGE_USER | paging::PAGE_DMA,
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

    uint64_t features = 0;
    int32_t rc = start_transport(0, VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS | VIRTIO_F_VERSION_1, &features);
    if (rc != 0) return rc;

    m_has_mac = (features & VIRTIO_NET_F_MAC) != 0;
    m_has_status = (features & VIRTIO_NET_F_STATUS) != 0;
    m_version_1 = (features & VIRTIO_F_VERSION_1) != 0;
    if (m_version_1) {
        m_net_hdr_size = 12; // includes num_buffers field
    }

    // Read MAC address and initialize virtqueues
    rc = read_mac();
    if (rc != 0) return rc;

    rc = init_queues();
    if (rc != 0) {
        set_failed();
        return rc;
    }

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
        route_config_msix(0);
        route_queue_msix(VIRTIO_NET_QUEUE_RX, 0);
        route_queue_msix(VIRTIO_NET_QUEUE_TX, (m_dev->get_msi_state().vector_count > 1) ? 1 : 0);
    }

    set_driver_ok();

    // Post RX buffers after DRIVER_OK, the virtio spec (Section 3.1.1) forbids
    // sending buffer available notifications before DRIVER_OK is set.
    fill_rx_queue();

    // Registered last when nothing can fail
    m_mtu = net::eth::MTU;
    m_enabled = true;
    RUN_ELEVATED(set_link_up(link_up()));

    rc = net::register_interface(this, "eth");
    if (rc != net::OK) {
        log::error("virtio-net: interface registration failed: %d", rc);
        m_enabled = false;
        return rc;
    }

    log::info("virtio-net: DRIVER_OK, device is live");

    return 0;
}

int32_t virtio_net_driver::detach() {
    // The registry may still name this interface, so it refuses traffic from here on
    m_enabled = false;
    RUN_ELEVATED(set_link_up(false));

    reset_device();
    return pci_driver::detach();
}

__PRIVILEGED_CODE void virtio_net_driver::on_interrupt(uint32_t vector) {
    (void)vector;
    ack_interrupt();
}

void virtio_net_driver::drain_rx_locked(rx_batch& batch) {
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

        uintptr_t buf_vaddr = m_rx_bufs[buf_idx].vaddr;
        size_t hdr_size = m_net_hdr_size;

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

// Copies each frame out of its DMA buffer into a packet and hands the packet to
// the stack. Runs without m_vq_lock so the stack may transmit from receive.
void virtio_net_driver::deliver_rx_batch(rx_batch& batch) {
    for (uint16_t i = 0; i < batch.count; i++) {
        rx_batch_entry& entry = batch.entries[i];
        net::packet* pkt = net::packet::alloc();

        uint8_t* dst = nullptr;
        if (pkt && pkt->reserve(net::eth::RX_ALIGN_PAD)) {
            dst = pkt->put(entry.len);
        }
        if (!dst) {
            net::packet::free(pkt);
            record_packet_dropped();

            m_rx_bufs[entry.buf_idx].delivering = false;
            continue;
        }

        string::memcpy(dst, entry.data, entry.len);
        pkt->set_iface(this);

        m_rx_bufs[entry.buf_idx].delivering = false;

        // Hand the packet off to the network stack to handle
        receive(pkt);
    }
}

void virtio_net_driver::replenish_rx() {
    bool posted_any = false;
    for (uint16_t i = 0; i < RX_BUF_COUNT; i++) {
        if (m_rx_bufs[i].desc_id >= 0) continue; // already posted

        if (m_rx_bufs[i].delivering) continue; // being read by deliver_rx_batch

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

        // Drain RX under lock, deliver lowered and without the lock so the
        // stack can transmit from receive, then re-lock to replenish.
        rx_batch batch;
        RUN_ELEVATED({
            sync::irq_lock_guard guard(m_vq_lock);
            drain_rx_locked(batch);
            process_tx_completions();
        });
        deliver_rx_batch(batch);
        RUN_ELEVATED({
            sync::irq_lock_guard guard(m_vq_lock);
            replenish_rx();
        });

        RUN_ELEVATED(set_link_up(link_up()));
    }
}

int32_t virtio_net_driver::transmit(net::packet* pkt) {
    if (!pkt || pkt->length() == 0) {
        return net::ERR_INVALID;
    }

    size_t hdr_size = m_net_hdr_size;
    size_t len = pkt->length();

    if (hdr_size + len > TX_BUF_SIZE) {
        return net::ERR_TOO_LARGE;
    }

    int32_t result = net::ERR_BUSY;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(m_vq_lock);

        // Find a free TX buffer, reclaiming completed ones if none is free
        int32_t buf_idx = -1;
        for (uint16_t i = 0; i < TX_BUF_COUNT; i++) {
            if (!m_tx_bufs[i].in_use) {
                buf_idx = static_cast<int32_t>(i);
                break;
            }
        }

        if (buf_idx < 0) {
            process_tx_completions();
            for (uint16_t i = 0; i < TX_BUF_COUNT; i++) {
                if (!m_tx_bufs[i].in_use) {
                    buf_idx = static_cast<int32_t>(i);
                    break;
                }
            }
        }

        if (buf_idx >= 0) {
            auto& buf = m_tx_bufs[buf_idx];

            auto* nethdr = reinterpret_cast<virtio_net_hdr*>(buf.vaddr);
            nethdr->gso_type = VIRTIO_NET_HDR_GSO_NONE;

            string::memset(nethdr, 0, hdr_size);
            string::memcpy(reinterpret_cast<uint8_t*>(buf.vaddr + hdr_size), pkt->data(), len);

            int32_t rc = m_txq.add_buf(buf.phys, static_cast<uint32_t>(hdr_size + len), 0);
            if (rc >= 0) {
                buf.in_use = true;
                buf.desc_id = static_cast<int16_t>(rc);

                m_txq.kick(m_tx_notify_addr);
                result = net::OK;
            }
        }

        if (result == net::OK) {
            m_counters.frames_out++;
            m_counters.bytes_out += len;
        } else {
            record_packet_dropped();
        }
    });

    return result;
}

bool virtio_net_driver::link_up() {
    volatile virtio_net_config* cfg = device_config<virtio_net_config>();
    bool up = true;
    if (m_has_status && cfg) {
        up = (cfg->status & VIRTIO_NET_S_LINK_UP) != 0;
    }

    return up;
}

// PCI driver registration
REGISTER_PCI_DRIVER(virtio_net_driver,
    PCI_MATCH(VIRTIO_VENDOR_ID, PCI_MATCH_ANY, 0x02, 0x00, PCI_MATCH_ANY_8),
    PCI_DRIVER_FACTORY(virtio_net_driver));

} // namespace drivers
