#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "trace/ktrace.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "common/string.h"

TEST_SUITE(ktrace);

struct cpu_line_totals {
    uint64_t kept;
    uint32_t wrapped_rings;
};

static constexpr uint64_t MISSING_STATUS_VALUE = ~0ull;
static constexpr uint64_t RING_WRAPPED_FLAG = 1 << 0;

static char g_status_text[4096];

static bool read_status() {
    fs::file* status = fs::open("/dev/ktrace/status", fs::O_RDONLY);
    if (!status) {
        return false;
    }

    size_t length = 0;
    while (length < sizeof(g_status_text) - 1) {
        ssize_t rc = fs::read(status, g_status_text + length, sizeof(g_status_text) - 1 - length);
        if (rc <= 0) {
            break;
        }

        length += static_cast<size_t>(rc);
    }

    fs::close(status);
    g_status_text[length] = '\0';
    return length > 0;
}

static const char* next_line(const char* line) {
    while (*line && *line != '\n') {
        line++;
    }

    return *line ? line + 1 : line;
}

static uint64_t parse_u64(const char** text) {
    uint64_t value = 0;
    while (**text >= '0' && **text <= '9') {
        value = value * 10 + static_cast<uint64_t>(**text - '0');
        (*text)++;
    }

    return value;
}

static bool status_state_is(const char* state) {
    const char* prefix = "state ";
    size_t prefix_length = string::strlen(prefix);
    size_t state_length = string::strlen(state);

    return string::strncmp(g_status_text, prefix, prefix_length) == 0 &&
           string::strncmp(g_status_text + prefix_length, state, state_length) == 0 &&
           g_status_text[prefix_length + state_length] == '\n';
}

static uint64_t status_value(const char* key) {
    size_t key_length = string::strlen(key);
    for (const char* line = g_status_text; *line; line = next_line(line)) {
        if (string::strncmp(line, key, key_length) == 0 && line[key_length] == ' ') {
            const char* value = line + key_length + 1;
            return parse_u64(&value);
        }
    }

    return MISSING_STATUS_VALUE;
}

// Totals the `cpu<N> <kept> <flags>` lines
static cpu_line_totals sum_cpu_lines() {
    cpu_line_totals totals = {};
    for (const char* line = g_status_text; *line; line = next_line(line)) {
        if (string::strncmp(line, "cpu", 3) != 0) {
            continue;
        }

        const char* field = line + 3;
        parse_u64(&field);
        field++;
        totals.kept += parse_u64(&field);
        field++;
        if (parse_u64(&field) & RING_WRAPPED_FLAG) {
            totals.wrapped_rings++;
        }
    }

    return totals;
}

static void record_events(uint64_t count) {
    ktrace::trace_record rec = {};
    for (uint64_t i = 0; i < count; i++) {
        ktrace::record_event(rec);
    }
}

// Runs before any other test starts a session
TEST(ktrace, status_reports_idle_before_the_first_session) {
    ASSERT_TRUE(read_status());
    EXPECT_TRUE(status_state_is("idle"));
    EXPECT_EQ(status_value("session_start_ns"), 0u);
    EXPECT_EQ(status_value("session_stop_ns"), 0u);
    EXPECT_EQ(status_value("capacity"), 65536u);
    EXPECT_EQ(sum_cpu_lines().kept, 0u);
}

TEST(ktrace, start_fails_while_a_session_is_recording) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    EXPECT_EQ(ktrace::start(), ktrace::ERR_BUSY);
    EXPECT_EQ(ktrace::stop(), ktrace::OK);
}

TEST(ktrace, stop_fails_when_no_session_is_recording) {
    EXPECT_EQ(ktrace::stop(), ktrace::ERR_NOT_RECORDING);
}

TEST(ktrace, a_stopped_session_can_be_started_again) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    EXPECT_EQ(ktrace::start(), ktrace::OK);
    EXPECT_EQ(ktrace::stop(), ktrace::OK);
}

static ssize_t write_to_control(const char* command) {
    fs::file* control = fs::open("/dev/ktrace/control", fs::O_WRONLY);
    if (!control) {
        return fs::ERR_NOENT;
    }

    ssize_t rc = fs::write(control, command, string::strlen(command));
    fs::close(control);
    return rc;
}

TEST(ktrace, writing_start_and_stop_to_control_drives_a_session) {
    ASSERT_EQ(write_to_control("start"), 5);
    EXPECT_EQ(ktrace::start(), ktrace::ERR_BUSY);
    EXPECT_EQ(write_to_control("stop"), 4);
    EXPECT_EQ(ktrace::stop(), ktrace::ERR_NOT_RECORDING);
}

TEST(ktrace, control_accepts_a_command_ending_in_a_newline) {
    ASSERT_EQ(write_to_control("start\n"), 6);
    EXPECT_EQ(write_to_control("stop\n"), 5);
}

TEST(ktrace, control_refuses_start_while_a_session_is_recording) {
    ASSERT_EQ(write_to_control("start"), 5);
    EXPECT_EQ(write_to_control("start"), fs::ERR_BUSY);
    EXPECT_EQ(write_to_control("stop"), 4);
}

TEST(ktrace, control_refuses_stop_when_no_session_is_recording) {
    EXPECT_EQ(write_to_control("stop"), fs::ERR_INVAL);
}

TEST(ktrace, control_refuses_unknown_commands) {
    EXPECT_EQ(write_to_control("restart"), fs::ERR_INVAL);
    EXPECT_EQ(write_to_control("st"), fs::ERR_INVAL);
    EXPECT_EQ(write_to_control("start\n\n"), fs::ERR_INVAL);
}

TEST(ktrace, status_counts_records_while_recording_and_after_stop) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    record_events(3);

    ASSERT_TRUE(read_status());
    EXPECT_TRUE(status_state_is("recording"));
    EXPECT_EQ(sum_cpu_lines().kept, 3u);
    EXPECT_GT(status_value("session_start_ns"), 0u);
    EXPECT_EQ(status_value("session_stop_ns"), 0u);

    ASSERT_EQ(ktrace::stop(), ktrace::OK);

    ASSERT_TRUE(read_status());
    EXPECT_TRUE(status_state_is("stopped"));
    EXPECT_EQ(sum_cpu_lines().kept, 3u);
    EXPECT_GE(status_value("session_stop_ns"), status_value("session_start_ns"));
}

TEST(ktrace, start_empties_the_rings) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    record_events(2);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    ASSERT_EQ(ktrace::start(), ktrace::OK);

    ASSERT_TRUE(read_status());
    EXPECT_EQ(sum_cpu_lines().kept, 0u);
    EXPECT_EQ(ktrace::stop(), ktrace::OK);
}

TEST(ktrace, events_outside_a_session_are_dropped) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    record_events(5);

    ASSERT_TRUE(read_status());
    EXPECT_EQ(sum_cpu_lines().kept, 0u);
}

TEST(ktrace, status_flags_a_ring_that_wrapped) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    record_events(65536 + 10);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);

    ASSERT_TRUE(read_status());
    cpu_line_totals totals = sum_cpu_lines();
    EXPECT_EQ(totals.kept, 65536u);
    EXPECT_EQ(totals.wrapped_rings, 1u);
}
