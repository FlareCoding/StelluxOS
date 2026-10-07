#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "trace/ktrace.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "exec/elf_arch.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "mm/heap.h"
#include "smp/smp.h"
#include "common/string.h"

TEST_SUITE(ktrace);

struct cpu_line_totals {
    uint64_t records;
    uint64_t lost;
};

struct stream_summary {
    bool     header_valid;
    bool     ended;
    bool     malformed;
    uint64_t session_start_ns;
    uint64_t stop_ns;
    uint64_t records;
    uint64_t lost;
    uint64_t out_of_order;
    uint64_t before_session;
    uint64_t bytes_after_end;
    uint64_t cpus_with_records_mask;
};

static constexpr uint64_t MISSING_STATUS_VALUE = ~0ull;
static constexpr uint64_t RING_RECORDS = 65536;

// Sessions here record only this id, so kernel events never mix into the counts
static constexpr uint16_t TEST_EVENT_ID = ktrace::EVENT_ID_COUNT - 1;
static constexpr uint64_t TEST_EVENT_MASK = 1ull << TEST_EVENT_ID;

static constexpr size_t SMALL_STREAM_BYTES = 64 * 1024;
static constexpr size_t FULL_RING_STREAM_BYTES = RING_RECORDS * sizeof(ktrace::trace_record) + SMALL_STREAM_BYTES;

static char g_status_text[4096];
alignas(64) static uint8_t g_stream_bytes[SMALL_STREAM_BYTES];

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

// Totals the `cpu<N> <records> <lost>` lines
static cpu_line_totals sum_cpu_lines() {
    cpu_line_totals totals = {};
    for (const char* line = g_status_text; *line; line = next_line(line)) {
        if (string::strncmp(line, "cpu", 3) != 0) {
            continue;
        }

        const char* field = line + 3;
        parse_u64(&field);
        field++;
        totals.records += parse_u64(&field);
        field++;
        totals.lost += parse_u64(&field);
    }

    return totals;
}

static void record_events(uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        ktrace::trace_record rec = {};
        rec.hdr.event_id = TEST_EVENT_ID;
        rec.payload[0] = i;
        ktrace::record_event(rec);
    }
}

static fs::file* open_reader(int32_t* err = nullptr) {
    return fs::open("/dev/ktrace/trace", fs::O_RDONLY, err);
}

static fs::file* start_session_with_reader() {
    fs::file* reader = open_reader();
    if (reader && ktrace::start(TEST_EVENT_MASK) != ktrace::OK) {
        fs::close(reader);
        return nullptr;
    }

    return reader;
}

static size_t read_until_end(fs::file* reader, uint8_t* buf, size_t cap) {
    size_t length = 0;
    while (length < cap) {
        ssize_t rc = fs::read(reader, buf + length, cap - length);
        if (rc <= 0) {
            break;
        }

        length += static_cast<size_t>(rc);
    }

    return length;
}

static bool header_matches_session(const ktrace::file_header* header) {
    return string::memcmp(header->magic, ktrace::FILE_MAGIC, sizeof(header->magic)) == 0 &&
           header->version == ktrace::FILE_VERSION &&
           header->header_size == sizeof(ktrace::file_header) &&
           header->arch == exec::ELF_EXPECTED_MACHINE &&
           header->cpu_count == smp::cpu_count() &&
           header->record_size == sizeof(ktrace::trace_record) &&
           header->session_start_ns > 0 &&
           header->event_mask == TEST_EVENT_MASK;
}

