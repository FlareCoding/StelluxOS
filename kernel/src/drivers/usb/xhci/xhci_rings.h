#ifndef XHCI_RINGS_H
#define XHCI_RINGS_H

#include "xhci_mem.h"
#include "xhci_trb.h"
#include "xhci_regs.h"
#include <kvector.h>

class XhciCommandRing {
public:
    XhciCommandRing(size_t maxTrbs);

    inline XhciTrb_t* getVirtualBase() const { return m_trbs; }
    inline physaddr_t getPhysicalBase() const { return m_physicalBase; }
    inline uint8_t  getCycleBit() const { return m_rcsBit; }

    void enqueue(XhciTrb_t* trb);

private:
    size_t              m_maxTrbCount;      // Number of valid TRBs in the ring including the LINK_TRB
    size_t              m_enqueuePtr;       // Index in the ring where to enqueue next TRB
    XhciTrb_t*          m_trbs;             // Base address of the ring buffer
    physaddr_t          m_physicalBase;     // Physical base of the ring
    uint8_t             m_rcsBit;           // Ring cycle state
};

/*
// xHci Spec Section 6.5 Event Ring Segment Table Figure 6-40: Event Ring Segment Table Entry

Note: The Ring Segment Size may be set to any value from 16 to 4096, however
software shall allocate a buffer for the Event Ring Segment that rounds up its
size to the nearest 64B boundary to allow full cache-line accesses.
*/
struct XhciErstEntry {
    uint64_t ringSegmentBaseAddress;  // Base address of the Event Ring segment
    uint32_t ringSegmentSize;         // Size of the Event Ring segment (only low 16 bits are used)
    uint32_t rsvd;
} __attribute__((packed));

class XhciEventRing {
public:
    XhciEventRing(
        size_t maxTrbs,
        volatile XhciInterrupterRegisters* primaryInterrupterRegisters
    );

    inline XhciTrb_t* getVirtualBase() const { return m_primarySegmentRing; }
    inline physaddr_t getPhysicalBase() const { return m_primarySegmentRingPhysicalBase; }
    inline uint8_t  getCycleBit() const { return m_rcsBit; }

    bool hasUnprocessedEvents();
    void dequeueEvents(kstl::vector<XhciTrb_t*>& receivedEventTrbs);

    void flushUnprocessedEvents();

private:
    volatile XhciInterrupterRegisters* m_interrupterRegs;

    size_t                  m_segmentTrbCount;      // Max TRBs allowed on the segment

    XhciTrb_t*              m_primarySegmentRing;   // Primary segment ring base
    physaddr_t              m_primarySegmentRingPhysicalBase;

    XhciErstEntry*          m_segmentTable;         // Event ring segment table base


    const uint64_t m_segmentCount = 1;          // Number of segments to be allocated in the segment table
    uint64_t       m_dequeuePtr;                // Event ring dequeue pointer
    uint8_t        m_rcsBit;                    // Ring cycle state

private:
    void _updateErdpInterrupterRegister();
    XhciTrb_t* _dequeueTrb();
};

class XhciTransferRing {
public:
    static kstl::SharedPtr<XhciTransferRing> allocate(uint8_t slotId);

    XhciTransferRing(size_t maxTrbs, uint8_t doorbellId);
    ~XhciTransferRing() = default;

    inline XhciTrb_t* getVirtualBase() const { return m_trbs; }
    inline physaddr_t getPhysicalBase() const { return m_physicalBase; }
    inline uint8_t  getCycleBit() const { return m_rcsBit; }
    inline uint8_t getDoorbellId() const { return m_doorbellId; }

    void enqueue(XhciTrb_t* trb);

private:
    size_t              m_maxTrbCount;  // Number of valid TRBs in the ring including the LINK_TRB
    size_t              m_dequeuePtr;   // Transfer ring consumer dequeue pointer
    size_t              m_enqueuePtr;   // Transfer ring producer enqueue pointer
    XhciTrb_t*          m_trbs;         // Base address of the ring buffer
    physaddr_t          m_physicalBase;
    uint8_t             m_rcsBit;       // Dequeue cycle state
    uint8_t             m_doorbellId;   // ID of the doorbell associated with the ring
};

#endif
