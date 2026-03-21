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
    m_async_state = heap::ualloc_new<endpoint_async_state>();
    if (!m_async_state) {
        log::error("xhci: failed to allocate async endpoint state for EP %u", endpoint_num());
        return -1;
    }
    m_async_state->active_request = nullptr;
    m_async_state->pending_head = nullptr;
    m_async_state->pending_tail = nullptr;
    m_async_state->async_enabled = false;
    m_async_state->disconnecting = false;
    m_async_state->active_request_cancelled = false;
    m_async_state->interrupt_in_stream.active = false;
    m_async_state->interrupt_in_stream.closing = false;
    m_async_state->interrupt_in_stream.payload_length = 0;
    m_async_state->interrupt_in_stream.queue_depth = 0;
    m_async_state->interrupt_in_stream.payloads = nullptr;
    m_async_state->interrupt_in_stream.payload_storage = nullptr;
    m_async_state->interrupt_in_stream.head = 0;
    m_async_state->interrupt_in_stream.count = 0;
    m_async_state->interrupt_in_stream.next_seq = 1;
    m_async_state->interrupt_in_stream.dropped = 0;
    m_async_state->interrupt_in_stream.available_wq.init();

    m_ring = heap::ualloc_new<xhci_transfer_ring>();
    if (!m_ring) {
        log::error("xhci: failed to allocate transfer ring for EP %u", endpoint_num());
        heap::ufree_delete(m_async_state);
        m_async_state = nullptr;
        return -1;
    }

    if (m_ring->init(XHCI_TRANSFER_RING_TRB_COUNT, slot_id) != 0) {
        log::error("xhci: failed to init transfer ring for EP %u", endpoint_num());
        heap::ufree_delete(m_ring);
        m_ring = nullptr;
        heap::ufree_delete(m_async_state);
        m_async_state = nullptr;
        return -1;
    }

    m_dma_buffer = alloc_xhci_memory(paging::PAGE_SIZE_4KB);
    if (!m_dma_buffer) {
        log::error("xhci: failed to allocate DMA buffer for EP %u", endpoint_num());
        m_ring->destroy();
        heap::ufree_delete(m_ring);
        m_ring = nullptr;
        heap::ufree_delete(m_async_state);
        m_async_state = nullptr;
        return -1;
    }
    m_dma_buffer_phys = xhci_get_physical_addr(m_dma_buffer);

    return 0;
}

void xhci_endpoint::destroy() {
    if (m_async_state) {
        if (m_async_state->interrupt_in_stream.payload_storage) {
            heap::ufree(m_async_state->interrupt_in_stream.payload_storage);
            m_async_state->interrupt_in_stream.payload_storage = nullptr;
        }
        if (m_async_state->interrupt_in_stream.payloads) {
            heap::ufree(m_async_state->interrupt_in_stream.payloads);
            m_async_state->interrupt_in_stream.payloads = nullptr;
        }
        heap::ufree_delete(m_async_state);
        m_async_state = nullptr;
    }
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

endpoint_async_state* xhci_endpoint::ensure_async_state() {
    return m_async_state;
}

} // namespace drivers::xhci
