/* Host tests for `ktrace_model`: `ktrace-decode` SQLite schema loading, NULL and `pid == 0`
 * handling, full musl syscall naming, zoomed window clipping, timeline bars, and flame spans. */

#include "extensions/perf_binaries/ktrace_model.h"

#include <sqlite3.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace sheen;

static constexpr uint32_t MAX_FAILURE_PRINTS = 12;

static constexpr const char* FULL_KTRACE_DECODE_SCHEMA = R"sql(
CREATE TABLE names (name_id INTEGER PRIMARY KEY, name TEXT NOT NULL);
CREATE TABLE session (format_version INTEGER NOT NULL, arch TEXT, elf_machine INTEGER NOT NULL, cpu_count INTEGER NOT NULL, boot_unix_ns INTEGER, start_ns INTEGER NOT NULL, stop_ns INTEGER, event_mask INTEGER NOT NULL, records INTEGER NOT NULL, records_before_start INTEGER NOT NULL, lost_records INTEGER NOT NULL, file_bytes INTEGER NOT NULL);
CREATE TABLE cpus (cpu INTEGER PRIMARY KEY, records INTEGER NOT NULL, records_before_start INTEGER NOT NULL, lost_records INTEGER NOT NULL);
CREATE TABLE health (name TEXT PRIMARY KEY, passed INTEGER NOT NULL, detail TEXT NOT NULL);
CREATE TABLE syscall (ts_ns INTEGER NOT NULL, cpu INTEGER NOT NULL, duration_ns INTEGER NOT NULL, result INTEGER NOT NULL, number INTEGER NOT NULL, tid INTEGER NOT NULL, pid INTEGER NOT NULL);
CREATE TABLE page_fault (ts_ns INTEGER NOT NULL, cpu INTEGER NOT NULL, address INTEGER NOT NULL, flags INTEGER NOT NULL, duration_ns INTEGER NOT NULL, result INTEGER NOT NULL, tid INTEGER NOT NULL);
CREATE TABLE cpu_slices (cpu INTEGER NOT NULL, tid INTEGER NOT NULL, start_ns INTEGER NOT NULL, end_ns INTEGER);
CREATE TABLE thread_states (tid INTEGER NOT NULL, state TEXT NOT NULL, start_ns INTEGER NOT NULL, end_ns INTEGER, cpu INTEGER, waker_tid INTEGER, switch_reason INTEGER, syscall_number INTEGER, in_page_fault INTEGER);
CREATE TABLE threads (tid INTEGER PRIMARY KEY, kind INTEGER, pid INTEGER, first_ns INTEGER NOT NULL, last_ns INTEGER NOT NULL, exit_ns INTEGER);
CREATE TABLE thread_names (tid INTEGER NOT NULL, name_id INTEGER NOT NULL, first_ns INTEGER NOT NULL, last_ns INTEGER NOT NULL);
CREATE TABLE processes (pid INTEGER PRIMARY KEY, leader_name_id INTEGER, threads INTEGER NOT NULL, first_ns INTEGER NOT NULL, last_ns INTEGER NOT NULL);
)sql";

static uint64_t g_checks = 0;
static uint64_t g_failures = 0;

static void check(bool holds, const char* description) {
    g_checks++;
    if (!holds && ++g_failures <= MAX_FAILURE_PRINTS) {
        std::printf("  FAIL: %s\n", description);
    }
}

static std::string create_temp_path(const char* pattern) {
    std::string temp_template(pattern);
    int fd = mkstemp(temp_template.data());
    if (fd < 0) {
        std::exit(1);
    }

    close(fd);
    return temp_template;
}

static void exec_sql_or_die(sqlite3* db, const char* sql) {
    char* err_msg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::printf("  SQLite setup error: %s\n", err_msg ? err_msg : "unknown");
        sqlite3_free(err_msg);
        std::exit(1);
    }
}

