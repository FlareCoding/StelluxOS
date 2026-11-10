#include "stats.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

constexpr uint32_t TENTHS_OF_PERCENT = 1000;
constexpr size_t CPU_TEXT_BYTES = 4096;
constexpr size_t SMALL_TEXT_BYTES = 256;
constexpr size_t TASKS_TEXT_BYTES = 32768;

constexpr uint64_t NS_PER_SECOND = 1000000000ull;
constexpr uint64_t SECONDS_PER_MINUTE = 60;
constexpr uint64_t SECONDS_PER_HOUR = 3600;

static char g_tasks_text[TASKS_TEXT_BYTES];
static stats_process g_read_processes[STATS_PROCESSES_MAX];

/* Reads a /dev/sysinfo text file into `buf`, cut to fit and NUL terminated.
 * Returns the bytes read, or -1 when it cannot be opened. */
static ssize_t read_text(const char* path, char* buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    size_t total = 0;
    while (total < cap - 1) {
        ssize_t got = read(fd, buf + total, cap - 1 - total);
        if (got <= 0) {
            break;
        }

        total += static_cast<size_t>(got);
    }

    close(fd);
    buf[total] = '\0';

    return static_cast<ssize_t>(total);
}

/* The number after `label` in a "<label> <number>" text, 0 without one */
static uint64_t text_field(const char* text, const char* label) {
    const char* at = strstr(text, label);
    if (!at) {
        return 0;
    }

    return strtoull(at + strlen(label), nullptr, 10);
}

static const char* next_line(const char* line) {
    const char* end = strchr(line, '\n');
    return end ? end + 1 : line + strlen(line);
}

static uint32_t percent_of(uint64_t part, uint64_t whole) {
    if (whole == 0) {
        return 0;
    }

    return static_cast<uint32_t>((part * STATS_PERCENT + whole / 2) / whole);
}

bool stats_sampler::sample(uint64_t now_ns) {
    if (now_ns < m_due_ns) {
        return false;
    }

    uint64_t nearest_period = (now_ns + STATS_SAMPLE_PERIOD_NS / 2) / STATS_SAMPLE_PERIOD_NS;
    m_due_ns = (nearest_period + 1) * STATS_SAMPLE_PERIOD_NS;

    read_cpus();
    read_memory();
    read_uptime();
    bool is_task_read_due = m_reads_tasks && now_ns - m_tasks_read_ns >= STATS_SAMPLE_PERIOD_NS / 2;
    if (is_task_read_due) {
        read_tasks(now_ns, false);
    }

    return true;
}

static uint64_t monotonic_ns() {
    timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return static_cast<uint64_t>(ts.tv_sec) * NS_PER_SECOND + static_cast<uint64_t>(ts.tv_nsec);
}

void stats_sampler::set_reads_tasks(bool reads) {
    if (reads == m_reads_tasks) {
        return;
    }

    m_reads_tasks = reads;
    m_task_count = 0;
    m_process_count = 0;
    m_tasks_cut = false;
    if (reads) {
        read_tasks(monotonic_ns(), true);
    }
}

uint32_t stats_sampler::load(uint32_t age) const {
    if (m_history_count == 0) {
        return 0;
    }

    return m_history[(m_history_head + STATS_HISTORY - age) % STATS_HISTORY];
}

/* Each CPU's busy and idle times since boot, whose growth since the last
 * read gives its load */
void stats_sampler::read_cpus() {
    char text[CPU_TEXT_BYTES];
    if (read_text("/dev/sysinfo/cpu", text, sizeof(text)) <= 0) {
        return;
    }

    bool has_previous = m_cpu_count != 0;
    uint64_t busy_growth = 0;
    uint64_t span_growth = 0;
    uint32_t count = 0;
    for (const char* line = text; *line && count < STATS_CPUS_MAX; line = next_line(line)) {
        if (strncmp(line, "cpu", 3) != 0) {
            continue;
        }

        char* end = nullptr;
        strtoull(line + 3, &end, 10);
        uint64_t busy = strtoull(end, &end, 10);
        uint64_t idle = strtoull(end, &end, 10);

        uint64_t cpu_busy_growth = busy - m_cpu_busy_ns[count];
        uint64_t cpu_span_growth = cpu_busy_growth + idle - m_cpu_idle_ns[count];
        m_cpu_load[count] = has_previous ? percent_of(cpu_busy_growth, cpu_span_growth) : 0;
        m_cpu_busy_ns[count] = busy;
        m_cpu_idle_ns[count] = idle;

        busy_growth += cpu_busy_growth;
        span_growth += cpu_span_growth;
        count++;
    }

    m_cpu_count = count;
    if (!has_previous) {
        return;
    }

    m_history_head = (m_history_head + 1) % STATS_HISTORY;
    m_history[m_history_head] = static_cast<uint8_t>(percent_of(busy_growth, span_growth));
    m_history_count = std::min(m_history_count + 1, STATS_HISTORY);
}

