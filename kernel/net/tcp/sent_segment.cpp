#include "net/tcp/sent_segment.h"
#include "net/tcp/byte_queue.h"
#include "net/tcp/seq.h"
#include "mm/heap.h"

namespace net {
namespace tcp {

static uint32_t length_of(const sent_segment* segment) {
    return segment->end_seq - segment->start_seq;
}

void sent_segments::init(size_t cap) {
    m_records.init();
    m_cap = cap;
    m_lost_bytes = 0;
}

void sent_segments::release(sent_segment* segment) {
    if (segment->marks & MARK_LOST) {
        m_lost_bytes -= length_of(segment);
    }

    m_records.remove(segment);
    heap::ufree(segment);
    release_budget(SENT_SEGMENT_COST);
}

sent_segment* sent_segments::track(uint32_t start_seq, uint32_t end_seq, uint64_t now_ns) {
    if (m_records.size() >= m_cap || !reserve_budget(SENT_SEGMENT_COST)) {
        return nullptr;
    }

    sent_segment* segment = static_cast<sent_segment*>(heap::ualloc(sizeof(sent_segment)));
    if (!segment) {
        release_budget(SENT_SEGMENT_COST);
        return nullptr;
    }

    segment->link = {};
    segment->start_seq = start_seq;
    segment->end_seq = end_seq;
    segment->sent_ns = now_ns;
    segment->retrans = 0;
    segment->marks = 0;
    m_records.push_back(segment);

    return segment;
}

sent_segment* sent_segments::oldest_lost() {
    for (sent_segment& segment : m_records) {
        if (segment.marks & MARK_LOST) {
            return &segment;
        }
    }

    return nullptr;
}

void sent_segments::mark_retransmitted(sent_segment* segment, uint64_t now_ns) {
    if (segment->retrans < 0xFFFF) {
        segment->retrans++;
    }

    if (segment->marks & MARK_LOST) {
        m_lost_bytes -= length_of(segment);
    }

    segment->sent_ns = now_ns;
    segment->marks = (segment->marks & ~MARK_LOST) | MARK_RETRANSMITTED;
}

void sent_segments::mark_all_lost() {
    for (sent_segment& segment : m_records) {
        if (!(segment.marks & MARK_LOST)) {
            segment.marks |= MARK_LOST;
            m_lost_bytes += length_of(&segment);
        }
    }
}

void sent_segments::clear_lost_marks() {
    for (sent_segment& segment : m_records) {
        segment.marks &= ~MARK_LOST;
    }

    m_lost_bytes = 0;
}

acknowledged sent_segments::acknowledge(uint32_t ack_seq) {
    acknowledged result = {};
    bool covered_retransmission = false;

    while (sent_segment* segment = m_records.front()) {
        if (!seq_leq(segment->end_seq, ack_seq)) {
            break;
        }

        result.bytes += length_of(segment);
        if (segment->retrans != 0) {
            covered_retransmission = true;
        } else if (result.rtt_sample_sent_ns == 0) {
            result.rtt_sample_sent_ns = segment->sent_ns;
        }

        release(segment);
    }

    sent_segment* partial = m_records.front();
    if (partial && seq_lt(partial->start_seq, ack_seq)) {
        uint32_t trimmed = ack_seq - partial->start_seq;
        result.bytes += trimmed;

        partial->start_seq = ack_seq;
        covered_retransmission = covered_retransmission || partial->retrans != 0;

        if (partial->marks & MARK_LOST) {
            m_lost_bytes -= trimmed;
        }
    }

    if (covered_retransmission) {
        result.rtt_sample_sent_ns = 0;
    }

    return result;
}

void sent_segments::clear() {
    while (sent_segment* segment = m_records.front()) {
        release(segment);
    }
}

} // namespace tcp
} // namespace net
