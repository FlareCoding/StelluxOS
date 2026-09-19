#include "net/tcp/reassembly.h"
#include "net/tcp/seq.h"
#include "net/byteorder.h"

namespace net {
namespace tcp {

static const tcp_header* header_of(const packet* pkt) {
    return reinterpret_cast<const tcp_header*>(pkt->transport_header());
}

// The sequence of the first byte still in the packet's window
static uint32_t segment_start(const packet* pkt) {
    const tcp_header* hdr = header_of(pkt);
    const uint8_t* payload = reinterpret_cast<const uint8_t*>(hdr) + hdr->header_len();
    return ntohl(hdr->seq) + static_cast<uint32_t>(pkt->data() - payload);
}

// One past the last sequence the packet covers, the FIN included
static uint32_t segment_end(const packet* pkt) {
    return segment_start(pkt) + static_cast<uint32_t>(pkt->length()) + ((header_of(pkt)->flags & FLAG_FIN) ? 1 : 0);
}

static bool carries_fin(const packet* pkt) {
    return header_of(pkt)->flags & FLAG_FIN;
}

static bool starts_before(const packet* a, const packet* b) {
    return seq_lt(segment_start(a), segment_start(b));
}

static void release(tcp_conn* conn, packet* pkt) {
    conn->ooo_queue.remove(pkt);
    conn->ooo_bytes -= pkt->length();
    release_budget(PACKET_OBJECT_SIZE);
    packet::free(pkt);
}

static void trim_front(tcp_conn* conn, packet* pkt, size_t bytes) {
    size_t cut = bytes < pkt->length() ? bytes : pkt->length();
    (void)pkt->pull(cut);
    conn->ooo_bytes -= cut;
}

void note_dsack_locked(tcp_conn* conn, uint32_t start, uint32_t end) {
    if (conn->sack_ok && seq_lt(start, end)) {
        conn->dsack = {start, end};
        conn->dsack_pending = true;
    }
}

// RFC 2018 4: the block holding the newest arrival goes first, blocks it
// grew into are merged, the rest keep their order
static void note_sack_block_locked(tcp_conn* conn, uint32_t start, uint32_t end) {
    sack_block kept[MAX_SACK_BLOCKS];
    uint8_t kept_count = 0;
    for (uint8_t i = 0; i < conn->recent_sack_count; i++) {
        const sack_block& block = conn->recent_sacks[i];
        if (seq_leq(block.start, end) && seq_leq(start, block.end)) {
            continue;
        }

        if (kept_count < MAX_SACK_BLOCKS - 1) {
            kept[kept_count++] = block;
        }
    }

    conn->recent_sacks[0] = {start, end};
    for (uint8_t i = 0; i < kept_count; i++) {
        conn->recent_sacks[i + 1] = kept[i];
    }

    conn->recent_sack_count = static_cast<uint8_t>(kept_count + 1);
}

static void forget_acknowledged_blocks_locked(tcp_conn* conn) {
    uint8_t kept = 0;
    for (uint8_t i = 0; i < conn->recent_sack_count; i++) {
        if (seq_gt(conn->recent_sacks[i].end, conn->rcv_nxt)) {
            conn->recent_sacks[kept++] = conn->recent_sacks[i];
        }
    }

    conn->recent_sack_count = kept;
}

// The contiguous run of queued segments that `pkt` belongs to
static sack_block block_around(tcp_conn* conn, packet* pkt) {
    sack_block run = {segment_start(pkt), segment_end(pkt)};
    bool in_run = false;
    bool reached = false;
    for (packet& p : conn->ooo_queue) {
        uint32_t start = segment_start(&p);
        uint32_t end = segment_end(&p);
        if (!in_run || seq_gt(start, run.end)) {
            if (reached) {
                break;
            }

            run = {start, end};
            in_run = true;
        } else if (seq_gt(end, run.end)) {
            run.end = end;
        }

        if (&p == pkt) {
            reached = true;
        }
    }

    return run;
}

// Inserts by sequence, the earlier packet keeping any overlap, and returns
// the packet holding the segment's bytes: `pkt` itself, or the queued one
// that already covered all of it.
static packet* insert_out_of_order_locked(tcp_conn* conn, packet* pkt) {
    uint32_t start = segment_start(pkt);
    uint32_t end = segment_end(pkt);

    auto it = conn->ooo_queue.begin();
    while (it != conn->ooo_queue.end()) {
        packet* queued = &*it;
        ++it;
        uint32_t queued_start = segment_start(queued);
        uint32_t queued_end = segment_end(queued);

        if (seq_leq(queued_end, start)) {
            continue;
        }

        if (seq_geq(queued_start, end)) {
            break;
        }

        if (seq_leq(queued_start, start)) {
            if (seq_geq(queued_end, end)) {
                return queued;
            }

            (void)pkt->pull(queued_end - start);
            start = queued_end;
            continue;
        }

        if (seq_geq(end, queued_end)) {
            release(conn, queued);
            continue;
        }

        trim_front(conn, queued, end - queued_start);
        break;
    }

    conn->ooo_queue.insert_sorted(pkt, starts_before);
    conn->ooo_bytes += pkt->length();

    return pkt;
}

// Moves the queued segments now contiguous with rcv_nxt into the receive queue
static size_t drain_out_of_order_locked(tcp_conn* conn, uint32_t* fin_seq) {
    size_t taken = 0;

    while (packet* head = conn->ooo_queue.front()) {
        uint32_t start = segment_start(head);
        if (seq_gt(start, conn->rcv_nxt)) {
            break;
        }

        if (seq_lt(start, conn->rcv_nxt)) {
            trim_front(conn, head, conn->rcv_nxt - start);
        }

        size_t appended = conn->rcv_queue.append(head->data(), head->length());
        conn->rcv_nxt += static_cast<uint32_t>(appended);
        conn->ooo_bytes -= appended;
        taken += appended;
        (void)head->pull(appended);
        if (head->length() > 0) {
            break;
        }

        if (carries_fin(head)) {
            *fin_seq = conn->rcv_nxt;
        }

        conn->ooo_queue.remove(head);
        release_budget(PACKET_OBJECT_SIZE);
        packet::free(head);
    }

    forget_acknowledged_blocks_locked(conn);

    return taken;
}

static bool accepts_payload(tcp_state state) {
    return state == tcp_state::established || state == tcp_state::fin_wait_1 ||
           state == tcp_state::fin_wait_2;
}

payload_result take_payload_locked(tcp_conn* conn, packet* pkt, const tcp_header* hdr, uint32_t seq, size_t payload_len) {
    payload_result result = {};
    bool fin = hdr->flags & FLAG_FIN;
    uint32_t end = seq + static_cast<uint32_t>(payload_len) + (fin ? 1 : 0);
    if (!accepts_payload(conn->state) || end == seq) {
        return result;
    }

    const uint8_t* payload = reinterpret_cast<const uint8_t*>(hdr) + hdr->header_len();
    if (seq_leq(end, conn->rcv_nxt)) {
        note_dsack_locked(conn, seq, end);
        return result;
    }

    if (seq_lt(seq, conn->rcv_nxt)) {
        uint32_t already = conn->rcv_nxt - seq;
        note_dsack_locked(conn, seq, conn->rcv_nxt);

        payload += already;
        payload_len -= already;
        seq = conn->rcv_nxt;
    }

    if (seq == conn->rcv_nxt) {
        result.queued = conn->rcv_queue.append(payload, payload_len);
        conn->rcv_nxt += static_cast<uint32_t>(result.queued);

        if (result.queued == payload_len) {
            result.queued += drain_out_of_order_locked(conn, &result.fin_seq);
            if (fin && result.fin_seq == 0 && end == conn->rcv_nxt + 1) {
                result.fin_seq = conn->rcv_nxt;
            }
        }

        update_receive_window_locked(conn);
        return result;
    }

    uint32_t window_end = conn->rcv_nxt + conn->rcv_wnd;
    if (seq_gt(end, window_end)) {
        (void)pkt->trim(pkt->length() - (end - window_end));
    }

    if (conn->ooo_queue.size() >= MAX_OOO_PACKETS || conn->ooo_bytes + payload_len > conn->rcv_queue.free_space() ||
        !reserve_budget(PACKET_OBJECT_SIZE)) {
        return result;
    }

    (void)pkt->pull(hdr->header_len());
    packet* holder = insert_out_of_order_locked(conn, pkt);
    if (holder != pkt) {
        release_budget(PACKET_OBJECT_SIZE);
        note_dsack_locked(conn, seq, end);
    }

    result.packet_taken = holder == pkt;
    sack_block block = block_around(conn, holder);
    note_sack_block_locked(conn, block.start, block.end);
    update_receive_window_locked(conn);

    return result;
}

void clear_out_of_order_locked(tcp_conn* conn) {
    while (packet* head = conn->ooo_queue.front()) {
        release(conn, head);
    }

    conn->recent_sack_count = 0;
    conn->dsack_pending = false;
}

void fill_sack_blocks_locked(const tcp_conn* conn, tcp_options* opts) {
    if (!conn->sack_ok) {
        return;
    }

    uint8_t count = 0;
    if (conn->dsack_pending) {
        opts->sack_blocks[count++] = conn->dsack;
    }

    for (uint8_t i = 0; i < conn->recent_sack_count && count < MAX_SACK_BLOCKS; i++) {
        opts->sack_blocks[count++] = conn->recent_sacks[i];
    }

    opts->sack_count = count;
}

} // namespace tcp
} // namespace net