static void test_magic_probes_and_validation() {
    const uint8_t raw_ktrace[] = { 'S', 'T', 'L', 'X', 'K', 'T', 'R', 'C', 1, 0 };
    const uint8_t sqlite_hdr[16] = {
        'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f', 'o', 'r', 'm', 'a', 't', ' ', '3', '\0'
    };

    check(is_ktrace_capture(raw_ktrace) && !is_ktrace_capture(std::span<const uint8_t>(raw_ktrace, 7)),
          "detects STLXKTRC magic and rejects short prefixes");
    check(is_sqlite_database(sqlite_hdr) && !is_sqlite_database(std::span<const uint8_t>(sqlite_hdr, 15)),
          "detects SQLite 3 magic and rejects short prefixes");

    std::string raw_path = create_temp_path("/tmp/sheen-raw-ktrace-XXXXXX");
    FILE* raw_file = std::fopen(raw_path.c_str(), "wb");
    std::fwrite(raw_ktrace, 1, sizeof(raw_ktrace), raw_file);
    std::fclose(raw_file);

    ktrace_profile_model model;
    std::string error;
    check(!load_ktrace_database(raw_path, model, error) && error.find("ktrace-decode") != std::string::npos,
          "rejects raw .ktrace capture with decode hint");
    unlink(raw_path.c_str());

    std::string neg_path = create_temp_path("/tmp/sheen-neg-ktrace-XXXXXX");
    sqlite3* db = nullptr;
    sqlite3_open(neg_path.c_str(), &db);
    exec_sql_or_die(db, FULL_KTRACE_DECODE_SCHEMA);
    exec_sql_or_die(db, "INSERT INTO session VALUES (1, 'x86_64', 62, -1, NULL, 0, 100, -1, 0, 0, 0, 64);");
    sqlite3_close(db);

    check(!load_ktrace_database(neg_path, model, error) && model.session.cpu_count == 0,
          "rejects negative cpu_count and resets output model");
    unlink(neg_path.c_str());
}

