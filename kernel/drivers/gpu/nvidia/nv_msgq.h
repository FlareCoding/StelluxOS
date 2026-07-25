#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_MSGQ_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_MSGQ_H

#include "common/types.h"

// libos message-queue ring protocol (host/CPU side). Faithful port of
// src/common/shared/msgq/msgq.c (+ msgq_priv.h) from open-gpu-kernel-modules.
// The RM CPU path registers no callbacks (no flush/notify/invalidate/barrier),
// so msgq does plain memory access; we add an explicit barrier where RM relies
// on x86 cache coherency. The on-wire headers (written into the shared GSP
// buffer) are ABI with the GSP firmware and must match byte-for-byte.
namespace nvidia {

constexpr uint32_t MSGQ_VERSION       = 0;
constexpr uint32_t MSGQ_MSG_SIZE_MIN  = 16;
constexpr uint32_t MSGQ_FLAGS_SWAP_RX = 1;

// Buffer metadata written by the source (producer), at the start of its ring.
struct msgqTxHeader {
    uint32_t version;   // queue version (MSGQ_VERSION)
    uint32_t size;      // bytes, page aligned
    uint32_t msgSize;   // entry size, bytes, power-of-2, >= 16
    uint32_t msgCount;  // number of entries in queue
    uint32_t writePtr;  // message id of next slot
    uint32_t flags;     // MSGQ_FLAGS_SWAP_RX
    uint32_t rxHdrOff;  // offset of msgqRxHeader from start of backing store
    uint32_t entryOff;  // offset of entries from start of backing store
};
static_assert(sizeof(msgqTxHeader) == 32, "msgqTxHeader must be 32 bytes");

// Buffer metadata written by the sink (consumer).
struct msgqRxHeader {
    uint32_t readPtr;   // message id of last message read
};

// Host-side queue tracking (private; not ABI). Mirrors the fields of
// msgqMetadata that the RM CPU path actually uses.
struct msgq {
    // Our TX ring (we produce; GSP consumes).
    msgqTxHeader*          our_tx_hdr;   // -> command ring base
    msgqRxHeader*          our_rx_hdr;   // -> our rx header (read ptr for SWAP_RX)
    uint8_t*               our_entries;  // first tx entry
    // Their TX ring (GSP produces; we consume) - linked later (status ring).
    const msgqTxHeader*    their_tx_hdr;
    const msgqRxHeader*    their_rx_hdr;
    const uint8_t*         their_entries;

    msgqTxHeader           tx;           // local copy of our tx header
    msgqTxHeader           rx;           // local copy of their tx header
    uint32_t               tx_free;      // cached free tx slots
    uint32_t               rx_read_ptr;  // our read cursor into their ring
    uint32_t               rx_avail;     // cached available rx slots
    bool                   tx_linked;
    bool                   rx_linked;
    bool                   rx_swapped;

    // Resolved read/write pointers in shared memory (SWAP_RX-aware), mirroring
    // msgqMetadata. "incoming" = peer writes/we read; "outgoing" = we write.
    volatile const uint32_t* p_read_incoming;   // GSP's consume cursor of our TX
    volatile const uint32_t* p_write_incoming;  // GSP's produce cursor (their TX)
    uint32_t*                p_read_outgoing;    // our consume cursor of their TX
    uint32_t*                p_write_outgoing;   // our produce cursor (our TX)
};

constexpr int32_t MSGQ_OK  = 0;
constexpr int32_t MSGQ_ERR = -1;

// Create our outgoing (TX) ring over pBackingStore (size bytes). Writes the
// msgqTxHeader into the shared buffer. msgSize/hdrAlign/entryAlign/flags per
// _gspMsgQueueInit (4096 / 16 / 4096 / SWAP_RX).
int32_t msgq_tx_create(msgq& q, void* pBackingStore, uint32_t size, uint32_t msgSize,
                       uint32_t hdrAlignShift, uint32_t entryAlignShift, uint32_t flags);

// Link to the GSP's TX ring as our RX (status queue). Reads + validates their
// header; fails (MSGQ_ERR) until the GSP has run its msgqTxCreate.
int32_t msgq_rx_link(msgq& q, const void* pBackingStore, uint32_t size, uint32_t msgSize);

// TX: free slots, write-slot pointer, submit (advances writePtr in shared hdr).
uint32_t msgq_tx_get_free_space(msgq& q);
void*    msgq_tx_get_write_buffer(msgq& q, uint32_t n);
int32_t  msgq_tx_submit_buffers(msgq& q, uint32_t n);

// RX: available count, read-slot pointer, mark consumed (advances readPtr).
uint32_t    msgq_rx_get_read_available(msgq& q);
const void* msgq_rx_get_read_buffer(msgq& q, uint32_t n);
int32_t     msgq_rx_mark_consumed(msgq& q, uint32_t n);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_MSGQ_H
