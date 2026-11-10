#ifndef STLXDM_FRAME_STATS_HPP
#define STLXDM_FRAME_STATS_HPP

#include <cstdint>

/* The shortest time one output's frame statistics line covers */
constexpr uint64_t FRAME_STATS_PERIOD_NS = 60000000000ull;

/* What one GPU compose cost: its thread's CPU time from its layer lists to
 * its submit and, when its queue writes timestamps and their read succeeds,
 * its GPU time between the timestamps around its dispatches */
struct compose_times {
    uint64_t cpu_ns = 0;
    uint64_t gpu_ns = 0;
    bool     has_gpu_time = false;
};

/* One output's GPU frames since this record's last line or, before its
 * first, since it was made, and the vertical blanks missed by the output's
 * presents that ended meanwhile. The line is logged at the first frame that
 * ends `FRAME_STATS_PERIOD_NS` or more after the period's first, so no
 * timer wakes for it. */
class frame_stats {
public:
    /* Adds a frame composed by `now_ns`, `is_whole_output` when one of its
     * rectangles is the whole output, then logs the line for `output` and
     * starts over once the period is over. `missed_frames` must be the
     * `missed_frames()` of `output`'s presenter, never -1, and one record
     * must take it from a single presenter. */
    void add(
        uint32_t output,
        uint64_t now_ns,
        bool is_whole_output,
        const compose_times& times,
        int64_t missed_frames
    );

private:
    void log(uint32_t output, uint64_t now_ns, int64_t missed_frames) const;

    uint64_t m_start_ns = 0;
    uint64_t m_frames = 0;
    uint64_t m_cpu_total_ns = 0;
    uint64_t m_cpu_longest_ns = 0;

    /* `missed_frames()` at the last line, or at the first frame before any line */
    int64_t m_missed_at_start = -1;

    /* Frames with a GPU time, and those of them that composed the
     * whole output */
    uint64_t m_gpu_frames = 0;
    uint64_t m_gpu_total_ns = 0;
    uint64_t m_gpu_longest_ns = 0;
    uint64_t m_whole_frames = 0;
    uint64_t m_whole_longest_ns = 0;
};

#endif
