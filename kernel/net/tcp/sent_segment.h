#ifndef STELLUX_NET_TCP_SENT_SEGMENT_H
#define STELLUX_NET_TCP_SENT_SEGMENT_H

#include "common/types.h"
#include "common/list.h"

namespace net {
namespace tcp {

constexpr size_t SENT_SEGMENT_COST = 64; // the heap slab a record is charged as

constexpr uint8_t MARK_SACKED        = 1u << 0;
constexpr uint8_t MARK_LOST          = 1u << 1;
constexpr uint8_t MARK_RETRANSMITTED = 1u << 2;
constexpr uint8_t MARK_TLP           = 1u << 3;

/**
 * One segment as it was last sent, alive until the peer acknowledges its end.
 * The send queue holds the bytes, this holds what the network was given.
 */
struct sent_segment {
    list::node link;
    uint32_t   start_seq;
    uint32_t   end_seq;
    uint64_t   sent_ns;
    uint16_t   retrans;
    uint8_t    marks;
};
static_assert(sizeof(sent_segment) <= SENT_SEGMENT_COST);

/**
 * What an acknowledgment covered: the bytes it newly acknowledged and the
 * send time of the oldest record it covered, zero when any covered record
 * was retransmitted, since the reply may be to the retransmission and the
 * records behind it waited for it (Karn's rule, RFC 6298 3).
 */
struct acknowledged {
    uint32_t bytes;
    uint64_t rtt_sample_sent_ns;
};

/**
 * The records of one connection's unacknowledged segments, in sequence order.
 * Records are charged to the global budget one at a time and freed as the
 * acknowledged edge passes them. The owner serializes every call.
 */
class sent_segments {
public:
    void init(size_t cap);

    bool empty() const { return m_records.empty(); }
    size_t count() const { return m_records.size(); }
    size_t cap() const { return m_cap; }
    void set_cap(size_t cap) { m_cap = cap; }

    sent_segment* oldest() { return m_records.front(); }
    sent_segment* newest() { return m_records.back(); }
    size_t lost_bytes() const { return m_lost_bytes; }

    /**
     * @brief The oldest record marked lost and not yet sent again, or nullptr.
     */
    sent_segment* oldest_lost();

    /**
     * @brief Records a segment sent at `now_ns` covering `start_seq` up to
     * but not including `end_seq`, after every record already held.
     * @return The record, or nullptr when the cap or the budget refuses, in
     *         which case the segment must not be sent.
     */
    sent_segment* track(uint32_t start_seq, uint32_t end_seq, uint64_t now_ns);

    /**
     * @brief Notes that the record's segment went out again at `now_ns`,
     * which takes it out of the lost bytes.
     */
    void mark_retransmitted(sent_segment* segment, uint64_t now_ns);

    /**
     * @brief Marks every record lost, as a retransmission timeout does.
     */
    void mark_all_lost();

    /**
     * @brief Takes the lost mark off every record, as a timeout found spurious does.
     */
    void clear_lost_marks();

    /**
     * @brief Frees every record `ack_seq` covers whole and trims the one it
     * covers in part.
     */
    acknowledged acknowledge(uint32_t ack_seq);

    /**
     * @brief Frees every record.
     */
    void clear();

private:
    void release(sent_segment* segment);

    list::head<sent_segment, &sent_segment::link> m_records;
    size_t                                        m_cap;
    size_t                                        m_lost_bytes;
};

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_SENT_SEGMENT_H
