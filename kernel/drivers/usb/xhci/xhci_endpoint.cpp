#include "drivers/usb/xhci/xhci_endpoint.h"
#include "drivers/usb/xhci/xhci_common.h"
#include "drivers/usb/xhci/xhci_mem.h"
#include "common/logging.h"
#include "mm/heap.h"
#include "mm/paging_types.h"

namespace drivers::xhci {

// Convert USB endpoint descriptor attributes to xHCI endpoint type
static uint8_t usb_to_xhci_ep_type(uint8_t usb_type, bool is_in) {
    switch (usb_type) {
    case 0: return XHCI_ENDPOINT_TYPE_CONTROL;
    case 1: return is_in ? XHCI_ENDPOINT_TYPE_ISOCHRONOUS_IN : XHCI_ENDPOINT_TYPE_ISOCHRONOUS_OUT;
    case 2: return is_in ? XHCI_ENDPOINT_TYPE_BULK_IN : XHCI_ENDPOINT_TYPE_BULK_OUT;
    case 3: return is_in ? XHCI_ENDPOINT_TYPE_INTERRUPT_IN : XHCI_ENDPOINT_TYPE_INTERRUPT_OUT;
    default: return XHCI_ENDPOINT_TYPE_INVALID;
    }
}

// Compute the xHCI Device Context Index from the USB endpoint address
// DCI = (endpoint_number * 2) + direction (0=OUT, 1=IN)
static uint8_t compute_dci(uint8_t endpoint_addr) {
    uint8_t ep_num = endpoint_addr & 0x0F;
    bool is_in = (endpoint_addr & 0x80) != 0;
    return static_cast<uint8_t>(ep_num * 2 + (is_in ? 1 : 0));
}

int32_t xhci_endpoint::init(uint8_t slot_id, const usb::usb_endpoint_descriptor* desc) {
    m_endpoint_addr = desc->bEndpointAddress;
    m_attributes = desc->bmAttributes;
    m_max_packet_size = desc->wMaxPacketSize;
    m_interval = desc->bInterval;
    m_dci = compute_dci(m_endpoint_addr);
    m_xhc_ep_type = usb_to_xhci_ep_type(transfer_type(), is_in());

    m_completion_wq.init();
    m_completion_lock = sync::SPINLOCK_INIT;

    m_ring = heap::ualloc_new<xhci_transfer_ring>();
    if (!m_ring) {
        log::error("xhci: failed to allocate transfer ring for EP %u", endpoint_num());
        return -1;
    }

    if (m_ring->init(XHCI_TRANSFER_RING_TRB_COUNT, slot_id) != 0) {
        log::error("xhci: failed to init transfer ring for EP %u", endpoint_num());
        heap::ufree_delete(m_ring);
        m_ring = nullptr;
        return -1;
    }

    m_dma_buffer = alloc_xhci_memory(paging::PAGE_SIZE_4KB);
    if (!m_dma_buffer) {
        log::error("xhci: failed to allocate DMA buffer for EP %u", endpoint_num());
        m_ring->destroy();
        heap::ufree_delete(m_ring);
        m_ring = nullptr;
        return -1;
    }
    m_dma_buffer_phys = xhci_get_physical_addr(m_dma_buffer);

    return 0;
}

void xhci_endpoint::destroy() {
    if (m_dma_buffer) {
        free_xhci_memory(m_dma_buffer);
        m_dma_buffer = nullptr;
        m_dma_buffer_phys = 0;
    }
    if (m_ring) {
        m_ring->destroy();
        heap::ufree_delete(m_ring);
        m_ring = nullptr;
    }
}

} // namespace drivers::xhci
