#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "trace/ktrace.h"
#include "exec/elf_arch.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "mm/heap.h"
#include "smp/smp.h"
#include "common/string.h"

TEST_SUITE(ktrace);

struct cpu_line_totals {
    uint64_t kept;
    uint32_t wrapped_rings;
};

static constexpr uint64_t MISSING_STATUS_VALUE = ~0ull;
static constexpr uint64_t RING_RECORDS = 65536;

// Odd sizes, so reads cross section and record boundaries
static constexpr size_t SMALL_READ_BYTES = 100;
static constexpr size_t LARGE_READ_BYTES = 64 * 1024 + 7;

static char g_status_text[4096];
alignas(64) static uint8_t g_trace_bytes[4096];

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
        if (parse_u64(&field) & ktrace::CPU_FLAG_RING_WRAPPED) {
            totals.wrapped_rings++;
        }
    }

    return totals;
}

static void record_events(uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        ktrace::trace_record rec = {};
        rec.payload[0] = i;
        ktrace::record_event(rec);
    }
}

static int32_t try_open_trace() {
    int32_t err = fs::OK;
    fs::file* trace = fs::open("/dev/ktrace/trace", fs::O_RDONLY, &err);
    if (!trace) {
        return err;
    }

    fs::close(trace);
    return fs::OK;
}

static size_t read_trace(uint8_t* buf, size_t cap, size_t chunk) {
    fs::file* trace = fs::open("/dev/ktrace/trace", fs::O_RDONLY);
    if (!trace) {
        return 0;
    }

    size_t length = 0;
    while (length < cap) {
        size_t room = cap - length;
        ssize_t rc = fs::read(trace, buf + length, room < chunk ? room : chunk);
        if (rc <= 0) {
            break;
        }

        length += static_cast<size_t>(rc);
    }

    fs::close(trace);
    return length;
}

// Runs before any other test starts a session
TEST(ktrace, status_reports_idle_before_the_first_session) {
    ASSERT_TRUE(read_status());
    EXPECT_TRUE(status_state_is("idle"));
    EXPECT_EQ(status_value("session_start_ns"), 0u);
    EXPECT_EQ(status_value("session_stop_ns"), 0u);
    EXPECT_EQ(status_value("capacity"), RING_RECORDS);
    EXPECT_EQ(sum_cpu_lines().kept, 0u);
}

TEST(ktrace, opening_the_trace_before_the_first_session_fails_with_nodata) {
    EXPECT_EQ(try_open_trace(), fs::ERR_NODATA);
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
    record_events(RING_RECORDS + 10);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);

    ASSERT_TRUE(read_status());
    cpu_line_totals totals = sum_cpu_lines();
    EXPECT_EQ(totals.kept, RING_RECORDS);
    EXPECT_EQ(totals.wrapped_rings, 1u);
}

TEST(ktrace, opening_the_trace_while_recording_fails_with_busy) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    EXPECT_EQ(try_open_trace(), fs::ERR_BUSY);
    EXPECT_EQ(ktrace::stop(), ktrace::OK);
}

TEST(ktrace, start_fails_while_the_trace_is_open) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);

    fs::file* trace = fs::open("/dev/ktrace/trace", fs::O_RDONLY);
    ASSERT_NOT_NULL(trace);
    EXPECT_EQ(ktrace::start(), ktrace::ERR_BUSY);
    EXPECT_TRUE(read_status());
    EXPECT_EQ(status_value("trace_open"), 1u);

    fs::close(trace);
    EXPECT_EQ(ktrace::start(), ktrace::OK);
    EXPECT_EQ(ktrace::stop(), ktrace::OK);
}

TEST(ktrace, the_trace_describes_the_stopped_session) {
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    record_events(3);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);

    size_t length = read_trace(g_trace_bytes, sizeof(g_trace_bytes), SMALL_READ_BYTES);
    ASSERT_TRUE(length >= sizeof(ktrace::file_header));

    const auto* header = reinterpret_cast<const ktrace::file_header*>(g_trace_bytes);
    EXPECT_EQ(string::memcmp(header->magic, ktrace::FILE_MAGIC, sizeof(header->magic)), 0);
    EXPECT_EQ(header->version, ktrace::FILE_VERSION);
    EXPECT_EQ(header->header_size, sizeof(ktrace::file_header));
    EXPECT_EQ(header->arch, exec::ELF_EXPECTED_MACHINE);
    EXPECT_EQ(header->cpu_count, smp::cpu_count());
    EXPECT_EQ(header->record_size, sizeof(ktrace::trace_record));
    EXPECT_EQ(header->cpu_table_offset, sizeof(ktrace::file_header));
    EXPECT_GE(header->session_stop_ns, header->session_start_ns);

    const auto* entries = reinterpret_cast<const ktrace::cpu_table_entry*>(g_trace_bytes + header->cpu_table_offset);
    const ktrace::cpu_table_entry* recorded = nullptr;
    uint64_t next_records_offset = entries[0].records_offset;
    uint64_t total_records = 0;
    EXPECT_EQ(next_records_offset % 64, 0u);

    for (uint32_t cpu = 0; cpu < header->cpu_count; cpu++) {
        EXPECT_EQ(entries[cpu].cpu_id, cpu);
        EXPECT_EQ(entries[cpu].records_offset, next_records_offset);
        next_records_offset += entries[cpu].record_count * sizeof(ktrace::trace_record);
        total_records += entries[cpu].record_count;
        if (entries[cpu].record_count > 0) {
            recorded = &entries[cpu];
        }
    }

    EXPECT_EQ(total_records, 3u);
    EXPECT_EQ(length, next_records_offset);
    ASSERT_NOT_NULL(recorded);

    const auto* records = reinterpret_cast<const ktrace::trace_record*>(g_trace_bytes + recorded->records_offset);
    for (uint64_t i = 0; i < 3; i++) {
        EXPECT_EQ(records[i].payload[0], i);
        EXPECT_GE(records[i].hdr.timestamp, header->session_start_ns);
    }
}

TEST(ktrace, the_trace_lists_a_wrapped_ring_oldest_first) {
    constexpr uint64_t overwritten = 10;
    ASSERT_EQ(ktrace::start(), ktrace::OK);
    record_events(RING_RECORDS + overwritten);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);

    size_t cap = sizeof(g_trace_bytes) + RING_RECORDS * sizeof(ktrace::trace_record);
    auto* trace = static_cast<uint8_t*>(heap::uzalloc(cap));
    ASSERT_NOT_NULL(trace);

    read_trace(trace, cap, LARGE_READ_BYTES);
    const auto* header = reinterpret_cast<const ktrace::file_header*>(trace);
    const auto* entries = reinterpret_cast<const ktrace::cpu_table_entry*>(trace + header->cpu_table_offset);

    const ktrace::cpu_table_entry* wrapped = nullptr;
    for (uint32_t cpu = 0; cpu < header->cpu_count; cpu++) {
        if (entries[cpu].flags & ktrace::CPU_FLAG_RING_WRAPPED) {
            wrapped = &entries[cpu];
        }
    }

    uint64_t out_of_order = 0;
    if (wrapped) {
        const auto* records = reinterpret_cast<const ktrace::trace_record*>(trace + wrapped->records_offset);
        for (uint64_t i = 0; i < wrapped->record_count; i++) {
            if (records[i].payload[0] != overwritten + i) {
                out_of_order++;
            }
        }
    }

    EXPECT_NOT_NULL(wrapped);
    EXPECT_EQ(wrapped ? wrapped->record_count : 0, RING_RECORDS);
    EXPECT_EQ(out_of_order, 0u);
    heap::ufree(trace);
}
