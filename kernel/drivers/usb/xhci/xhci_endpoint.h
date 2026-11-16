#ifndef STELLUX_DRIVERS_USB_XHCI_XHCI_ENDPOINT_H
#define STELLUX_DRIVERS_USB_XHCI_XHCI_ENDPOINT_H

#include "xhci_rings.h"
#include "xhci_trb.h"
#include "drivers/usb/core/usb_transfer.h"
#include "drivers/usb/usb_descriptors.h"
#include "sync/spinlock.h"
#include "sync/wait_queue.h"

namespace drivers::xhci {

struct in_stream_payload {
    uint32_t seq = 0;
    uint16_t mfindex = 0xffff;
    uint16_t len = 0;
    uint64_t queued_t_us = 0;
    uint8_t* data = nullptr;
};

struct in_stream_state {
    bool active = false;
    bool closing = false;
    uint32_t payload_length = 0;
    uint8_t queue_depth = 0;
    in_stream_payload* payloads = nullptr;
    uint8_t* payload_storage = nullptr;
    uint8_t head = 0;
    uint8_t count = 0;
    uint32_t next_seq = 1;
    uint32_t dropped = 0;
    sync::wait_queue available_wq;

    // Called with each read in place of the payload queue, and kept until the stream's last call returns
    usb::in_stream_callback on_payload = nullptr;
    void*                   context    = nullptr;

    // How many reads a stream with a callback keeps posted, the TRB each was last posted at, and a bit per posted read
    uint32_t read_count = 0;
    uint64_t read_trbs[usb::MAX_IN_STREAM_READS] = {};
    uint64_t posted_reads = 0;
};

struct endpoint_async_state {
    bool async_enabled = false;
    usb::transfer_request* active_request = nullptr;
    usb::transfer_request* pending_head = nullptr;
    usb::transfer_request* pending_tail = nullptr;
    bool disconnecting = false;
    bool active_request_cancelled = false;

    // Set from a cancel or a stream's close until its stop or an unplug clears it, keeping requests off the ring
    bool stopping = false;

    // Set when the ring may hold transfers no one tracks, after a failed stop or move or a stream's failed read
    bool stop_failed = false;

    in_stream_state in_stream = {};
};

class xhci_endpoint {
public:
    xhci_endpoint() = default;
    int32_t init(uint8_t slot_id, const usb::usb_endpoint_descriptor* desc);
    void destroy();

    // USB endpoint properties
    inline uint8_t  endpoint_addr() const { return m_endpoint_addr; }
    inline uint8_t  endpoint_num() const { return m_endpoint_addr & 0x0F; }
    inline bool     is_in() const { return (m_endpoint_addr & 0x80) != 0; }
    inline uint8_t  transfer_type() const { return m_attributes & 0x03; }
    inline uint16_t max_packet_size() const { return m_max_packet_size; }
    inline uint8_t  interval() const { return m_interval; }

    // xHCI properties
    inline uint8_t  dci() const { return m_dci; }
    inline uint8_t  xhc_ep_type() const { return m_xhc_ep_type; }

    // Transfer ring
    inline xhci_transfer_ring* ring() { return m_ring; }

    // Persistent DMA buffer for transfers
    inline void*     dma_buffer() { return m_dma_buffer; }
    inline uintptr_t dma_buffer_phys() const { return m_dma_buffer_phys; }

    // The buffers of a stream with a callback's reads, one every stride bytes
    inline uint8_t*  stream_buffer(uint32_t index) { return m_stream_buffers + index * m_stream_buffer_stride; }
    inline uintptr_t stream_buffer_phys(uint32_t index) const {
        return m_stream_buffers_phys + index * m_stream_buffer_stride;
    }

    // Installs `buffers` as the stream buffers, `stride` bytes apart, and gives back the buffers they replace
    uint8_t* install_stream_buffers(uint8_t* buffers, uintptr_t buffers_phys, uint32_t stride);

    // The power of two from 1 to a page that holds `length`, so no read's buffer crosses a page
    static uint32_t stream_buffer_stride(uint32_t length);

    // Completion state accessors (used by xhci_hcd for event dispatch)
    inline sync::spinlock&   completion_lock() { return m_completion_lock; }
    inline sync::wait_queue& completion_wq() { return m_completion_wq; }
    inline bool              completed() const { return m_completed; }
    inline bool*             completed_ptr() { return &m_completed; }
    inline void              set_completed(bool v) { m_completed = v; }
    inline xhci_transfer_completion_trb_t& result() { return m_result; }
    inline endpoint_async_state* async_state() { return m_async_state; }
    endpoint_async_state* ensure_async_state();

private:
    uint8_t   m_endpoint_addr = 0;
    uint8_t   m_attributes = 0;
    uint16_t  m_max_packet_size = 0;
    uint8_t   m_interval = 0;
    uint8_t   m_dci = 0;             // Device Context Index (1-31)
    uint8_t   m_xhc_ep_type = 0;     // xHCI endpoint type for context programming

    xhci_transfer_ring* m_ring = nullptr;

    void*     m_dma_buffer = nullptr;
    uintptr_t m_dma_buffer_phys = 0;
    uint8_t*  m_stream_buffers = nullptr;
    uintptr_t m_stream_buffers_phys = 0;
    uint32_t  m_stream_buffer_stride = 0;

    sync::wait_queue m_completion_wq;
    sync::spinlock   m_completion_lock;
    bool             m_completed = false;
    xhci_transfer_completion_trb_t m_result = {};
    endpoint_async_state* m_async_state = nullptr;
};

} // namespace drivers::xhci

#endif // STELLUX_DRIVERS_USB_XHCI_XHCI_ENDPOINT_H
