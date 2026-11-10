#include "frame_stats.hpp"

#include <algorithm>
#include <cstdio>

static unsigned long long micros(uint64_t ns) {
    return static_cast<unsigned long long>(ns / 1000);
}

void frame_stats::add(
    uint32_t output,
    uint64_t now_ns,
    bool is_whole_output,
    const compose_times& times,
    int64_t missed_frames
) {
    if (m_frames == 0) {
        m_start_ns = now_ns;
    }

    if (m_missed_at_start < 0) {
        m_missed_at_start = missed_frames;
    }

    m_frames++;
    m_cpu_total_ns += times.cpu_ns;
    m_cpu_longest_ns = std::max(m_cpu_longest_ns, times.cpu_ns);

    if (times.has_gpu_time) {
        m_gpu_frames++;
        m_gpu_total_ns += times.gpu_ns;
        m_gpu_longest_ns = std::max(m_gpu_longest_ns, times.gpu_ns);
    }

    if (times.has_gpu_time && is_whole_output) {
        m_whole_frames++;
        m_whole_longest_ns = std::max(m_whole_longest_ns, times.gpu_ns);
    }

    if (now_ns - m_start_ns < FRAME_STATS_PERIOD_NS) {
        return;
    }

    log(output, now_ns, missed_frames);
    *this = frame_stats();

    /* This frame's present ends after `add()`, so its misses count in the next period */
    m_missed_at_start = missed_frames;
}

void frame_stats::log(uint32_t output, uint64_t now_ns, int64_t missed_frames) const {
    unsigned long long frames = static_cast<unsigned long long>(m_frames);
    unsigned long long seconds = static_cast<unsigned long long>((now_ns - m_start_ns) / 1000000000ull);
    unsigned long long missed_blanks = static_cast<unsigned long long>(missed_frames - m_missed_at_start);
    const char* unit = missed_blanks == 1 ? "blank" : "blanks";

    char missed[64];
    snprintf(missed, sizeof(missed), ", and its presents missed %llu vertical %s", missed_blanks, unit);

    if (m_gpu_frames == 0) {
        printf("stlxdm: output %u composed %llu frames on the GPU in %llu s: CPU time %llu us on average, %llu us at "
               "most%s\r\n", output, frames, seconds, micros(m_cpu_total_ns / m_frames), micros(m_cpu_longest_ns),
               missed);
        return;
    }

    char whole[128] = "";
    if (m_whole_frames != 0) {
        snprintf(whole, sizeof(whole), ", %llu us at most in the %llu that composed the whole output",
                 micros(m_whole_longest_ns), static_cast<unsigned long long>(m_whole_frames));
    }

    printf("stlxdm: output %u composed %llu frames on the GPU in %llu s: CPU time %llu us on average, %llu us at most, "
           "GPU time %llu us on average, %llu us at most%s%s\r\n", output, frames, seconds,
           micros(m_cpu_total_ns / m_frames), micros(m_cpu_longest_ns), micros(m_gpu_total_ns / m_gpu_frames),
           micros(m_gpu_longest_ns), whole, missed);
}