// Expects the record_events() payloads in order from 0
static stream_summary summarize_stream(const uint8_t* bytes, size_t length) {
    stream_summary summary = {};
    if (length < sizeof(ktrace::file_header)) {
        return summary;
    }

    const auto* header = reinterpret_cast<const ktrace::file_header*>(bytes);
    summary.header_valid = header_matches_session(header);
    summary.session_start_ns = header->session_start_ns;

    size_t pos = sizeof(ktrace::file_header);
    uint64_t next_payload = 0;

    while (pos + sizeof(ktrace::chunk_header) <= length) {
        const auto* chunk = reinterpret_cast<const ktrace::chunk_header*>(bytes + pos);
        pos += sizeof(ktrace::chunk_header);

        if (chunk->kind == ktrace::CHUNK_END) {
            summary.ended = true;
            summary.stop_ns = chunk->stop_ns;
            break;
        }

        uint64_t records_bytes = chunk->record_count * sizeof(ktrace::trace_record);
        if (chunk->kind != ktrace::CHUNK_RECORDS || chunk->cpu_id >= header->cpu_count ||
            pos + records_bytes > length) {
            summary.malformed = true;
            break;
        }

        const auto* records = reinterpret_cast<const ktrace::trace_record*>(bytes + pos);
        for (uint64_t i = 0; i < chunk->record_count; i++) {
            if (records[i].payload[0] != next_payload) {
                summary.out_of_order++;
            }

            if (records[i].hdr.timestamp < header->session_start_ns) {
                summary.before_session++;
            }

            next_payload = records[i].payload[0] + 1;
        }

        if (chunk->record_count > 0) {
            summary.cpus_with_records_mask |= 1ull << chunk->cpu_id;
        }

        summary.records += chunk->record_count;
        summary.lost += chunk->lost_records;
        pos += records_bytes;
    }

    summary.bytes_after_end = length - pos;
    return summary;
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

// Runs before any other test starts a session
TEST(ktrace, status_reports_idle_before_the_first_session) {
    ASSERT_TRUE(read_status());
    EXPECT_TRUE(status_state_is("idle"));
    EXPECT_EQ(status_value("session_start_ns"), 0u);
    EXPECT_EQ(status_value("session_stop_ns"), 0u);
    EXPECT_EQ(status_value("capacity"), RING_RECORDS);
    EXPECT_EQ(status_value("trace_open"), 0u);
    EXPECT_EQ(sum_cpu_lines().records, 0u);
}

TEST(ktrace, start_fails_without_a_reader) {
    EXPECT_EQ(ktrace::start(TEST_EVENT_MASK), ktrace::ERR_NO_READER);
    EXPECT_EQ(write_to_control("start"), fs::ERR_PIPE);
}

TEST(ktrace, a_second_reader_is_refused) {
    fs::file* reader = open_reader();
    ASSERT_NOT_NULL(reader);

    int32_t err = fs::OK;
    EXPECT_NULL(open_reader(&err));
    EXPECT_EQ(err, fs::ERR_BUSY);

    fs::close(reader);
}

TEST(ktrace, start_fails_while_a_session_is_recording) {
    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    EXPECT_EQ(ktrace::start(TEST_EVENT_MASK), ktrace::ERR_BUSY);
    EXPECT_EQ(ktrace::stop(), ktrace::OK);

    fs::close(reader);
}

TEST(ktrace, stop_fails_when_no_session_is_recording) {
    EXPECT_EQ(ktrace::stop(), ktrace::ERR_NOT_RECORDING);
}

TEST(ktrace, a_reader_streams_only_one_session) {
    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    EXPECT_EQ(ktrace::start(TEST_EVENT_MASK), ktrace::ERR_NO_READER);
    fs::close(reader);

    reader = start_session_with_reader();
    EXPECT_NOT_NULL(reader);

    EXPECT_EQ(ktrace::stop(), ktrace::OK);
    fs::close(reader);
}

TEST(ktrace, closing_the_reader_stops_the_session) {
    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    fs::close(reader);

    ASSERT_TRUE(read_status());
    EXPECT_TRUE(status_state_is("stopped"));
    EXPECT_EQ(status_value("trace_open"), 0u);
}

TEST(ktrace, writing_start_and_stop_to_control_drives_a_session) {
    fs::file* reader = open_reader();
    ASSERT_NOT_NULL(reader);

    ASSERT_EQ(write_to_control("start"), 5);
    EXPECT_EQ(ktrace::start(TEST_EVENT_MASK), ktrace::ERR_BUSY);
    EXPECT_EQ(write_to_control("stop"), 4);
    EXPECT_EQ(ktrace::stop(), ktrace::ERR_NOT_RECORDING);

    fs::close(reader);
}

TEST(ktrace, control_accepts_a_command_ending_in_a_newline) {
    fs::file* reader = open_reader();
    ASSERT_NOT_NULL(reader);

    ASSERT_EQ(write_to_control("start\n"), 6);
    EXPECT_EQ(write_to_control("stop\n"), 5);

    fs::close(reader);
}

TEST(ktrace, control_refuses_start_while_a_session_is_recording) {
    fs::file* reader = open_reader();
    ASSERT_NOT_NULL(reader);

    ASSERT_EQ(write_to_control("start"), 5);
    EXPECT_EQ(write_to_control("start"), fs::ERR_BUSY);
    EXPECT_EQ(write_to_control("stop"), 4);

    fs::close(reader);
}

TEST(ktrace, control_refuses_stop_when_no_session_is_recording) {
    EXPECT_EQ(write_to_control("stop"), fs::ERR_INVAL);
}

TEST(ktrace, control_refuses_unknown_commands) {
    EXPECT_EQ(write_to_control("restart"), fs::ERR_INVAL);
    EXPECT_EQ(write_to_control("st"), fs::ERR_INVAL);
    EXPECT_EQ(write_to_control("start\n\n"), fs::ERR_INVAL);
}

TEST(ktrace, the_stream_holds_the_session_and_ends_with_its_stop_time) {
    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    record_events(3);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);

    size_t length = read_until_end(reader, g_stream_bytes, sizeof(g_stream_bytes));
    fs::close(reader);

    stream_summary summary = summarize_stream(g_stream_bytes, length);
    EXPECT_TRUE(summary.header_valid);
    EXPECT_FALSE(summary.malformed);
    EXPECT_EQ(length % 64, 0u);

    EXPECT_EQ(summary.records, 3u);
    EXPECT_EQ(summary.out_of_order, 0u);
    EXPECT_EQ(summary.before_session, 0u);
    EXPECT_EQ(summary.lost, 0u);

    EXPECT_TRUE(summary.ended);
    EXPECT_GE(summary.stop_ns, summary.session_start_ns);
    EXPECT_EQ(summary.bytes_after_end, 0u);
}