void stats_sampler::read_memory() {
    char text[SMALL_TEXT_BYTES];
    if (read_text("/dev/sysinfo/mem", text, sizeof(text)) <= 0) {
        return;
    }

    m_page_size = text_field(text, "page_size ");
    m_total_pages = text_field(text, "total_pages ");
    m_used_pages = text_field(text, "used_pages ");
}

void stats_sampler::read_uptime() {
    char text[SMALL_TEXT_BYTES];
    if (read_text("/dev/sysinfo/uptime", text, sizeof(text)) > 0) {
        m_uptime_ns = strtoull(text, nullptr, 10);
    }
}

static stats_process* find_process(stats_process* list, uint32_t count, uint32_t pid) {
    for (uint32_t i = 0; i < count; i++) {
        if (list[i].pid == pid) {
            return &list[i];
        }
    }

    return nullptr;
}

/* Adds one "tid pid state cpu cpu_time_ns name" line to its process, named by its main thread. Kernel tasks,
 * which belong to no process, and the tasks of processes past `STATS_PROCESSES_MAX` are skipped. */
static void add_task_line(const char* line, uint32_t& count) {
    char* end = nullptr;
    uint32_t tid = static_cast<uint32_t>(strtoul(line, &end, 10));
    uint32_t pid = static_cast<uint32_t>(strtoul(end, &end, 10));
    if (pid == 0 || *end != ' ') {
        return;
    }

    const char* after_state = strchr(end + 1, ' ');
    if (!after_state) {
        return;
    }

    strtoul(after_state, &end, 10);
    uint64_t cpu_time_ns = strtoull(end, &end, 10);
    const char* name = end + (*end == ' ' ? 1 : 0);
    size_t name_length = strcspn(name, "\n");

    stats_process* process = find_process(g_read_processes, count, pid);
    if (!process && count == STATS_PROCESSES_MAX) {
        return;
    }

    if (!process) {
        process = &g_read_processes[count++];
        *process = stats_process();
        process->pid = pid;
    }

    process->cpu_time_ns += cpu_time_ns;
    if (tid == pid || !process->name[0]) {
        size_t kept = std::min(name_length, sizeof(process->name) - 1);
        memcpy(process->name, name, kept);
        process->name[kept] = '\0';
    }
}

/* Groups the tasks into processes, each with a load of its CPU time's growth since the last read, or 0 in a
 * baseline read or without an earlier read of it. Only a read that succeeds stamps `now_ns`. */
void stats_sampler::read_tasks(uint64_t now_ns, bool is_baseline) {
    ssize_t length = read_text("/dev/sysinfo/tasks", g_tasks_text, sizeof(g_tasks_text));
    if (length <= 0) {
        return;
    }

    /* A list that fills the buffer may go on past it, so its last line is left out unless it ends in a newline */
    bool is_cut = static_cast<size_t>(length) == sizeof(g_tasks_text) - 1;
    uint64_t elapsed_ns = is_baseline ? 0 : now_ns - m_tasks_read_ns;
    uint32_t tasks = 0;
    uint32_t count = 0;
    for (const char* line = g_tasks_text; *line; line = next_line(line)) {
        bool is_cut_line = is_cut && !strchr(line, '\n');
        if (is_cut_line) {
            break;
        }

        add_task_line(line, count);
        tasks++;
    }

    for (uint32_t i = 0; i < count; i++) {
        stats_process& process = g_read_processes[i];
        const stats_process* before = find_process(m_processes, m_process_count, process.pid);
        bool has_growth = before && elapsed_ns != 0 && process.cpu_time_ns >= before->cpu_time_ns;
        uint64_t growth_ns = has_growth ? process.cpu_time_ns - before->cpu_time_ns : 0;
        process.load_tenths = has_growth ? static_cast<uint32_t>(growth_ns * TENTHS_OF_PERCENT / elapsed_ns) : 0;
    }

    std::sort(g_read_processes, g_read_processes + count, [](const stats_process& a, const stats_process& b) {
        if (a.load_tenths != b.load_tenths) {
            return a.load_tenths > b.load_tenths;
        }

        return a.cpu_time_ns > b.cpu_time_ns;
    });

    std::copy(g_read_processes, g_read_processes + count, m_processes);
    m_process_count = count;
    m_task_count = tasks;
    m_tasks_cut = is_cut;
    m_tasks_read_ns = now_ns;
}

std::string format_uptime(uint64_t uptime_ns) {
    uint64_t seconds = uptime_ns / NS_PER_SECOND;
    unsigned long long hours = seconds / SECONDS_PER_HOUR;
    unsigned long long minutes = seconds % SECONDS_PER_HOUR / SECONDS_PER_MINUTE;
    unsigned long long rest = seconds % SECONDS_PER_MINUTE;

    char text[32];
    snprintf(text, sizeof(text), "%llu:%02llu:%02llu", hours, minutes, rest);

    return text;
}
