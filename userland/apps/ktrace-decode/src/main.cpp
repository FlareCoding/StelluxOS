#include "database.hpp"
#include "event_tables.hpp"
#include "health.hpp"
#include "session_tables.hpp"
#include "text.hpp"
#include "trace_file.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

struct phase_durations {
    uint64_t read_ns;
    uint64_t order_ns;
    uint64_t write_ns;
};

constexpr const char* USAGE = "usage: ktrace-decode FILE -o DB";

static uint64_t monotonic_ns() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ull + static_cast<uint64_t>(now.tv_nsec);
}

static double to_milliseconds(uint64_t ns) {
    return static_cast<double>(ns) / 1e6;
}

static double to_seconds(uint64_t ns) {
    return static_cast<double>(ns) / 1e9;
}

static bool parse_arguments(int argc, char** argv, const char*& trace_path, const char*& database_path) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc && !database_path) {
            database_path = argv[++i];
        } else if (argv[i][0] != '-' && !trace_path) {
            trace_path = argv[i];
        } else {
            return false;
        }
    }

    return trace_path && database_path;
}

static std::string describe_event_rows(const record_counts& counts) {
    std::string rows;
    uint64_t unknown = counts.with_event_id_out_of_range;
    for (uint16_t event_id = 0; event_id < ktrace::EVENT_ID_COUNT; event_id++) {
        const ktrace::event_description* event = find_event_description(event_id);
        if (!event) {
            unknown += counts.by_event_id[event_id];
            continue;
        }

        rows += format_string("%s%s %" PRIu64, rows.empty() ? "" : ", ", event->name, counts.by_event_id[event_id]);
    }

    if (unknown > 0) {
        rows += format_string("%sunknown_records %" PRIu64, rows.empty() ? "" : ", ", unknown);
    }

    return rows;
}

static void print_summary(const char* database_path, const trace_file& file, const record_counts& counts,
                          const std::vector<health_check>& checks, const phase_durations& durations) {
    const ktrace::file_header& header = file.header;
    const char* arch = arch_name(header.arch);
    printf("wrote %s\n", database_path);

    if (file.has_end_chunk) {
        printf("  session  %.3f s", to_seconds(file.stop_ns - header.session_start_ns));
    } else {
        printf("  session  cut short");
    }

    printf(" on %u CPUs (%s), %" PRIu64 " records, %" PRIu64 " lost\n", header.cpu_count,
           arch ? arch : "unknown arch", counts.in_session, file.lost_records);
    printf("  rows     %s\n", describe_event_rows(counts).c_str());

    const char* label = "health";
    for (const health_check& check : checks) {
        printf("  %-7s  %-6s  %-16s %s\n", label, check.passed ? "ok" : "FAILED", check.name,
               check.detail.c_str());
        label = "";
    }

    printf("  time     read %.1f ms, order %.1f ms, write %.1f ms\n", to_milliseconds(durations.read_ns),
           to_milliseconds(durations.order_ns), to_milliseconds(durations.write_ns));
}

static bool write_database(const char* database_path, const trace_file& file, const timeline& order,
                           const std::vector<health_check>& checks, std::string& error) {
    database db;
    if (open_database(database_path, db, error) &&
        write_session_tables(db, file, order.counts, checks, error) &&
        write_event_tables(db, file, order, error) &&
        finish_database(db, error)) {
        return true;
    }

    std::string discard_error;
    if (!discard_database(db, discard_error)) {
        error += "\nktrace-decode: " + discard_error;
    }

    return false;
}

int main(int argc, char** argv) {
    const char* trace_path = nullptr;
    const char* database_path = nullptr;
    if (!parse_arguments(argc, argv, trace_path, database_path)) {
        fprintf(stderr, "%s\n", USAGE);
        return 1;
    }

    if (strcmp(trace_path, database_path) == 0) {
        fprintf(stderr, "ktrace-decode: the database would replace the trace it is decoded from\n");
        return 1;
    }

    std::string error;
    trace_file file;
    uint64_t read_start_ns = monotonic_ns();
    if (!load_trace_file(trace_path, file, error)) {
        fprintf(stderr, "ktrace-decode: %s\n", error.c_str());
        return 1;
    }

    uint64_t order_start_ns = monotonic_ns();
    timeline order = build_timeline(file);
    std::vector<health_check> checks = check_health(file, order.counts);

    uint64_t write_start_ns = monotonic_ns();
    if (!write_database(database_path, file, order, checks, error)) {
        fprintf(stderr, "ktrace-decode: %s\n", error.c_str());
        return 1;
    }

    uint64_t done_ns = monotonic_ns();
    phase_durations durations = {order_start_ns - read_start_ns, write_start_ns - order_start_ns,
                                 done_ns - write_start_ns};
    print_summary(database_path, file, order.counts, checks, durations);

    return 0;
}
