#include "perf.hpp"

#include <algorithm>
#include <cstdio>

/* Scratch for the window's latencies during a refresh */
static uint32_t g_values[1024];

static unsigned long long micros(uint64_t ns) {
    return static_cast<unsigned long long>(ns / 1000);
}

void perf_monitor::note_input(uint64_t now_ns) {
    m_quiescent = false;

    /* An input still waiting keeps its claim unless its response is
     * so overdue that it clearly never comes */
    if (m_pending_ns != 0 && now_ns - m_pending_ns <= PERF_INPUT_STALE_NS) {
        return;
    }

    m_pending_ns = now_ns;
    m_answered = false;
    m_fresh = true;
    if (++m_seq == 0) {
        m_seq = 1;
    }
}

void perf_monitor::answer(uint32_t seq) {
    if (m_pending_ns != 0 && seq == m_seq) {
        m_answered = true;
    }
}

void perf_monitor::settle(bool dm_damage) {
    if (m_fresh && dm_damage) {
        m_answered = true;
    }
    m_fresh = false;
}

void perf_monitor::note_present(uint64_t now_ns) {
    if (m_pending_ns == 0 || !m_answered) {
        return;
    }

    if (now_ns > m_pending_ns) {
        m_ring[m_head] = { now_ns, static_cast<uint32_t>((now_ns - m_pending_ns) / 1000) };
        m_head = (m_head + 1) % RING;
        if (m_count < RING) {
            m_count++;
        }
        m_quiescent = false;
    }
    m_pending_ns = 0;
    m_answered = false;
}

void perf_monitor::note_missed_frames(uint64_t now_ns, int64_t total) {
    if (total < 0) {
        return;
    }

    uint64_t latest_total = static_cast<uint64_t>(total);
    if (m_counts_missed_frames && latest_total > m_missed_total) {
        m_missed_ring[m_missed_head] = { now_ns, static_cast<uint32_t>(latest_total - m_missed_total) };
        m_missed_head = (m_missed_head + 1) % RING;
        if (m_missed_sample_count < RING) {
            m_missed_sample_count++;
        }

        m_quiescent = false;
    }

    m_counts_missed_frames = true;
    m_missed_total = latest_total;
}

int64_t perf_monitor::timeout_ns(uint64_t now_ns) const {
    if (m_quiescent) {
        return -1;
    }

    return m_next_refresh_ns > now_ns
         ? static_cast<int64_t>(m_next_refresh_ns - now_ns) : 0;
}

/* Walks both rings newest first to the window's edge, copying the
 * latencies out so p95's partial sort leaves their ring alone */
bool perf_monitor::tick(uint64_t now_ns, perf_snapshot& out) {
    if (m_quiescent || now_ns < m_next_refresh_ns) {
        return false;
    }

    m_next_refresh_ns = now_ns + PERF_REFRESH_MS * 1000000ull;

    uint64_t oldest = now_ns > PERF_WINDOW_MS * 1000000ull
                    ? now_ns - PERF_WINDOW_MS * 1000000ull : 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < m_count; i++) {
        const sample& s = m_ring[(m_head + RING - 1 - i) % RING];
        if (s.at_ns < oldest) {
            break;
        }
        g_values[n++] = s.us;
    }

    uint32_t missed_frames = 0;
    for (uint32_t i = 0; i < m_missed_sample_count; i++) {
        const missed_sample& s = m_missed_ring[(m_missed_head + RING - 1 - i) % RING];
        if (s.at_ns < oldest) {
            break;
        }

        missed_frames += s.frames;
    }

    out = perf_snapshot();
    out.counts_missed_frames = m_counts_missed_frames;
    out.missed_frames = missed_frames;

    /* Without latency samples their readout's dashes never change, so
     * the monitor goes quiescent once no missed frames are left */
    if (n == 0) {
        m_quiescent = missed_frames == 0;
        return true;
    }

    /* Nearest rank percentile, so a handful of samples reports its
     * worst rather than its best */
    out.valid = true;
    out.last_us = g_values[0];
    uint32_t rank = (n * 95 + 99) / 100 - 1;
    std::nth_element(g_values, g_values + rank, g_values + n);
    out.p95_us = g_values[rank];

    return true;
}

void frame_stats::add(uint32_t output, uint64_t now_ns, bool is_whole_output, const compose_times& times) {
    if (m_frames == 0) {
        m_start_ns = now_ns;
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

    log(output, now_ns);
    *this = frame_stats();
}

void frame_stats::log(uint32_t output, uint64_t now_ns) const {
    unsigned long long frames = static_cast<unsigned long long>(m_frames);
    unsigned long long seconds = static_cast<unsigned long long>((now_ns - m_start_ns) / 1000000000ull);

    if (m_gpu_frames == 0) {
        printf("stlxdm: output %u composed %llu frames on the GPU in %llu s: CPU time %llu us on average, %llu us at "
               "most\r\n", output, frames, seconds, micros(m_cpu_total_ns / m_frames), micros(m_cpu_longest_ns));
        return;
    }

    char whole[128] = "";
    if (m_whole_frames != 0) {
        snprintf(whole, sizeof(whole), ", %llu us at most in the %llu that composed the whole output",
                 micros(m_whole_longest_ns), static_cast<unsigned long long>(m_whole_frames));
    }

    printf("stlxdm: output %u composed %llu frames on the GPU in %llu s: CPU time %llu us on average, %llu us at most, "
           "GPU time %llu us on average, %llu us at most%s\r\n", output, frames, seconds,
           micros(m_cpu_total_ns / m_frames), micros(m_cpu_longest_ns), micros(m_gpu_total_ns / m_gpu_frames),
           micros(m_gpu_longest_ns), whole);
}