static void test_sqlite_decode_schema_and_invariants() {
    std::string db_path = create_temp_path("/tmp/sheen-ktrace-db-XXXXXX");
    sqlite3* db = nullptr;
    sqlite3_open(db_path.c_str(), &db);
    exec_sql_or_die(db, FULL_KTRACE_DECODE_SCHEMA);
    exec_sql_or_die(db, R"sql(
        INSERT INTO names VALUES (1, 'idle/0'), (2, 'idle/1'), (3, 'stlxdm-init'),
                                 (4, 'stlxdm'), (5, 'ktrace'), (6, 'khubd');
        INSERT INTO session VALUES (1, 'x86_64', 62, 2, NULL, 1000, 5000, -1, 42, 0, 0, 4096);
        INSERT INTO cpus VALUES (0, 25, 0, 0), (1, 17, 0, 0);
        INSERT INTO health VALUES ('file_complete', 1, 'ok'), ('switch_chain', 1, 'ok');

        INSERT INTO threads VALUES (1,3,0,1000,5000,NULL), (2,3,0,1000,5000,NULL), (12,2,0,1000,5000,NULL),
                                   (104,1,104,1000,5000,NULL), (200,1,200,1000,5000,NULL);
        INSERT INTO thread_names VALUES (1,1,1000,5000), (2,2,1000,5000), (12,6,1000,5000),
                                        (104,3,1000,1500), (104,4,1501,5000), (200,5,1000,5000);
        INSERT INTO processes VALUES (104, 4, 1, 1000, 5000), (200, 5, 1, 1000, 5000);
        INSERT INTO cpu_slices VALUES (0,104,1000,2000), (0,12,2000,2400), (0,1,2400,3000),
                                      (0,104,3000,4500), (0,1,4500,5000), (1,200,1000,1800), (1,2,1800,5000);
        INSERT INTO thread_states VALUES
            (12,'running',2000,2400,0,NULL,NULL,NULL,0), (104,'running',1000,2000,0,NULL,NULL,NULL,0),
            (104,'sleeping',2000,2800,NULL,NULL,NULL,1030,0), (104,'runnable',2800,3000,NULL,12,NULL,NULL,0),
            (104,'running',3000,4500,0,NULL,NULL,NULL,0), (104,'sleeping',4500,5000,NULL,NULL,NULL,NULL,0),
            (200,'running',1000,1800,1,NULL,NULL,NULL,0), (200,'sleeping',1800,5000,NULL,NULL,NULL,0,0);
        INSERT INTO syscall VALUES (3100,0,1200,0,1030,104,104), (4200,0,500,-4095,16,104,104),
                                   (4400,0,100,-4096,332,104,104), (1700,1,400,64,1,200,200);
        INSERT INTO page_fault VALUES (4100,0,-140737488351232,2,100,0,104), (4480,0,4198400,2,50,0,104);
    )sql");
    sqlite3_close(db);

    ktrace_profile_model model;
    std::string error;
    bool ok = load_ktrace_database(db_path, model, error);
    unlink(db_path.c_str());
    check(ok && error.empty(), "loads real ktrace-decode SQLite schema");
    if (!ok) {
        return;
    }

    check(model.session.event_mask == ~0ull && !model.session.boot_unix_ns.has_value(),
          "preserves signed -1 event_mask and NULL boot_unix_ns");
    check(thread_name_at(model, 104, 1200) == "stlxdm-init" && thread_name_at(model, 104, 3500) == "stlxdm",
          "resolves time-dependent thread_names across rename");

    ktrace_window_view pid_zero = query_ktrace_window(model, { 1000, 5000, 0, std::nullopt, false });
    check(pid_zero.busy_cpu_ns == 0 && pid_zero.running_ns == 0,
          "normalizes threads.pid == 0 for kernel and idle tasks to nullopt rather than pid 0");

    check(resolve_syscall_name(model, 1001) == "sys_1001" &&
              resolve_syscall_name(model, 1030) == "futex_wait" &&
              resolve_syscall_name(model, 332) == "statx" &&
              resolve_errno_name(model, 38) == "ENOSYS",
          "omits SYS_ELEVATE 1001 while resolving Stellux and full musl x86_64 syscalls/errnos");

    ktrace_profile_model arm_model;
    arm_model.session.arch = "aarch64";
    arm_model.session.elf_machine = 183;
    check(resolve_syscall_name(arm_model, 29) == "ioctl" &&
              resolve_syscall_name(arm_model, 291) == "statx" &&
              resolve_syscall_name(arm_model, 332) == "sys_332",
          "uses full musl aarch64 syscall table without cross-arch contamination");

    ktrace_window_view full_stlxdm = query_ktrace_window(model, { 1000, 5000, 104, std::nullopt, false });
    check(full_stlxdm.busy_cpu_ns == 2500 && full_stlxdm.running_ns == 2500,
          "cross-checks cpu_slices busy time against thread_states running time");

    const ktrace_hotspot* futex_hotspot = nullptr;
    const ktrace_hotspot* pf_hotspot = nullptr;
    for (const ktrace_hotspot& hotspot : full_stlxdm.function_hotspots) {
        if (hotspot.name == "futex_wait") {
            futex_hotspot = &hotspot;
        } else if (hotspot.name == "page_fault") {
            pf_hotspot = &hotspot;
        }
    }

    check(futex_hotspot != nullptr && futex_hotspot->total_ns == 200 && futex_hotspot->call_count == 1,
          "counts blocking futex_wait spanning two CPU slices as 1 call with 200 ns on-CPU duration");
    check(pf_hotspot != nullptr && pf_hotspot->total_ns == 50,
          "excludes page_fault enclosed inside ioctl syscall from synthesized CPU hotspots");

    ktrace_window_view zoomed = query_ktrace_window(model, { 1500, 3500, 104, std::nullopt, false });
    check(zoomed.busy_cpu_ns == 1000 && zoomed.running_ns == 1000 && zoomed.sleeping_ns == 800,
          "clips cpu_slices and thread_states to zoomed [1500, 3500] window");

    ktrace_window_view without_recorder = query_ktrace_window(model, { 1000, 5000, std::nullopt, std::nullopt, true });
    check(without_recorder.busy_cpu_ns == 2900, "excludes ktrace recorder while keeping stlxdm and khubd");

    for (const ktrace_flame_span& span : full_stlxdm.flame_spans) {
        check(span.start_fraction >= 0.0 && span.start_fraction < span.end_fraction && span.end_fraction <= 1.0,
              "keeps flame span fractions inside [0.0, 1.0]");
    }

    auto frames = extract_ktrace_thread_frames(model, 104, 1000, 5000);
    check(frames.size() == 2 && frames[1].wall_ns == 1700 && frames[1].running_ns == 1500,
          "extracts wakeup-to-sleep active thread frames");

    ktrace_profile_model huge_cap;
    huge_cap.session.cpu_count = 8;
    huge_cap.threads.push_back({ 1, KTRACE_TASK_USER, 1, "app", {}, 0, 0, std::nullopt });
    huge_cap.cpu_slices.push_back({ 0, 1, false, 0, uint64_t{ 1 } << 62 });
    finalize_ktrace_model(huge_cap);

    auto bars = compute_ktrace_timeline_bars(huge_cap, { 0, uint64_t{ 1 } << 62, std::nullopt, std::nullopt, false }, 1);
    check(bars.size() == 1 && std::fabs(bars[0].busy_fraction - 0.125) < 1e-9,
          "computes bar capacity in double without uint64_t wall * cpu_count wrap");
}

