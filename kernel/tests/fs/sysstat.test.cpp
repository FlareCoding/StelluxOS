#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "smp/smp.h"
#include "percpu/percpu.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

using test_helpers::spin_wait;

TEST_SUITE(sysstat);

// Read up to cap - 1 bytes from path, NUL terminate, return length
static ssize_t read_all(const char* path, char* buf, size_t cap) {
    fs::file* f = fs::open(path, fs::O_RDONLY);
    if (!f) {
        return -1;
    }
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t rd = fs::read(f, buf + total, cap - 1 - total);
        if (rd <= 0) {
            break;
        }
        total += static_cast<size_t>(rd);
    }
    fs::close(f);
    buf[total] = '\0';
    return static_cast<ssize_t>(total);
}

// Parse the unsigned decimal that follows "<label> " in text
static bool parse_labeled_u64(const char* text, const char* label,
                              uint64_t* out) {
    size_t label_len = string::strlen(label);
    for (const char* p = text; *p; p++) {
        if ((p == text || p[-1] == '\n') &&
            string::strncmp(p, label, label_len) == 0 &&
            p[label_len] == ' ') {
            const char* d = p + label_len + 1;
            if (*d < '0' || *d > '9') {
                return false;
            }
            uint64_t value = 0;
            while (*d >= '0' && *d <= '9') {
                value = value * 10 + static_cast<uint64_t>(*d - '0');
                d++;
            }
            *out = value;
            return true;
        }
    }
    return false;
}

static size_t count_lines(const char* text) {
    size_t lines = 0;
    for (const char* p = text; *p; p++) {
        if (*p == '\n') {
            lines++;
        }
    }
    return lines;
}

// --- cpu_lines_match_cpu_count ---
// Proves: /dev/sysinfo/cpu reports the tick rate and one tick line
// for every CPU.

TEST(sysstat, cpu_lines_match_cpu_count) {
    char buf[2048] = {};
    ssize_t len = read_all("/dev/sysinfo/cpu", buf, sizeof(buf));
    ASSERT_TRUE(len > 0);

    uint32_t cpu_count = 0;
    RUN_ELEVATED({
        cpu_count = smp::cpu_count();
    });

    uint64_t hz = 0;
    EXPECT_TRUE(parse_labeled_u64(buf, "tick_hz", &hz));
    EXPECT_TRUE(hz > 0);

    size_t cpu_lines = 0;
    for (const char* p = buf; *p; p++) {
        if ((p == buf || p[-1] == '\n') &&
            string::strncmp(p, "cpu", 3) == 0) {
            cpu_lines++;
        }
    }
    EXPECT_EQ(cpu_lines, static_cast<size_t>(cpu_count));
}

// --- mem_reports_sane_counters ---
// Proves: /dev/sysinfo/mem reports internally consistent page counters.

TEST(sysstat, mem_reports_sane_counters) {
    char buf[512] = {};
    ssize_t len = read_all("/dev/sysinfo/mem", buf, sizeof(buf));
    ASSERT_TRUE(len > 0);

    uint64_t page_size = 0;
    uint64_t total = 0;
    uint64_t free_count = 0;
    uint64_t used = 0;
    ASSERT_TRUE(parse_labeled_u64(buf, "page_size", &page_size));
    ASSERT_TRUE(parse_labeled_u64(buf, "total_pages", &total));
    ASSERT_TRUE(parse_labeled_u64(buf, "free_pages", &free_count));
    ASSERT_TRUE(parse_labeled_u64(buf, "used_pages", &used));

    EXPECT_TRUE(page_size > 0);
    EXPECT_TRUE(total > 0);
    EXPECT_TRUE(free_count <= total);
    EXPECT_EQ(used, total - free_count);
}

// --- uptime_advances_between_reads ---
// Proves: /dev/sysinfo/uptime reports monotonic time and each fresh
// open captures a newer snapshot.

