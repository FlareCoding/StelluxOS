#ifndef STLXDM_PERF_HPP
#define STLXDM_PERF_HPP

#include <cstdint>

/* Rolling statistics window and the readout refresh cadence */
constexpr uint32_t PERF_WINDOW_MS = 10000;
constexpr uint32_t PERF_REFRESH_MS = 250;

/* An input whose response never came is forgotten after this long */
constexpr uint64_t PERF_INPUT_STALE_NS = 2000000000ull;

/* What the bar shows after one refresh: the percentile over the
 * rolling window and the newest sample */
struct perf_snapshot {
    bool valid = false;
    uint32_t p95_us = 0;
    uint32_t last_us = 0;
};

/* Measures the desktop's input to next paint latency: from the poll
 * wakeup that delivered an input to the present that shows its
 * consequence. A consequence is damage the desktop itself produced in
 * that wakeup, such as the cursor, a drag, or a focus change, or the
 * first commit from a client a key press reached. An input with no
 * consequence is dropped rather than charged to an unrelated frame
 * such as the clock. The one blind spot is a key the client ignores,
 * which its next unprompted repaint, a cursor blink for instance, is
 * taken to answer. Sampling costs one timestamp per presented
 * frame and refreshes run at a fixed cadence. Once the window drains
 * the monitor goes quiescent and asks for no wakeups until the next
 * input. */
class perf_monitor {
public:
    /* An input that may paint. Only one waits at a time: the oldest
     * input since the last paint defines the latency, later ones wait
     * behind it. */
    void note_input(uint64_t now_ns);

    /* Id of the waiting input, 0 when none, and whether it was
     * stamped during the current wakeup. Deliveries made while it is
     * fresh are tagged with the id so their commits can be matched. */
    uint32_t seq() const { return m_pending_ns != 0 ? m_seq : 0; }
    bool fresh() const { return m_fresh; }

    /* A consequence of input seq will be in the next present */
    void answer(uint32_t seq);

    /* End of a wakeup: the desktop's own damage answers a fresh input */
    void settle(bool dm_damage);

    /* A frame reached the screen */
    void note_present(uint64_t now_ns);

    /* Nanoseconds until the next refresh, -1 when nothing will change */
    int64_t timeout_ns(uint64_t now_ns) const;

    /* Runs the refresh when it is due. Returns true with out filled
     * when the bar should take a new snapshot. */
    bool tick(uint64_t now_ns, perf_snapshot& out);

private:
    struct sample {
        uint64_t at_ns = 0;
        uint32_t us = 0;
    };

    static constexpr uint32_t RING = 1024;

    sample m_ring[RING];
    uint32_t m_head = 0;
    uint32_t m_count = 0;

    uint64_t m_pending_ns = 0;
    uint32_t m_seq = 0;
    bool m_answered = false;
    bool m_fresh = false;
    uint64_t m_next_refresh_ns = 0;
    bool m_quiescent = false;
};

#endif
