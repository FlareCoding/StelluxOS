#ifndef SHEEN_EXTENSIONS_PERF_BINARIES_KTRACE_MODEL_H
#define SHEEN_EXTENSIONS_PERF_BINARIES_KTRACE_MODEL_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sheen {

inline constexpr uint8_t KTRACE_TASK_NOT_RECORDED = 0;
inline constexpr uint8_t KTRACE_TASK_USER = 1;
inline constexpr uint8_t KTRACE_TASK_KERNEL = 2;
inline constexpr uint8_t KTRACE_TASK_IDLE = 3;

inline constexpr uint8_t KTRACE_SWITCH_PREEMPTED = 0;
inline constexpr uint8_t KTRACE_SWITCH_YIELDED = 1;
inline constexpr uint8_t KTRACE_SWITCH_BLOCKED = 2;
inline constexpr uint8_t KTRACE_SWITCH_EXITED = 3;

inline constexpr uint64_t KTRACE_PF_PRESENT = 0x1;
inline constexpr uint64_t KTRACE_PF_WRITE = 0x2;
inline constexpr uint64_t KTRACE_PF_INSTRUCTION = 0x4;
inline constexpr size_t KTRACE_MAX_ROWS_PER_TABLE = 2000000;

enum class thread_state_kind : uint8_t {
    RUNNING,
    RUNNABLE,
    SLEEPING,
};

struct ktrace_session {
    uint32_t format_version = 0, cpu_count = 0;
    std::string arch;
    uint16_t elf_machine = 0;
    std::optional<uint64_t> boot_unix_ns, stop_ns;
    uint64_t start_ns = 0, effective_end_ns = 0, event_mask = 0;
    uint64_t records = 0, records_before_start = 0, lost_records = 0, file_bytes = 0;
};

struct ktrace_cpu_counter {
    uint32_t cpu = 0;
    uint64_t records = 0, records_before_start = 0, lost_records = 0;
};

struct ktrace_health_status {
    std::string name;
    bool passed = false;
    std::string detail;
};

struct ktrace_thread_name_span {
    std::string name;
    uint64_t first_ns = 0, last_ns = 0;
};

struct ktrace_thread {
    uint32_t tid = 0;
    std::optional<uint8_t> kind;
    std::optional<uint32_t> pid;
    std::string latest_name;
    std::vector<ktrace_thread_name_span> names;
    uint64_t first_ns = 0, last_ns = 0;
    std::optional<uint64_t> exit_ns;
};

struct ktrace_process {
    uint32_t pid = 0;
    std::string leader_name;
    uint64_t thread_count = 0, first_ns = 0, last_ns = 0;
    bool is_recorder = false;
};

struct ktrace_cpu_slice {
    uint32_t cpu = 0, tid = 0;
    bool is_idle = false;
    uint64_t start_ns = 0;
    std::optional<uint64_t> end_ns;
};

struct ktrace_thread_state {
    uint32_t tid = 0;
    thread_state_kind state = thread_state_kind::RUNNING;
    uint64_t start_ns = 0;
    std::optional<uint64_t> end_ns;
    std::optional<uint32_t> cpu, waker_tid, syscall_number;
    std::optional<uint8_t> switch_reason;
    bool in_page_fault = false;
};

struct ktrace_syscall_event {
    uint64_t start_ns = 0, end_ns = 0, duration_ns = 0, sleep_ns = 0;
    uint32_t cpu = 0, tid = 0, pid = 0, number = 0;
    int64_t result = 0;
    bool is_error = false;
    int32_t errno_code = 0;
};

struct ktrace_page_fault_event {
    uint64_t start_ns = 0, end_ns = 0, duration_ns = 0;
    uint32_t cpu = 0, tid = 0;
    uint64_t address = 0, flags = 0;
    int32_t result = 0;
};

struct ktrace_profile_model {
    ktrace_session session;
    bool all_health_checks_passed = false;
    std::vector<ktrace_health_status> health_checks;
    std::vector<ktrace_cpu_counter> cpus;
    std::vector<ktrace_process> processes;
    std::vector<ktrace_thread> threads;
    std::vector<ktrace_cpu_slice> cpu_slices;
    std::vector<ktrace_thread_state> thread_states;
    std::vector<ktrace_syscall_event> syscalls;
    std::vector<ktrace_page_fault_event> page_faults;
    std::map<uint32_t, std::string> custom_syscall_names;
    std::map<int32_t, std::string> custom_errno_names;
};

struct ktrace_window_filter {
    uint64_t start_ns = 0, end_ns = 0;
    std::optional<uint32_t> pid, tid;
    bool exclude_recorder = false;
};

struct ktrace_syscall_summary {
    uint32_t number = 0;
    std::string name;
    uint64_t call_count = 0, error_count = 0;
    uint64_t wall_ns = 0, sleep_ns = 0, max_duration_ns = 0;
};

struct ktrace_hotspot {
    std::string name, source_file;
    int source_line = 0;
    uint64_t self_ns = 0, total_ns = 0, call_count = 0;
    double self_percent = 0.0;
};

struct ktrace_window_view {
    uint64_t start_ns = 0, end_ns = 0, window_ns = 0;
    uint64_t busy_cpu_ns = 0, idle_cpu_ns = 0;
    uint64_t running_ns = 0, runnable_ns = 0, sleeping_ns = 0;
    uint64_t indeterminate_slice_count = 0, indeterminate_state_count = 0;
    std::vector<ktrace_syscall_summary> syscall_summaries;
    std::vector<ktrace_hotspot> function_hotspots;
};

bool is_ktrace_capture(std::span<const uint8_t> header_bytes);
bool is_sqlite_database(std::span<const uint8_t> header_bytes);
bool load_ktrace_database(const std::string& db_path, ktrace_profile_model& model, std::string& error);
void finalize_ktrace_model(ktrace_profile_model& model);
std::string resolve_syscall_name(const ktrace_profile_model& model, uint32_t syscall_number);
std::string resolve_errno_name(const ktrace_profile_model& model, int32_t errno_code);
std::string thread_name_at(const ktrace_profile_model& model, uint32_t tid, uint64_t ts_ns);
ktrace_window_view query_ktrace_window(const ktrace_profile_model& model,
                                       const ktrace_window_filter& filter);
std::string format_duration_ns(uint64_t ns);

} // namespace sheen

#endif // SHEEN_EXTENSIONS_PERF_BINARIES_KTRACE_MODEL_H
