#include <modules/usb/xhci/xhci_rings.h>
#include <memory/paging.h>
#include <serial/serial.h>

xhci_command_ring::xhci_command_ring(size_t max_trbs) {
    m_max_trb_count = max_trbs;
    m_rcs_bit = XHCI_CRCR_RING_CYCLE_STATE;
    m_enqueue_ptr = 0;

    const uint64_t ring_size = max_trbs * sizeof(xhci_trb_t);

    // Create the command ring memory block
    m_trbs = (xhci_trb_t*)alloc_xhci_memory(
        ring_size,
        XHCI_COMMAND_RING_SEGMENTS_ALIGNMENT,
        XHCI_COMMAND_RING_SEGMENTS_BOUNDARY
    );

    m_physical_base = xhci_get_physical_addr(m_trbs);

    // Set the last TRB as a link TRB to point back to the first TRB
    m_trbs[m_max_trb_count - 1].parameter = m_physical_base;
    m_trbs[m_max_trb_count - 1].control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | m_rcs_bit;
}

void xhci_command_ring::enqueue(xhci_trb_t* trb) {
    // Adjust the TRB's cycle bit to the current RCS
    trb->cycle_bit = m_rcs_bit;

    // Insert the TRB into the ring
    m_trbs[m_enqueue_ptr] = *trb;

    // Advance and possibly wrap the enqueue pointer if needed.
    // maxTrbCount - 1 accounts for the LINK_TRB.
    if (++m_enqueue_ptr == m_max_trb_count - 1) {
        m_enqueue_ptr = 0;
        m_rcs_bit = !m_rcs_bit;
    }
}

xhci_event_ring::xhci_event_ring(
    size_t max_trbs,
    volatile xhci_interrupter_registers* primary_interrupter_registers
) {
    m_interrupter_regs = primary_interrupter_registers;
    m_segment_trb_count = max_trbs;
    m_rcs_bit = XHCI_CRCR_RING_CYCLE_STATE;
    m_dequeue_ptr = 0;

    const uint64_t event_ring_segment_size = max_trbs * sizeof(xhci_trb_t);
    const uint64_t event_ring_segment_table_size = m_segment_count * sizeof(xhci_erst_entry);

    // Create the event ring segment memory block
    m_primary_segment_ring = (xhci_trb_t*)alloc_xhci_memory(
        event_ring_segment_size,
        XHCI_EVENT_RING_SEGMENTS_ALIGNMENT,
        XHCI_EVENT_RING_SEGMENTS_BOUNDARY
    );

    // Store the physical DMA base
    m_primary_segment_ring_physical_base = xhci_get_physical_addr(m_primary_segment_ring);

    // Create the event ring segment table
    m_segment_table = (xhci_erst_entry*)alloc_xhci_memory(
        event_ring_segment_table_size,
        XHCI_EVENT_RING_SEGMENT_TABLE_ALIGNMENT,
        XHCI_EVENT_RING_SEGMENT_TABLE_BOUNDARY
    );

    // Construct the segment table entry
    xhci_erst_entry entry;
    entry.ring_segment_base_address = m_primary_segment_ring_physical_base;
    entry.ring_segment_size = XHCI_EVENT_RING_TRB_COUNT;
    entry.rsvd = 0;

    // Insert the constructed segment into the table
    m_segment_table[0] = entry;

    // Configure the Event Ring Segment Table Size (ERSTSZ) register
    m_interrupter_regs->erstsz = 1;

    // Initialize and set ERDP
    _update_erdp_interrupter_register();

    // Write to ERSTBA register
    m_interrupter_regs->erstba = xhci_get_physical_addr(m_segment_table);
}

bool xhci_event_ring::has_unprocessed_events() {
    return (m_primary_segment_ring[m_dequeue_ptr].cycle_bit == m_rcs_bit);
}