TEST(ktrace, status_counts_the_records_and_bytes_streamed) {
    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    record_events(3);

    ASSERT_TRUE(read_status());
    EXPECT_TRUE(status_state_is("recording"));
    EXPECT_EQ(status_value("trace_open"), 1u);
    EXPECT_GT(status_value("session_start_ns"), 0u);
    EXPECT_EQ(status_value("session_stop_ns"), 0u);

    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    size_t length = read_until_end(reader, g_stream_bytes, sizeof(g_stream_bytes));

    ASSERT_TRUE(read_status());
    EXPECT_TRUE(status_state_is("stopped"));
    EXPECT_EQ(sum_cpu_lines().records, 3u);
    EXPECT_EQ(sum_cpu_lines().lost, 0u);
    EXPECT_EQ(status_value("bytes_streamed"), length);
    EXPECT_GE(status_value("session_stop_ns"), status_value("session_start_ns"));

    fs::close(reader);
}

TEST(ktrace, a_read_while_recording_returns_the_records_written_so_far) {
    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    record_events(3);
    ssize_t first = fs::read(reader, g_stream_bytes, sizeof(g_stream_bytes));
    ASSERT_TRUE(first > 0);

    stream_summary so_far = summarize_stream(g_stream_bytes, static_cast<size_t>(first));
    EXPECT_TRUE(so_far.header_valid);
    EXPECT_EQ(so_far.records, 3u);
    EXPECT_FALSE(so_far.ended);

    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    size_t rest = read_until_end(reader, g_stream_bytes + first, sizeof(g_stream_bytes) - first);
    fs::close(reader);

    stream_summary whole = summarize_stream(g_stream_bytes, static_cast<size_t>(first) + rest);
    EXPECT_EQ(whole.records, 3u);
    EXPECT_TRUE(whole.ended);
    EXPECT_EQ(whole.bytes_after_end, 0u);
}

TEST(ktrace, reading_past_the_end_returns_end_of_file) {
    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    read_until_end(reader, g_stream_bytes, sizeof(g_stream_bytes));
    EXPECT_EQ(fs::read(reader, g_stream_bytes, sizeof(g_stream_bytes)), 0);

    fs::close(reader);
}

TEST(ktrace, a_read_too_small_for_a_chunk_is_refused) {
    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    constexpr size_t too_small = sizeof(ktrace::chunk_header) + sizeof(ktrace::trace_record) - 1;
    EXPECT_EQ(fs::read(reader, g_stream_bytes, too_small), fs::ERR_INVAL);

    EXPECT_EQ(ktrace::stop(), ktrace::OK);
    fs::close(reader);
}

