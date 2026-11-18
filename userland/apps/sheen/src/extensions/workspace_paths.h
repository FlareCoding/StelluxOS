#ifndef SHEEN_EXTENSIONS_WORKSPACE_PATHS_H
#define SHEEN_EXTENSIONS_WORKSPACE_PATHS_H

/* Shared Qt-free terminal escape filtering, workspace path containment, and bounded directory walking. */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sheen {

enum class script_diagnostic_severity {
    WARNING,
    ERROR,
};

struct script_diagnostic {
    std::string file_path;
    int line = 1;
    int column = 0;
    script_diagnostic_severity severity = script_diagnostic_severity::ERROR;
    std::string message;

    bool operator==(const script_diagnostic&) const = default;
};

/* Stateful byte-stream filter removing ANSI CSI/OSC sequences, 2-byte ESC codes, and CR across read chunks. */
class terminal_escape_filter {
public:
    std::string feed(std::string_view chunk);
    void reset();

private:
    enum class escape_state {
        TEXT,
        AFTER_ESC,
        CSI,
        OSC,
        OSC_ESC,
    };

    escape_state m_state = escape_state::TEXT;
};

std::string strip_terminal_escapes(std::string_view text);
std::string_view trim_ascii_spaces(std::string_view text);
bool ends_with_ignore_case(std::string_view text, std::string_view suffix);
bool parse_positive_int_prefix(std::string_view text, int* parsed_value, size_t* consumed_chars);

std::string resolve_workspace_file_path(std::string_view workspace_root, std::string_view candidate_path,
                                        bool require_existing_file = false);
std::string relative_workspace_display_path(std::string_view workspace_root, std::string_view file_path);

bool is_common_ignored_directory(std::string_view dir_name);
std::vector<std::string> walk_workspace_files(const std::string& workspace_root,
                                              bool (*include_file)(std::string_view),
                                              bool (*skip_directory)(std::string_view));

} // namespace sheen

#endif
