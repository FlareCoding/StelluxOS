#ifndef SHEEN_EXTENSIONS_EMBEDDED_PROTOCOL_H
#define SHEEN_EXTENSIONS_EMBEDDED_PROTOCOL_H

/* Pure C++20 protocol parsers and workspace state models for mcufw builds, boards, and libraries. */

#include "workspace/event_stream.h"

#include <cstddef>
#include <cstdint>
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
    std::string failure_kind;
    std::string reason;
};

struct board_entry {
    std::string fqbn;
    std::string name;
    std::string needs;
};

struct sketch_defaults {
    std::string fqbn;
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

struct installed_library {
    std::string name;
    std::string version;
    std::string directory;
    bool is_core = false;
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

std::string base_fqbn(std::string_view fqbn);

mcufw_event parse_mcufw_event_fields(const std::vector<std::string>& fields);
mcufw_event parse_mcufw_event_line(std::string_view line);

void record_memory_size(memory_usage& usage, const mcufw_event& size_event);
std::string format_memory_summary(const memory_usage& usage);
bool is_memory_usage_danger(const memory_usage& usage);

int compute_progress_percent(uint64_t done, uint64_t total);
std::string format_progress_label(std::string_view stage_or_item, uint64_t done, uint64_t total, bool is_install);

bool append_problem_diagnostic(std::vector<embedded_diagnostic>& diagnostics, const mcufw_event& problem_event);
bool attach_library_hint(std::vector<embedded_diagnostic>& diagnostics, const mcufw_event& hint_event,
                         const std::string& sketch_dir = {});

sketch_defaults parse_sketch_yaml(std::string_view text);
sketch_defaults read_sketch_defaults(const std::string& sketch_dir);

bool is_path_within_workspace(std::string_view candidate_path, std::string_view workspace_root);
std::string resolve_sketch_directory(const std::string& workspace_or_file_path);

std::vector<installed_library> scan_installed_libraries(const std::string& libraries_dir,
                                                        const std::string& hardware_dir = {});

} // namespace sheen

#endif