TEST(ktrace, start_empties_the_rings) {
    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    record_events(2);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    fs::close(reader);

    reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    size_t length = read_until_end(reader, g_stream_bytes, sizeof(g_stream_bytes));
    fs::close(reader);

    stream_summary summary = summarize_stream(g_stream_bytes, length);
    EXPECT_EQ(summary.records, 0u);
    EXPECT_TRUE(summary.ended);
}

TEST(ktrace, events_outside_the_session_mask_are_dropped) {
    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    record_events(2);

    ktrace::trace_record unselected = {};
    unselected.hdr.event_id = TEST_EVENT_ID - 1;
    ktrace::record_event(unselected);

    ktrace::trace_record out_of_range = {};
    out_of_range.hdr.event_id = ktrace::EVENT_ID_COUNT;
    ktrace::record_event(out_of_range);

    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    size_t length = read_until_end(reader, g_stream_bytes, sizeof(g_stream_bytes));
    fs::close(reader);

    EXPECT_EQ(summarize_stream(g_stream_bytes, length).records, 2u);
}

TEST(ktrace, events_outside_a_session_are_dropped) {
    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    ASSERT_EQ(ktrace::stop(), ktrace::OK);
    record_events(5);

    size_t length = read_until_end(reader, g_stream_bytes, sizeof(g_stream_bytes));
    fs::close(reader);

    EXPECT_EQ(summarize_stream(g_stream_bytes, length).records, 0u);
}

TEST(ktrace, a_full_ring_drops_new_records_and_counts_them) {
    constexpr uint64_t dropped = 10;
    auto* stream = static_cast<uint8_t*>(heap::uzalloc(FULL_RING_STREAM_BYTES));
    ASSERT_NOT_NULL(stream);

    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    record_events(RING_RECORDS + dropped);
    ASSERT_EQ(ktrace::stop(), ktrace::OK);

    size_t length = read_until_end(reader, stream, FULL_RING_STREAM_BYTES);
    stream_summary summary = summarize_stream(stream, length);
    EXPECT_EQ(summary.records, RING_RECORDS);
    EXPECT_EQ(summary.lost, dropped);
    EXPECT_EQ(summary.out_of_order, 0u);
    EXPECT_TRUE(summary.ended);

    ASSERT_TRUE(read_status());
    EXPECT_EQ(sum_cpu_lines().lost, dropped);

    fs::close(reader);
    heap::ufree(stream);
}

static constexpr uint32_t RECORDING_CPU = 1;
static constexpr uint64_t RECORDING_CPU_EVENTS = 5;

static sync::atomic<uint32_t> g_recorder_done;

static void record_on_cpu_task(void*) {
    record_events(RECORDING_CPU_EVENTS);
    g_recorder_done.store_release(1);
    sched::exit(0);
}

// CPU 0 records nothing, so its ring yields no chunk
TEST(ktrace, the_stream_reads_past_a_cpu_that_recorded_nothing) {
    ASSERT_TRUE(smp::cpu_count() > RECORDING_CPU);
    g_recorder_done.store_relaxed(0);

    fs::file* reader = start_session_with_reader();
    ASSERT_NOT_NULL(reader);

    bool created = false;
    RUN_ELEVATED({
        sched::task* t = sched::create_kernel_task(record_on_cpu_task, nullptr, "ktrace_recorder");
        if (t) {
            sched::enqueue_on(t, RECORDING_CPU);
            created = true;
        }
    });
    ASSERT_TRUE(created);
    EXPECT_TRUE(test_helpers::spin_wait(g_recorder_done));
    ASSERT_EQ(ktrace::stop(), ktrace::OK);

    size_t length = read_until_end(reader, g_stream_bytes, sizeof(g_stream_bytes));
    fs::close(reader);

    stream_summary summary = summarize_stream(g_stream_bytes, length);
    EXPECT_EQ(summary.records, RECORDING_CPU_EVENTS);
    EXPECT_EQ(summary.cpus_with_records_mask, 1ull << RECORDING_CPU);
    EXPECT_EQ(summary.out_of_order, 0u);
    EXPECT_TRUE(summary.ended);
}
