#ifndef SHEEN_EXTENSIONS_EMBEDDED_PROTOCOL_H
#define SHEEN_EXTENSIONS_EMBEDDED_PROTOCOL_H

/* Pure C++20 protocol parsers and workspace state models for mcufw and serial `--events` streams. */

#include "workspace/event_stream.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sheen {

inline constexpr const char* EMBEDDED_PYTHON_PROBE_PATH = "/bin/python3";
inline constexpr const char* EMBEDDED_ESPTOOL_PROBE_PATH =
    "/usr/lib/python3.12/site-packages/esptool/__init__.py";
inline constexpr const char* EMBEDDED_XTENSA_GPP_PROBE_PATH =
    "/usr/xtensa-esp32-elf/bin/xtensa-esp32-elf-g++";
inline constexpr const char* EMBEDDED_DEFAULT_CORE_TARGET = "esp32:esp32:esp32";
inline constexpr const char* EMBEDDED_ESP32_LIBS_PROBE_PATH =
    "/var/lib/mcufw/tools/esp32/esp32-libs";

enum class problem_level { ERROR, WARNING, NOTE };

enum class memory_bank { FLASH, RAM };

enum class mcufw_event_kind {
    UNKNOWN,
    BOARD,
    LIBRARY,
    PROBLEM,
    HINT,
    SIZE,
    PROGRESS,
    LOG,
    START,
    INSTALLED,
    NEEDS,
    WARNING,
    BUILT,
    PORT,
    FLASHED,
    FAILED,
};

struct mcufw_event {
    mcufw_event_kind kind = mcufw_event_kind::UNKNOWN;
    std::string fqbn;
    std::string name;
    std::string needs;
    std::string folder;
    std::string version;
    problem_level severity = problem_level::ERROR;
    std::string file_path;
    int line = 0;
    int column = 0;
    std::string message;
    std::string header;
    memory_bank bank = memory_bank::FLASH;
    uint64_t used_bytes = 0;
    uint64_t max_bytes = 0;
    bool has_max_bytes = false;
    std::string stage;
    uint64_t done = 0;
    uint64_t total = 0;
    std::string port_name;
    std::string port_path;
    std::string failure_kind;
    std::string reason;
};

enum class serial_event_kind {
    UNKNOWN,
    PORT,
    LISTED,
    ADD,
    REMOVE,
    OPEN,
    GONE,
    BACK,
    CLOSED,
    FAILED,
};

struct serial_port_entry {
    std::string name;
    std::string bus;
    std::string driver;
    std::string usb_ids;
    std::string location;
    std::string serial_number;
    std::string manufacturer;
    std::string product;
    uint64_t arrival = 0;
};

struct serial_event {
    serial_event_kind kind = serial_event_kind::UNKNOWN;
    serial_port_entry port;
    std::string port_name;
    size_t listed_count = 0;
    uint32_t baud_rate = 0;
    std::string failure_subject;
    std::string failure_kind;
    std::string failure_reason;
};

struct resolved_serial_port {
    std::string name;
    std::string short_description;
    std::string full_description;
    bool is_present = false;
    bool is_auto_selected = false;
};

struct board_entry {
    std::string fqbn;
    std::string name;
    std::string needs;
};

struct sketch_defaults {
    std::string fqbn;
    std::string port_name;
    std::string raw_port;
};

struct memory_bank_usage {
    uint64_t used_bytes = 0;
    uint64_t max_bytes = 0;
    bool has_value = false;
    bool has_max = false;
};

struct memory_usage {
    memory_bank_usage flash;
    memory_bank_usage ram;
};

struct embedded_diagnostic {
    std::string file_path;
    int line = 1;
    int column = 0;
    bool is_warning = false;
    std::string message;
    std::string fix_library;
    std::string fix_version;
    std::string fix_header;
};

struct backtrace_location {
    std::string file_path;
    int line = 1;
    size_t span_offset = 0;
    size_t span_length = 0;
};

struct installed_library {
    std::string name;
    std::string version;
    std::string directory;
    bool is_core = false;
};

enum class flash_cancel_action {
    NONE,
    START_MONITOR_NOW,
    RESTART_MONITOR_WHEN_STOPPED,
};

enum class monitor_stop_action {
    NONE,
    START_PENDING_FLASH,
    RESTART_MONITOR,
};