TEST(sysstat, uptime_advances_between_reads) {
    char buf[64] = {};
    ASSERT_TRUE(read_all("/dev/sysinfo/uptime", buf, sizeof(buf)) > 0);
    uint64_t first = 0;
    for (const char* p = buf; *p >= '0' && *p <= '9'; p++) {
        first = first * 10 + static_cast<uint64_t>(*p - '0');
    }
    EXPECT_TRUE(first > 0);

    ASSERT_TRUE(read_all("/dev/sysinfo/uptime", buf, sizeof(buf)) > 0);
    uint64_t second = 0;
    for (const char* p = buf; *p >= '0' && *p <= '9'; p++) {
        second = second * 10 + static_cast<uint64_t>(*p - '0');
    }
    EXPECT_LT(first, second);
}

// --- tasks_lists_running_tasks ---
// Proves: /dev/sysinfo/tasks reports a compute-bound task as running,
// with numeric lead fields, even when the table spans many reads.

static volatile uint32_t g_tasks_spin_started = 0;

static void tasks_spinner_fn(void*) {
    __atomic_store_n(&g_tasks_spin_started, 1, __ATOMIC_RELEASE);
    // Spin long enough for the reader to stream the whole task table,
    // then exit on our own so no cross-task handshake is needed
    uint64_t deadline = clock::now_ns() + 500ULL * 1000 * 1000;
    while (clock::now_ns() < deadline) {
    }
    sched::exit(0);
}

TEST(sysstat, tasks_lists_running_tasks) {
    // A lone CPU cannot run the spinner and this reader concurrently
    if (smp::cpu_count() < 2) {
        return;
    }
    g_tasks_spin_started = 0;

    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(tasks_spinner_fn, nullptr,
                                                   "test_stats_spin");
        ASSERT_NOT_NULL(t);
        // Place the spinner away from this CPU so it runs while we read
        uint32_t target = (percpu::current_cpu_id() + 1) % smp::cpu_count();
        sched::enqueue_on(t, target);
    });
    ASSERT_TRUE(spin_wait(&g_tasks_spin_started));

    fs::file* f = fs::open("/dev/sysinfo/tasks", fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    // Stream the whole table in chunks, keeping an overlap window so a
    // match cannot be split across chunk boundaries
    char chunk[256];
    char window[300] = {};
    size_t window_len = 0;
    bool has_running = false;
    bool first_chunk = true;

    while (true) {
        ssize_t rd = fs::read(f, chunk, sizeof(chunk));
        if (rd <= 0) {
            break;
        }
        if (first_chunk) {
            EXPECT_TRUE(chunk[0] >= '0' && chunk[0] <= '9');
            first_chunk = false;
        }

        string::memcpy(window + window_len, chunk, static_cast<size_t>(rd));
        window_len += static_cast<size_t>(rd);
        window[window_len] = '\0';

        for (const char* p = window; *p; p++) {
            if (string::strncmp(p, " running ", 9) == 0) {
                has_running = true;
                break;
            }
        }
        if (has_running) {
            break;
        }

        // Keep the tail as overlap for the next chunk
        size_t keep = window_len < 16 ? window_len : 16;
        string::memcpy(window, window + window_len - keep, keep);
        window_len = keep;
    }
    fs::close(f);

    EXPECT_FALSE(first_chunk);
    EXPECT_TRUE(has_running);
}

// --- snapshot_consistent_across_small_reads ---
// Proves: one open serves a single frozen snapshot regardless of how
// the reader chunks its reads, and EOF is reported cleanly.

TEST(sysstat, snapshot_consistent_across_small_reads) {
    fs::file* f = fs::open("/dev/sysinfo/mem", fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    char buf[512] = {};
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        char c = 0;
        ssize_t rd = fs::read(f, &c, 1);
        if (rd == 0) {
            break;
        }
        ASSERT_EQ(rd, static_cast<ssize_t>(1));
        buf[total++] = c;
    }
    buf[total] = '\0';

    // Reads past the end keep returning EOF
    char c = 0;
    EXPECT_EQ(fs::read(f, &c, 1), static_cast<ssize_t>(0));
    fs::close(f);

    EXPECT_EQ(count_lines(buf), static_cast<size_t>(4));
    uint64_t value = 0;
    EXPECT_TRUE(parse_labeled_u64(buf, "total_pages", &value));
}
