#ifndef STLXDM_SHELL_STATS_HPP
#define STLXDM_SHELL_STATS_HPP

#include <cstdint>
#include <string>

/* How often the sampler reads, due at whole multiples of this period on the monotonic clock */
constexpr uint64_t STATS_SAMPLE_PERIOD_NS = 1000000000ull;

/* The full scale of a load, which is a percent */
constexpr uint32_t STATS_PERCENT = 100;

/* The system figures the bar and the system menu show, read from /dev/sysinfo once a second */
class stats_sampler {
public:
    /* Takes a sample at the first call, then at the first call once a
     * whole second of the monotonic clock has begun more than half a
     * second after the last sample. Returns whether it took one. */
    bool sample(uint64_t now_ns);

    /* Percent of all CPUs busy between the last two successful reads of
     * their times, 0 before the second such read */
    uint32_t load() const { return m_load; }

    uint64_t page_size() const { return m_page_size; }
    uint64_t used_pages() const { return m_used_pages; }

    /* Time since boot at the last successful read, 0 before one */
    uint64_t uptime_ns() const { return m_uptime_ns; }

private:
    void read_cpus();
    void read_memory();
    void read_uptime();

    uint64_t m_due_ns = 0;

    /* All CPUs' busy and idle time at the last read, and the load since the read before */
    bool     m_has_totals = false;
    uint64_t m_busy_ns = 0;
    uint64_t m_idle_ns = 0;
    uint32_t m_load = 0;

    uint64_t m_page_size = 0;
    uint64_t m_used_pages = 0;
    uint64_t m_uptime_ns = 0;
};

/* `uptime_ns` in whole seconds as hours, minutes and seconds, such as 2:05:09 */
std::string format_uptime(uint64_t uptime_ns);

#endif
