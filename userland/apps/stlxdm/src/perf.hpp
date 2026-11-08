#ifndef STLXDM_PERF_HPP
#define STLXDM_PERF_HPP

#include <cstdint>

/* Rolling statistics window and the readout refresh cadence */
constexpr uint32_t PERF_WINDOW_MS = 10000;
constexpr uint32_t PERF_REFRESH_MS = 250;

/* An input whose response never came is forgotten after this long */
constexpr uint64_t PERF_INPUT_STALE_NS = 2000000000ull;

/* The shortest time one output's frame statistics line covers */
constexpr uint64_t FRAME_STATS_PERIOD_NS = 60000000000ull;

/* What the bar shows after one refresh: the window's latency percentile
 * and newest sample, `valid` if it holds one, and its missed frames */
struct perf_snapshot {
    bool valid = false;
    uint32_t p95_us = 0;
    uint32_t last_us = 0;
    bool counts_missed_frames = false;
    uint32_t missed_frames = 0;
};

/* What one GPU compose cost: its thread's CPU time from its layer lists to
 * its submit and, when its queue writes timestamps and their read succeeds,
 * its GPU time between the timestamps around its dispatches */
struct compose_times {
    uint64_t cpu_ns = 0;
    uint64_t gpu_ns = 0;
    bool     has_gpu_time = false;
};

/* Measures the desktop's input to next paint latency and sums the
 * frames presents missed, both over the rolling window of
 * `PERF_WINDOW_MS`. The latency runs from the poll wakeup that
 * delivered an input to the present that shows its consequence. A
 * consequence is damage the desktop itself produced in that wakeup,
 * such as the pointer drawn into frames, a drag, or a focus change, or
 * the first commit from a client a key press reached. An input with no
 * consequence is dropped rather than charged to an unrelated frame such
 * as the clock. The one blind spot is a key the client ignores, which
 * its next unprompted repaint, a cursor blink for instance, is taken to
 * answer. Sampling costs one timestamp per presented frame and
 * refreshes run at a fixed cadence. Once the window drains the monitor
 * goes quiescent and asks for no wakeups until the next input or missed
 * frame. */
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

    /* A frame was presented */
    void note_present(uint64_t now_ns);

    /* Takes the missed frames summed over the served outputs that count
     * them, each since its presenter's init, -1 if none does. Each
     * increase after the first total is stamped `now_ns` and counts in
     * each refresh up to `PERF_WINDOW_MS` later, and a fall only lowers
     * the base. */
    void note_missed_frames(uint64_t now_ns, int64_t total);

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

    /* Frames missed between two totals taken, stamped with the later
     * one's time */
    struct missed_sample {
        uint64_t at_ns = 0;
        uint32_t frames = 0;
    };

    static constexpr uint32_t RING = 1024;

    sample m_ring[RING];
    uint32_t m_head = 0;
    uint32_t m_count = 0;

    missed_sample m_missed_ring[RING];
    uint32_t m_missed_head = 0;
    uint32_t m_missed_sample_count = 0;
    uint64_t m_missed_total = 0;
    bool m_counts_missed_frames = false;

    uint64_t m_pending_ns = 0;
    uint32_t m_seq = 0;
    bool m_answered = false;
    bool m_fresh = false;
    uint64_t m_next_refresh_ns = 0;
    bool m_quiescent = false;
};

/* One output's GPU frames since this record's last line or, before its
 * first, since it was made. The line is logged at the first frame that ends
 * `FRAME_STATS_PERIOD_NS` or more after the period's first, so no timer
 * wakes for it. */
class frame_stats {
public:
    /* Adds a frame whose compose ended at `now_ns`, `is_whole_output` when
     * one of its rectangles is the whole output, then logs the line for
     * `output` and starts over once the period is over */
    void add(uint32_t output, uint64_t now_ns, bool is_whole_output, const compose_times& times);

private:
    void log(uint32_t output, uint64_t now_ns) const;

    uint64_t m_start_ns = 0;
    uint64_t m_frames = 0;
    uint64_t m_cpu_total_ns = 0;
    uint64_t m_cpu_longest_ns = 0;

    /* Frames with a GPU time, and those of them that composed the
     * whole output */
    uint64_t m_gpu_frames = 0;
    uint64_t m_gpu_total_ns = 0;
    uint64_t m_gpu_longest_ns = 0;
    uint64_t m_whole_frames = 0;
    uint64_t m_whole_longest_ns = 0;
};

#endif
