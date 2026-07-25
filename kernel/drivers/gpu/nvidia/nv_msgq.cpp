#include "drivers/gpu/nvidia/nv_msgq.h"
#include "hw/barrier.h"

// Faithful port of src/common/shared/msgq/msgq.c (CPU/RM path: no callbacks).
// Where the RM registers fcnFlush/fcnBarrier we issue barrier::dma_full() so the
// shared headers/entries are visible to the GSP (x86 is cache-coherent for DMA).
namespace nvidia {

static inline uint32_t align_up_u32(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// Volatile byte copy of a 32-byte header to/from shared GSP memory. Volatile
// keeps the compiler from turning this into a (global, unprovided) memcpy and is
// correct for the GSP-shared headers.
static inline void copy32(volatile void* dst, const volatile void* src) {
    volatile uint8_t* d = static_cast<volatile uint8_t*>(dst);
    const volatile uint8_t* s = static_cast<const volatile uint8_t*>(src);
    for (uint32_t i = 0; i < sizeof(msgqTxHeader); i++) {
        d[i] = s[i];
    }
}

int32_t msgq_tx_create(msgq& q, void* pBackingStore, uint32_t size, uint32_t msgSize,
                       uint32_t hdrAlignShift, uint32_t entryAlignShift, uint32_t flags) {
    if (pBackingStore == nullptr || q.tx_linked) return MSGQ_ERR;
    if (msgSize < MSGQ_MSG_SIZE_MIN || msgSize > size) return MSGQ_ERR;

    q.tx.rxHdrOff = align_up_u32(sizeof(msgqTxHeader), 1u << hdrAlignShift);
    q.tx.entryOff = align_up_u32(q.tx.rxHdrOff + sizeof(msgqRxHeader), 1u << entryAlignShift);
    if (size < q.tx.entryOff + msgSize) return MSGQ_ERR;

    q.tx.version  = MSGQ_VERSION;
    q.tx.size     = size;
    q.tx.msgSize  = msgSize;
    q.tx.writePtr = 0;
    q.tx.flags    = flags;
    q.tx.msgCount = (size - q.tx.entryOff) / msgSize;

    q.our_tx_hdr  = static_cast<msgqTxHeader*>(pBackingStore);
    q.our_rx_hdr  = reinterpret_cast<msgqRxHeader*>(static_cast<uint8_t*>(pBackingStore) + q.tx.rxHdrOff);
    q.our_entries = static_cast<uint8_t*>(pBackingStore) + q.tx.entryOff;
    q.tx_linked   = true;
    q.rx_avail    = 0;
    q.tx_free     = q.tx.msgCount - 1; // allow queueing before rx is linked

    q.rx_swapped     = (flags & MSGQ_FLAGS_SWAP_RX) && (q.rx.flags & MSGQ_FLAGS_SWAP_RX);
    q.p_write_outgoing = &q.our_tx_hdr->writePtr;
    if (q.rx_swapped) {
        q.p_read_outgoing = &q.our_rx_hdr->readPtr;
        q.p_read_incoming = &q.their_rx_hdr->readPtr;
    } else {
        q.p_read_incoming = &q.our_rx_hdr->readPtr;
    }

    // Publish our header into the shared buffer.
    copy32(q.our_tx_hdr, &q.tx);
    barrier::dma_full();
    return MSGQ_OK;
}

int32_t msgq_rx_link(msgq& q, const void* pBackingStore, uint32_t size, uint32_t msgSize) {
    if (pBackingStore == nullptr || q.rx_linked) return MSGQ_ERR;
    if (msgSize < MSGQ_MSG_SIZE_MIN || msgSize > size) return MSGQ_ERR;

    q.their_tx_hdr = static_cast<const msgqTxHeader*>(pBackingStore);
    copy32(&q.rx, q.their_tx_hdr); // snapshot their header

    if (size < q.rx.entryOff + msgSize) return MSGQ_ERR;
    if (q.rx.size != size || q.rx.msgSize != msgSize || q.rx.version != MSGQ_VERSION) return MSGQ_ERR;
    if (q.rx.rxHdrOff < sizeof(msgqTxHeader) ||
        q.rx.entryOff < q.tx.rxHdrOff + sizeof(msgqRxHeader) ||
        q.rx.msgCount != (size - q.rx.entryOff) / msgSize) {
        return MSGQ_ERR;
    }

    q.their_rx_hdr   = reinterpret_cast<const msgqRxHeader*>(static_cast<const uint8_t*>(pBackingStore) + q.rx.rxHdrOff);
    q.their_entries  = static_cast<const uint8_t*>(pBackingStore) + q.rx.entryOff;
    q.rx_linked      = true;
    q.rx_swapped     = (q.tx.flags & MSGQ_FLAGS_SWAP_RX) && (q.rx.flags & MSGQ_FLAGS_SWAP_RX);
    q.p_write_incoming = &q.their_tx_hdr->writePtr;
    if (q.rx_swapped) {
        q.p_read_outgoing = &q.our_rx_hdr->readPtr;
        q.p_read_incoming = &q.their_rx_hdr->readPtr;
    } else {
        q.p_read_outgoing = const_cast<uint32_t*>(&q.their_rx_hdr->readPtr);
        if (q.tx_linked) q.p_read_incoming = &q.our_rx_hdr->readPtr;
    }

    q.rx_read_ptr = 0;
    *q.p_read_outgoing = q.rx_read_ptr;
    barrier::dma_full();
    return MSGQ_OK;
}

uint32_t msgq_tx_get_free_space(msgq& q) {
    if (!q.tx_linked) return 0;
    const uint32_t readPtr = *q.p_read_incoming;
    if (readPtr >= q.tx.msgCount) return 0;
    uint32_t freeSlots = readPtr + q.tx.msgCount - q.tx.writePtr - 1;
    if (freeSlots >= q.tx.msgCount) freeSlots -= q.tx.msgCount;
    q.tx_free = freeSlots;
    return freeSlots;
}

void* msgq_tx_get_write_buffer(msgq& q, uint32_t n) {
    if (!q.tx_linked) return nullptr;
    if (n >= q.tx_free && n >= msgq_tx_get_free_space(q)) return nullptr;
    uint32_t wp = q.tx.writePtr + n;
    if (wp >= q.tx.msgCount) wp -= q.tx.msgCount;
    return q.our_entries + (wp * q.tx.msgSize);
}

int32_t msgq_tx_submit_buffers(msgq& q, uint32_t n) {
    if (!q.tx_linked) return MSGQ_ERR;
    if (n > q.tx_free && n > msgq_tx_get_free_space(q)) return MSGQ_ERR;

    barrier::dma_full(); // flush entries before advancing the write pointer
    q.tx.writePtr += n;
    if (q.tx.writePtr >= q.tx.msgCount) q.tx.writePtr -= q.tx.msgCount;
    *q.p_write_outgoing = q.tx.writePtr;
    q.tx_free -= n;
    barrier::dma_full();
    return MSGQ_OK;
}

uint32_t msgq_rx_get_read_available(msgq& q) {
    if (!q.rx_linked) return 0;
    q.rx.writePtr = *q.p_write_incoming;
    if (q.rx.writePtr >= q.rx.msgCount) return 0;
    uint32_t avail = q.rx.writePtr + q.rx.msgCount - q.rx_read_ptr;
    if (avail >= q.rx.msgCount) avail -= q.rx.msgCount;
    q.rx_avail = avail;
    return avail;
}

const void* msgq_rx_get_read_buffer(msgq& q, uint32_t n) {
    if (!q.rx_linked) return nullptr;
    if (n >= q.rx_avail && n >= msgq_rx_get_read_available(q)) return nullptr;
    uint32_t rp = q.rx_read_ptr + n;
    if (rp >= q.rx.msgCount) rp -= q.rx.msgCount;
    return q.their_entries + (rp * q.rx.msgSize);
}

int32_t msgq_rx_mark_consumed(msgq& q, uint32_t n) {
    if (!q.rx_linked) return MSGQ_ERR;
    if (n > q.rx_avail && n > msgq_rx_get_read_available(q)) return MSGQ_ERR;
    q.rx_read_ptr += n;
    if (q.rx_read_ptr >= q.rx.msgCount) q.rx_read_ptr -= q.rx.msgCount;
    *q.p_read_outgoing = q.rx_read_ptr;
    q.rx_avail -= n;
    barrier::dma_full();
    return MSGQ_OK;
}

} // namespace nvidia
