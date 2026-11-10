#ifndef STLXDM_SHELL_STATS_HPP
#define STLXDM_SHELL_STATS_HPP

#include <cstdint>
#include <string>

/* How often the sampler reads, due at whole multiples of this period on the monotonic clock */
constexpr uint64_t STATS_SAMPLE_PERIOD_NS = 1000000000ull;

/* The full scale of a load, which is a percent */
constexpr uint32_t STATS_PERCENT = 100;

constexpr uint32_t STATS_HISTORY = 60;
constexpr uint32_t STATS_CPUS_MAX = 64; /* the kernel's default `MAX_CPUS` */
constexpr uint32_t STATS_PROCESSES_MAX = 128;
constexpr uint32_t STATS_NAME_MAX = 32;

/* One process in the last read of the task list: its threads' CPU time summed, and the tenths of a percent of
 * one CPU they used since the read before */
struct stats_process {
    uint32_t pid = 0;
    char     name[STATS_NAME_MAX] = {};
    uint64_t cpu_time_ns = 0;
    uint32_t load_tenths = 0;
};

/* The system figures the shell shows, read from /dev/sysinfo once a second
 * so the bar and the panels show the same sample. The overall CPU load is
 * also kept for the last minute. The task list is read only while a panel
 * asks for it, since reading it walks every task in the kernel. */
class stats_sampler {
public:
    /* Takes a sample at the first call, then at the first call once a
     * whole second of the monotonic clock has begun more than half a
     * second after the last sample. Returns whether it took one. */
    bool sample(uint64_t now_ns);

    /* Starts or stops reading the task list, taking a read at the start so
     * the next read has a load for each process */
    void set_reads_tasks(bool reads);

    uint32_t cpu_count() const { return m_cpu_count; }

    /* Percent CPU `cpu` was busy between the last two reads of the CPU
     * times, 0 before the second. `cpu` must be less than `cpu_count()`. */
    uint32_t cpu_load(uint32_t cpu) const { return m_cpu_load[cpu]; }

    /* Percent of all CPUs busy `age` reads of the CPU times before the newest, 0 while
     * `history_count()` is 0. Otherwise `age` must be less than it. */
    uint32_t load(uint32_t age = 0) const;
    uint32_t history_count() const { return m_history_count; }

    uint64_t page_size() const { return m_page_size; }
    uint64_t total_pages() const { return m_total_pages; }
    uint64_t used_pages() const { return m_used_pages; }

    /* Time since boot at the last successful read, 0 before one */
    uint64_t uptime_ns() const { return m_uptime_ns; }

    /* The last read of the task list since reading started, empty until one succeeds: its tasks, and the first
     * `STATS_PROCESSES_MAX` processes they form, busiest first. `rank` must be less than `process_count()`. */
    uint32_t task_count() const { return m_task_count; }
    uint32_t process_count() const { return m_process_count; }
    const stats_process& process(uint32_t rank) const { return m_processes[rank]; }

    /* Whether the last read filled the read buffer, so more tasks may follow the ones it holds */
    bool tasks_cut() const { return m_tasks_cut; }

private:
    void read_cpus();
    void read_memory();
    void read_uptime();
    void read_tasks(uint64_t now_ns, bool is_baseline);

    uint64_t m_due_ns = 0;
    bool     m_reads_tasks = false;
    uint64_t m_tasks_read_ns = 0;

    uint32_t m_cpu_count = 0;
    uint64_t m_cpu_busy_ns[STATS_CPUS_MAX] = {};
    uint64_t m_cpu_idle_ns[STATS_CPUS_MAX] = {};
    uint32_t m_cpu_load[STATS_CPUS_MAX] = {};

    /* The overall load ring, newest at `m_history_head` */
    uint8_t  m_history[STATS_HISTORY] = {};
    uint32_t m_history_head = 0;
    uint32_t m_history_count = 0;

    uint64_t m_page_size = 0;
    uint64_t m_total_pages = 0;
    uint64_t m_used_pages = 0;
    uint64_t m_uptime_ns = 0;

    uint32_t      m_task_count = 0;
    bool          m_tasks_cut = false;
    uint32_t      m_process_count = 0;
    stats_process m_processes[STATS_PROCESSES_MAX];
};

/* `uptime_ns` in whole seconds as hours, minutes and seconds, such as 2:05:09 */
std::string format_uptime(uint64_t uptime_ns);

#endif