enum class flash_finish_action {
    NONE,
    RESUME_AND_REVEAL_MONITOR,
    RESUME_MONITOR_QUIETLY,
};

/* Streams `mcufw boards --events` and commits the new board list only when the process succeeds. */
class board_catalog {
public:
    board_catalog();

    void begin_refresh();
    void feed_stdout(std::string_view chunk);
    bool finish_refresh(int exit_code, bool was_stopped);

    const std::vector<board_entry>& boards() const { return m_boards; }
    const board_entry* find_by_fqbn(std::string_view fqbn) const;

private:
    void handle_fields(const std::vector<std::string>& fields);

    event_line_stream m_stream;
    std::vector<board_entry> m_boards;
    std::vector<board_entry> m_pending;
    bool m_saw_failure = false;
};

/* Tracks live serial ports across `serial watch --events` snapshots, restarts, and hotplug transitions. */
class serial_port_catalog {
public:
    bool apply(const serial_event& event);
    bool feed_chunk(std::string_view chunk);
    bool finish_process();

    const std::vector<serial_port_entry>& ports() const { return m_ports; }
    const serial_port_entry* find(std::string_view port_name) const;
    resolved_serial_port resolve(std::string_view preferred_port, uint32_t baud_rate = 0) const;

private:
    std::vector<serial_port_entry> m_ports;
    std::vector<serial_port_entry> m_snapshot_ports;
    std::string m_line_buffer;
    bool m_has_pending_snapshot = false;
    bool m_has_completed_snapshot = false;
};

/* Coordinates pausing an active Serial Monitor before `mcufw flash` and restoring it on finish or cancel. */
class flash_monitor_coordinator {
public:
    bool is_waiting_for_monitor_stop() const { return m_waiting_for_monitor_stop; }

    bool begin_flash(bool monitor_is_running);
    flash_cancel_action cancel_flash(bool monitor_is_running);
    monitor_stop_action on_monitor_stopped();
    flash_finish_action on_flash_finished(bool flash_succeeded);

    void request_monitor_restart_after_stop();
    void clear_for_manual_monitor_toggle();
    void reset();

private:
    bool m_waiting_for_monitor_stop = false;
    bool m_resume_monitor_after_flash = false;
    bool m_restart_monitor_after_stop = false;
};

/* Emits only the first existing source frame for each decoded panic dump on the monitor stream. */
class crash_output_scanner {
public:
    void reset();
    std::optional<backtrace_location> feed_chunk(
        std::string_view chunk,
        const std::function<bool(const std::string&)>& file_exists = {});

private:
    std::string m_line_buffer;
    bool m_reported_in_current_dump = false;
};

std::string normalize_serial_port_name(std::string_view port);
std::string base_fqbn(std::string_view fqbn);

mcufw_event parse_mcufw_event_fields(const std::vector<std::string>& fields);
mcufw_event parse_mcufw_event_line(std::string_view line);

serial_event parse_serial_event_line(std::string_view line);
std::string format_serial_port_short_description(const serial_port_entry& port);
std::string format_serial_port_detail(const serial_port_entry& port, uint32_t baud_rate = 0);
std::string format_serial_monitor_mark(const serial_event& event, bool& is_danger);

void record_memory_size(memory_usage& usage, const mcufw_event& size_event);
std::string format_memory_summary(const memory_usage& usage);
bool is_memory_usage_danger(const memory_usage& usage);

int compute_progress_percent(uint64_t done, uint64_t total);
std::string format_progress_label(std::string_view stage_or_item, uint64_t done, uint64_t total, bool is_install);

bool append_problem_diagnostic(std::vector<embedded_diagnostic>& diagnostics, const mcufw_event& problem_event);
bool attach_library_hint(std::vector<embedded_diagnostic>& diagnostics, const mcufw_event& hint_event,
                         const std::string& sketch_dir = {});

std::optional<backtrace_location> extract_backtrace_location(std::string_view line);

sketch_defaults parse_sketch_yaml(std::string_view text);
sketch_defaults read_sketch_defaults(const std::string& sketch_dir);

bool is_path_within_workspace(std::string_view candidate_path, std::string_view workspace_root);
std::string resolve_sketch_directory(const std::string& workspace_or_file_path);

std::vector<installed_library> scan_installed_libraries(const std::string& libraries_dir,
                                                        const std::string& hardware_dir = {});

} // namespace sheen

#endif