static void test_cut_short_trace_null_preservation() {
    std::string db_path = create_temp_path("/tmp/sheen-ktrace-cut-XXXXXX");
    sqlite3* db = nullptr;
    sqlite3_open(db_path.c_str(), &db);
    exec_sql_or_die(db, FULL_KTRACE_DECODE_SCHEMA);
    exec_sql_or_die(db, R"sql(
        INSERT INTO names VALUES (1, 'worker');
        INSERT INTO session VALUES (1, 'aarch64', 183, 1, NULL, 100, NULL, -1, 4, 0, 2, 512);
        INSERT INTO health VALUES ('file_complete', 0, 'cut short');
        INSERT INTO threads VALUES (50, 1, 50, 100, 400, NULL);
        INSERT INTO thread_names VALUES (50, 1, 100, 400);
        INSERT INTO cpu_slices VALUES (0, 50, 100, 300), (0, 50, 300, NULL);
        INSERT INTO thread_states VALUES (50, 'running', 100, 300, 0, NULL, NULL, NULL, 0),
                                         (50, 'running', 300, NULL, 0, NULL, NULL, NULL, 0);
    )sql");
    sqlite3_close(db);

    ktrace_profile_model model;
    std::string error;
    bool ok = load_ktrace_database(db_path, model, error);
    unlink(db_path.c_str());

    ktrace_window_view view = query_ktrace_window(model, { 100, 500, std::nullopt, std::nullopt, false });
    check(ok && !model.all_health_checks_passed && !model.session.stop_ns.has_value() &&
              model.session.effective_end_ns == 300 && view.busy_cpu_ns == 200 &&
              view.indeterminate_slice_count == 1 && view.indeterminate_state_count == 1,
          "preserves NULL stop_ns and end_ns without counting indeterminate slices as zero");
}

int main() {
    test_magic_probes_and_validation();
    test_sqlite_decode_schema_and_invariants();
    test_cut_short_trace_null_preservation();

    if (g_failures != 0) {
        std::printf("ktrace_model: %llu of %llu checks failed\n",
                    static_cast<unsigned long long>(g_failures),
                    static_cast<unsigned long long>(g_checks));
        return 1;
    }

    std::printf("ktrace_model: %llu checks passed\n", static_cast<unsigned long long>(g_checks));
    return 0;
}
