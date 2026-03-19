#ifndef STELLUX_DRIVERS_USB_XHCI_XHCI_RINGS_H
#define STELLUX_DRIVERS_USB_XHCI_XHCI_RINGS_H

#include "xhci_mem.h"
#include "xhci_trb.h"
#include "xhci_regs.h"

namespace drivers::xhci {

class xhci_command_ring {
public:
    xhci_command_ring() = default;
    int32_t init(size_t max_trbs);
    void destroy();

    inline xhci_trb_t* get_virtual_base() const { return m_trbs; }
    inline uintptr_t get_physical_base() const { return m_physical_base; }
    inline uint8_t  get_cycle_bit() const { return m_rcs_bit; }

    bool enqueue(xhci_trb_t* trb);
    void process_event(xhci_command_completion_trb_t* event);

private:
    size_t              m_max_trb_count = 0;    // Number of valid TRBs in the ring including the LINK_TRB
    size_t              m_enqueue_ptr = 0;      // Index in the ring where to enqueue next TRB
    xhci_trb_t*         m_trbs = nullptr;       // Base address of the ring buffer
    uintptr_t           m_physical_base = 0;    // Physical base of the ring
    uint8_t             m_rcs_bit = 0;          // Ring cycle state
    size_t              m_dequeue_ptr = 0;      // The xHC's position as it reads the ring
    bool                m_consumer_cycle_state = false; // The consumer (xHC)'s ring cycle state
};

/*
// xHci Spec Section 6.5 Event Ring Segment Table Figure 6-40: Event Ring Segment Table Entry

Note: The Ring Segment Size may be set to any value from 16 to 4096, however
software shall allocate a buffer for the Event Ring Segment that rounds up its
size to the nearest 64B boundary to allow full cache-line accesses.
*/
struct xhci_erst_entry {
    uint64_t ring_segment_base_address; // Base address of the Event Ring segment
    uint32_t ring_segment_size;         // Size of the Event Ring segment (only low 16 bits are used)
    uint32_t rsvd;
} __attribute__((packed));

class xhci_event_ring {
public:
    xhci_event_ring() = default;
    int32_t init(
        size_t max_trbs,
        volatile xhci_interrupter_registers* primary_interrupter_registers
    );
    void destroy();

    inline xhci_trb_t* get_virtual_base() const { return m_primary_segment_ring; }
    inline uintptr_t get_physical_base() const { return m_primary_segment_ring_physical_base; }
    inline uint8_t  get_cycle_bit() const { return m_rcs_bit; }

    bool has_unprocessed_events();
    xhci_trb_t* dequeue_trb();
    void finish_processing();

    void flush_unprocessed_events();

private:
    volatile xhci_interrupter_registers* m_interrupter_regs = nullptr;

    size_t              m_segment_trb_count = 0;                     // Max TRBs allowed on the segment

    xhci_trb_t*         m_primary_segment_ring = nullptr;            // Primary segment ring base
    uintptr_t           m_primary_segment_ring_physical_base = 0;    // Physical base of the primary segment ring

    xhci_erst_entry*    m_segment_table = nullptr;                   // Event ring segment table base

    const uint64_t      m_segment_count = 1;                         // Number of segments to be allocated in the segment table
    uint64_t            m_dequeue_ptr = 0;                           // Event ring dequeue pointer
    uint8_t             m_rcs_bit = 0;                               // Ring cycle state

private:
    void _update_erdp_interrupter_register();
};

class xhci_transfer_ring {
public:
    xhci_transfer_ring() = default;
    int32_t init(size_t max_trbs, uint8_t doorbell_id);
    void destroy();

    inline xhci_trb_t* get_virtual_base() const { return m_trbs; }
    inline uintptr_t get_physical_base() const { return m_physical_base; }
    inline uint8_t  get_cycle_bit() const { return m_rcs_bit; }
    inline uint8_t get_doorbell_id() const { return m_doorbell_id; }
    
    uintptr_t get_enqueue_phys() const;
    bool can_enqueue(size_t n) const;

    void enqueue(xhci_trb_t* trb);

private:
    size_t              m_max_trb_count = 0; // Number of valid TRBs in the ring including the LINK_TRB
    size_t              m_dequeue_ptr = 0;   // Transfer ring consumer dequeue pointer
    size_t              m_enqueue_ptr = 0;   // Transfer ring producer enqueue pointer
    xhci_trb_t*         m_trbs = nullptr;    // Base address of the ring buffer
    uintptr_t           m_physical_base = 0;
    uint8_t             m_rcs_bit = 0;       // Dequeue cycle state
    uint8_t             m_doorbell_id = 0;   // ID of the doorbell associated with the ring
};

} // namespace drivers::xhci

#endif // STELLUX_DRIVERS_USB_XHCI_XHCI_RINGS_H
