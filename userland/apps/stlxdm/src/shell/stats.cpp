#include "stats.hpp"

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

constexpr size_t CPU_TEXT_BYTES = 4096;
constexpr size_t SMALL_TEXT_BYTES = 256;

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

    return true;
}

/* All CPUs' busy and idle times since boot, whose growth since the last
 * read gives the load */
void stats_sampler::read_cpus() {
    char text[CPU_TEXT_BYTES];
    if (read_text("/dev/sysinfo/cpu", text, sizeof(text)) <= 0) {
        return;
    }

    uint64_t busy_ns = 0;
    uint64_t idle_ns = 0;
    for (const char* line = text; *line; line = next_line(line)) {
        if (strncmp(line, "cpu", 3) != 0) {
            continue;
        }

        char* end = nullptr;
        strtoull(line + 3, &end, 10);
        busy_ns += strtoull(end, &end, 10);
        idle_ns += strtoull(end, &end, 10);
    }

    if (m_has_totals) {
        uint64_t busy_growth = busy_ns - m_busy_ns;
        m_load = percent_of(busy_growth, busy_growth + idle_ns - m_idle_ns);
    }

    m_has_totals = true;
    m_busy_ns = busy_ns;
    m_idle_ns = idle_ns;
}

void stats_sampler::read_memory() {
    char text[SMALL_TEXT_BYTES];
    if (read_text("/dev/sysinfo/mem", text, sizeof(text)) <= 0) {
        return;
    }

    m_page_size = text_field(text, "page_size ");
    m_used_pages = text_field(text, "used_pages ");
}
