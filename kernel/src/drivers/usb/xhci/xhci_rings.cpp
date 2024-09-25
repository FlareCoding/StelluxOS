#include "xhci_rings.h"
#include <kprint.h>

XhciCommandRing::XhciCommandRing(size_t maxTrbs) {
    m_maxTrbCount = maxTrbs;
    m_rcsBit = XHCI_CRCR_RING_CYCLE_STATE;
    m_enqueuePtr = 0;

    const uint64_t ringSize = maxTrbs * sizeof(XhciTrb_t);

    // Create the command ring memory block
    m_trbs = (XhciTrb_t*)allocXhciMemory(
        ringSize,
        XHCI_COMMAND_RING_SEGMENTS_ALIGNMENT,
        XHCI_COMMAND_RING_SEGMENTS_BOUNDARY
    );

    m_physicalBase = physbase(m_trbs);

    // Set the last TRB as a link TRB to point back to the first TRB
    m_trbs[m_maxTrbCount - 1].parameter = m_physicalBase;
    m_trbs[m_maxTrbCount - 1].control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | m_rcsBit;
}

void XhciCommandRing::enqueue(XhciTrb_t* trb) {
    // Adjust the TRB's cycle bit to the current RCS
    trb->cycleBit = m_rcsBit;

    // Insert the TRB into the ring
    m_trbs[m_enqueuePtr] = *trb;

    // Advance and possibly wrap the enqueue pointer if needed.
    // maxTrbCount - 1 accounts for the LINK_TRB.
    if (++m_enqueuePtr == m_maxTrbCount - 1) {
        m_enqueuePtr = 0;
        m_rcsBit = !m_rcsBit;
    }
}

XhciEventRing::XhciEventRing(
    size_t maxTrbs,
    volatile XhciInterrupterRegisters* primaryInterrupterRegisters
) {
    m_interrupterRegs = primaryInterrupterRegisters;
    m_segmentTrbCount = maxTrbs;
    m_rcsBit = XHCI_CRCR_RING_CYCLE_STATE;
    m_dequeuePtr = 0;

    const uint64_t eventRingSegmentSize = maxTrbs * sizeof(XhciTrb_t);
    const uint64_t eventRingSegmentTableSize = m_segmentCount * sizeof(XhciErstEntry);

    // Create the event ring segment memory block
    m_primarySegmentRing = (XhciTrb_t*)allocXhciMemory(
        eventRingSegmentSize,
        XHCI_EVENT_RING_SEGMENTS_ALIGNMENT,
        XHCI_EVENT_RING_SEGMENTS_BOUNDARY
    );

    // Store the physical DMA base
    m_primarySegmentRingPhysicalBase = physbase(m_primarySegmentRing);

    // Create the event ring segment table
    m_segmentTable = (XhciErstEntry*)allocXhciMemory(
        eventRingSegmentTableSize,
        XHCI_EVENT_RING_SEGMENT_TABLE_ALIGNMENT,
        XHCI_EVENT_RING_SEGMENT_TABLE_BOUNDARY
    );

    // Construct the segment table entry
    XhciErstEntry entry;
    entry.ringSegmentBaseAddress = m_primarySegmentRingPhysicalBase;
    entry.ringSegmentSize = XHCI_EVENT_RING_TRB_COUNT;
    entry.rsvd = 0;

    // Insert the constructed segment into the table
    m_segmentTable[0] = entry;

    // Configure the Event Ring Segment Table Size (ERSTSZ) register
    m_interrupterRegs->erstsz = 1;

    // Initialize and set ERDP
    _updateErdpInterrupterRegister();

    // Write to ERSTBA register
    m_interrupterRegs->erstba = physbase(m_segmentTable);
}

bool XhciEventRing::hasUnprocessedEvents() {
    return (m_primarySegmentRing[m_dequeuePtr].cycleBit == m_rcsBit);
}

void XhciEventRing::dequeueEvents(kstl::vector<XhciTrb_t*>& receivedEventTrbs) {
    // Process each event TRB
    while (hasUnprocessedEvents()) {
        auto trb = _dequeueTrb();
        if (!trb) break;

        receivedEventTrbs.pushBack(trb);
    }

    // Update the ERDP register
    _updateErdpInterrupterRegister();

    // Clear the EHB (Event Handler Busy) bit
    m_interrupterRegs->erdp |= XHCI_ERDP_EHB;
}

void XhciEventRing::flushUnprocessedEvents() {
    // Dequeue all unprocessed TRBs
    while (hasUnprocessedEvents()) {
        _dequeueTrb();
    }

    // Update the ERDP register
    _updateErdpInterrupterRegister();

    // Clear the EHB (Event Handler Busy) bit
    m_interrupterRegs->erdp |= XHCI_ERDP_EHB;
}

void XhciEventRing::_updateErdpInterrupterRegister() {
    uint64_t dequeueAddress = m_primarySegmentRingPhysicalBase + (m_dequeuePtr * sizeof(XhciTrb_t));
    m_interrupterRegs->erdp = dequeueAddress;
}

XhciTrb_t* XhciEventRing::_dequeueTrb() {
    if (m_primarySegmentRing[m_dequeuePtr].cycleBit != m_rcsBit) {
        kprint("[XHCI_EVENT_RING] Dequeued an invalid TRB, returning NULL!\n");
        return nullptr;
    }

    // Get the resulting TRB
    XhciTrb_t* ret = &m_primarySegmentRing[m_dequeuePtr];

    // Advance and possibly wrap the dequeue pointer if needed
    if (++m_dequeuePtr == m_segmentTrbCount) {
        m_dequeuePtr = 0;
        m_rcsBit = !m_rcsBit;
    }

    return ret;
}

kstl::SharedPtr<XhciTransferRing> XhciTransferRing::allocate(uint8_t slotId) {
    return kstl::SharedPtr<XhciTransferRing>(
        new XhciTransferRing(XHCI_TRANSFER_RING_TRB_COUNT, slotId)
    );
}

XhciTransferRing::XhciTransferRing(size_t maxTrbs, uint8_t doorbellId) {
    m_maxTrbCount = maxTrbs;
    m_rcsBit = 1;
    m_dequeuePtr = 0;
    m_enqueuePtr = 0;
    m_doorbellId = doorbellId;

    const uint64_t ringSize = maxTrbs * sizeof(XhciTrb_t);

    // Create the transfer ring memory block
    m_trbs = (XhciTrb_t*)allocXhciMemory(
        ringSize,
        XHCI_TRANSFER_RING_SEGMENTS_ALIGNMENT,
        XHCI_TRANSFER_RING_SEGMENTS_BOUNDARY
    );

    m_physicalBase = physbase(m_trbs);

    // Set the last TRB as a link TRB to point back to the first TRB
    m_trbs[m_maxTrbCount - 1].parameter = physbase(m_trbs);
    m_trbs[m_maxTrbCount - 1].control = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | m_rcsBit;
}

void XhciTransferRing::enqueue(XhciTrb_t* trb) {
    // Adjust the TRB's cycle bit to the current DCS
    trb->cycleBit = m_rcsBit;

    // Insert the TRB into the ring
    m_trbs[m_enqueuePtr] = *trb;

    // Advance and possibly wrap the enqueue pointer if needed.
    // maxTrbCount - 1 accounts for the LINK_TRB.
    if (++m_enqueuePtr == m_maxTrbCount - 1) {
        m_enqueuePtr = 0;
        m_rcsBit = !m_rcsBit;
    }
}
