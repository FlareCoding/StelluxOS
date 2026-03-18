#include "drivers/usb/xhci/xhci_rings.h"
#include "common/logging.h"

namespace drivers::xhci {

int32_t xhci_command_ring::init(size_t max_trbs) {
    m_max_trb_count = max_trbs;
    m_rcs_bit = XHCI_CRCR_RING_CYCLE_STATE;
    m_enqueue_ptr = 0;
    m_dequeue_ptr = 0;
    m_consumer_cycle_state = true;

    const size_t ring_size = max_trbs * sizeof(xhci_trb_t);

    // Create the command ring memory block
    m_trbs = static_cast<xhci_trb_t*>(alloc_xhci_memory(ring_size));
    if (!m_trbs) {
        log::error("xhci: failed to allocate command ring (%lu TRBs)", max_trbs);
        return -1;
    }

    m_physical_base = xhci_get_physical_addr(m_trbs);

    // Set the last TRB as a link TRB to point back to the first TRB
    m_trbs[m_max_trb_count - 1].parameter = m_physical_base;
    m_trbs[m_max_trb_count - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_LINK_TRB_TC_BIT | m_rcs_bit;

    return 0;
}

void xhci_command_ring::destroy() {
    if (m_trbs) {
        free_xhci_memory(m_trbs);
        m_trbs = nullptr;
    }
}

bool xhci_command_ring::enqueue(xhci_trb_t* trb) {
    bool can_enqueue = m_consumer_cycle_state == (m_rcs_bit != 0)
        ? m_enqueue_ptr >= m_dequeue_ptr
        : m_enqueue_ptr < m_dequeue_ptr;
    if (!can_enqueue) {
        return false;
    }

    // Adjust the TRB's cycle bit to the current RCS
    trb->cycle_bit = m_rcs_bit;

    // Insert the TRB into the ring
    m_trbs[m_enqueue_ptr] = *trb;

    // Advance and possibly wrap the enqueue pointer if needed.
    // maxTrbCount - 1 accounts for the LINK_TRB.
    if (++m_enqueue_ptr == m_max_trb_count - 1) {
        // Update the Link TRB to reflect the current
        // cycle state including the TC flag.
        m_trbs[m_max_trb_count - 1].control =
            (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_LINK_TRB_TC_BIT | m_rcs_bit;

        m_enqueue_ptr = 0;
        m_rcs_bit = !m_rcs_bit;
    }

    return true;
}

void xhci_command_ring::process_event(xhci_command_completion_trb_t* event) {
    // xHCI 4.9.3 Command Ring Management
    // > The location of the Command Ring Dequeue Pointer is reported
    // > on the Event Ring in Command Completion Events.
    // xHCI 3.3 Command Interface
    // > Commands are executed by the xHC in the order that they are placed on the Command Ring.
    size_t command_index = (event->command_trb_pointer - m_physical_base) / sizeof(xhci_trb_t);
    size_t new_dequeue_ptr = command_index + 1;

    // If the completed command was the last slot before the Link TRB,
    // the xHC has already followed the Link and wrapped to index 0
    // with a toggled consumer cycle state.
    if (new_dequeue_ptr >= m_max_trb_count - 1) {
        new_dequeue_ptr = 0;
        m_consumer_cycle_state = !m_consumer_cycle_state;
    } else if (new_dequeue_ptr < m_dequeue_ptr) {
        // If the consumer (xHC) looped around, it must have toggled
        // its consumer cycle state
        m_consumer_cycle_state = !m_consumer_cycle_state;
    }

    m_dequeue_ptr = new_dequeue_ptr;
}

int32_t xhci_event_ring::init(
    size_t max_trbs,
    volatile xhci_interrupter_registers* primary_interrupter_registers
) {
    m_interrupter_regs = primary_interrupter_registers;
    m_segment_trb_count = max_trbs;
    m_rcs_bit = XHCI_CRCR_RING_CYCLE_STATE;
    m_dequeue_ptr = 0;

    const size_t event_ring_segment_size = max_trbs * sizeof(xhci_trb_t);
    const size_t event_ring_segment_table_size = m_segment_count * sizeof(xhci_erst_entry);

    // Create the event ring segment memory block
    m_primary_segment_ring = static_cast<xhci_trb_t*>(
        alloc_xhci_memory(event_ring_segment_size));
    if (!m_primary_segment_ring) {
        log::error("xhci: failed to allocate event ring segment (%lu TRBs)", max_trbs);
        return -1;
    }

    // Store the physical DMA base
    m_primary_segment_ring_physical_base = xhci_get_physical_addr(m_primary_segment_ring);

    // Create the event ring segment table
    m_segment_table = static_cast<xhci_erst_entry*>(
        alloc_xhci_memory(event_ring_segment_table_size));
    if (!m_segment_table) {
        log::error("xhci: failed to allocate event ring segment table");
        free_xhci_memory(m_primary_segment_ring);
        m_primary_segment_ring = nullptr;
        return -1;
    }

    // Construct the segment table entry
    xhci_erst_entry entry;
    entry.ring_segment_base_address = m_primary_segment_ring_physical_base;
    entry.ring_segment_size = XHCI_EVENT_RING_TRB_COUNT;
    entry.rsvd = 0;

    // Insert the constructed segment into the table
    m_segment_table[0] = entry;

    // xHCI Spec Section 4.9.4: Event Ring initialization order
    // Program ERSTSZ, then ERSTBA, then ERDP.

    // Configure the Event Ring Segment Table Size (ERSTSZ) register
    m_interrupter_regs->erstsz = 1;

    // Write to ERSTBA register (must be before ERDP per spec)
    m_interrupter_regs->erstba = xhci_get_physical_addr(m_segment_table);

    // Initialize and set ERDP
    _update_erdp_interrupter_register();

    return 0;
}

void xhci_event_ring::destroy() {
    if (m_primary_segment_ring) {
        free_xhci_memory(m_primary_segment_ring);
        m_primary_segment_ring = nullptr;
    }
    if (m_segment_table) {
        free_xhci_memory(m_segment_table);
        m_segment_table = nullptr;
    }
}

bool xhci_event_ring::has_unprocessed_events() {
    return (m_primary_segment_ring[m_dequeue_ptr].cycle_bit == m_rcs_bit);
}

xhci_trb_t* xhci_event_ring::dequeue_trb() {
    if (m_primary_segment_ring[m_dequeue_ptr].cycle_bit != m_rcs_bit) {
        log::error("xhci: event ring dequeued an invalid TRB");
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

void xhci_event_ring::finish_processing() {
    // Write the updated dequeue pointer with EHB set (write-1-to-clear)
    // in a single ERDP write per xHCI spec section 5.5.2.3.3
    uint64_t dequeue_address =
        m_primary_segment_ring_physical_base + (m_dequeue_ptr * sizeof(xhci_trb_t));
    m_interrupter_regs->erdp = dequeue_address | XHCI_ERDP_EHB;
}

void xhci_event_ring::flush_unprocessed_events() {
    // Dequeue all unprocessed TRBs
    while (has_unprocessed_events()) {
        dequeue_trb();
    }

    finish_processing();
}

void xhci_event_ring::_update_erdp_interrupter_register() {
    uint64_t dequeue_address =
        m_primary_segment_ring_physical_base + (m_dequeue_ptr * sizeof(xhci_trb_t));
    m_interrupter_regs->erdp = dequeue_address;
}

int32_t xhci_transfer_ring::init(size_t max_trbs, uint8_t doorbell_id) {
    m_max_trb_count = max_trbs;
    m_rcs_bit = 1;
    m_dequeue_ptr = 0;
    m_enqueue_ptr = 0;
    m_doorbell_id = doorbell_id;

    const size_t ring_size = max_trbs * sizeof(xhci_trb_t);

    // Create the transfer ring memory block
    m_trbs = static_cast<xhci_trb_t*>(alloc_xhci_memory(ring_size));
    if (!m_trbs) {
        log::error("xhci: failed to allocate transfer ring (%lu TRBs)", max_trbs);
        return -1;
    }

    m_physical_base = xhci_get_physical_addr(m_trbs);

    // Set the last TRB as a link TRB to point back to the first TRB
    m_trbs[m_max_trb_count - 1].parameter = m_physical_base;
    m_trbs[m_max_trb_count - 1].control =
        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_LINK_TRB_TC_BIT | m_rcs_bit;

    return 0;
}

void xhci_transfer_ring::destroy() {
    if (m_trbs) {
        free_xhci_memory(m_trbs);
        m_trbs = nullptr;
    }
}

uintptr_t xhci_transfer_ring::get_physical_dequeue_pointer_base() const {
    return m_physical_base + m_enqueue_ptr * sizeof(xhci_trb_t);
}

void xhci_transfer_ring::enqueue(xhci_trb_t* trb) {
    // Adjust the TRB's cycle bit to the current DCS
    trb->cycle_bit = m_rcs_bit;

    // Insert the TRB into the ring
    m_trbs[m_enqueue_ptr] = *trb;

    // Advance and possibly wrap the enqueue pointer if needed.
    // maxTrbCount - 1 accounts for the LINK_TRB.
    if (++m_enqueue_ptr == m_max_trb_count - 1) {
        // Only now update the Link TRB, syncing its cycle bit and setting the TC flag.
        m_trbs[m_max_trb_count - 1].control =
            (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_LINK_TRB_TC_BIT | m_rcs_bit;

        m_enqueue_ptr = 0;
        m_rcs_bit = !m_rcs_bit;
    }
}

} // namespace drivers::xhci