void xhci_event_ring::dequeue_events(kstl::vector<xhci_trb_t*>& received_event_trbs) {
    // Process each event TRB
    while (has_unprocessed_events()) {
        auto trb = _dequeue_trb();
        if (!trb) break;

        received_event_trbs.push_back(trb);
    }

    // Update the ERDP register
    _update_erdp_interrupter_register();

    // Clear the EHB (Event Handler Busy) bit
    m_interrupter_regs->erdp |= XHCI_ERDP_EHB;
}

void xhci_event_ring::flush_unprocessed_events() {
    // Dequeue all unprocessed TRBs
    while (has_unprocessed_events()) {
        _dequeue_trb();
    }

    // Update the ERDP register
    _update_erdp_interrupter_register();

    // Clear the EHB (Event Handler Busy) bit
    m_interrupter_regs->erdp |= XHCI_ERDP_EHB;
}

void xhci_event_ring::_update_erdp_interrupter_register() {
    uint64_t dequeue_address = m_primary_segment_ring_physical_base + (m_dequeue_ptr * sizeof(xhci_trb_t));
    m_interrupter_regs->erdp = dequeue_address;
}

xhci_trb_t* xhci_event_ring::_dequeue_trb() {
    if (m_primary_segment_ring[m_dequeue_ptr].cycle_bit != m_rcs_bit) {
        serial::printf("[XHCI_EVENT_RING] Dequeued an invalid TRB, returning nullptr!\n");
        return nullptr;
    }

    // Get the resulting TRB
    xhci_trb_t* ret = &m_primary_segment_ring[m_dequeue_ptr];

    // Advance and possibly wrap the dequeue pointer if needed
    if (++m_dequeue_ptr == m_segment_trb_count) {
        m_dequeue_ptr = 0;
        m_rcs_bit = !m_rcs_bit;
    }

    return ret;
}

kstl::shared_ptr<xhci_transfer_ring> xhci_transfer_ring::allocate(uint8_t slot_id) {
    return kstl::shared_ptr<xhci_transfer_ring>(
        new xhci_transfer_ring(XHCI_TRANSFER_RING_TRB_COUNT, slot_id)
    );
}

xhci_transfer_ring::xhci_transfer_ring(size_t max_trbs, uint8_t doorbell_id) {
    m_max_trb_count = max_trbs;
    m_rcs_bit = 1;
    m_dequeue_ptr = 0;
    m_enqueue_ptr = 0;
    m_doorbell_id = doorbell_id;

    const uint64_t ring_size = max_trbs * sizeof(xhci_trb_t);

    // Create the transfer ring memory block
    m_trbs = (xhci_trb_t*)alloc_xhci_memory(
        ring_size,
        XHCI_TRANSFER_RING_SEGMENTS_ALIGNMENT,
        XHCI_TRANSFER_RING_SEGMENTS_BOUNDARY
    );

    m_physical_base = xhci_get_physical_addr(m_trbs);

    // Set the last TRB as a link TRB to point back to the first TRB
    m_trbs[m_max_trb_count - 1].parameter = m_physical_base;
    m_trbs[m_max_trb_count - 1].control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | m_rcs_bit;
}

uintptr_t xhci_transfer_ring::get_physical_dequeue_pointer_base() const {
    return xhci_get_physical_addr(&m_trbs[m_enqueue_ptr]);
}

void xhci_transfer_ring::enqueue(xhci_trb_t* trb) {
    // Adjust the TRB's cycle bit to the current DCS
    trb->cycle_bit = m_rcs_bit;

    // Insert the TRB into the ring
    m_trbs[m_enqueue_ptr] = *trb;

    // Advance and possibly wrap the enqueue pointer if needed.
    // maxTrbCount - 1 accounts for the LINK_TRB.
    if (++m_enqueue_ptr == m_max_trb_count - 1) {
        m_enqueue_ptr = 0;
        m_rcs_bit = !m_rcs_bit;
    }
}
