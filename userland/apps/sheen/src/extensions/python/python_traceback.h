#ifndef SHEEN_EXTENSIONS_PYTHON_PYTHON_TRACEBACK_H
#define SHEEN_EXTENSIONS_PYTHON_PYTHON_TRACEBACK_H

/* Pure C++20 streaming parser for CPython runtime tracebacks, PEP 657 carets, and workspace target discovery. */

#include "extensions/workspace_paths.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sheen {

struct traceback_frame {
    std::string raw_path;
    std::string resolved_path;
    int line = 1;
    int column = 0;
    std::string scope_name;
};

/* Stateful line-streaming parser for CPython 3 runtime tracebacks and compile-time SyntaxError blocks. */
class python_traceback_parser {
public:
    explicit python_traceback_parser(std::string workspace_root, bool require_existing_files = false);

    void reset();
    bool feed(std::string_view chunk);
    bool finish();

    std::string take_pending_jump_lines();

    const std::vector<script_diagnostic>& diagnostics() const { return m_diagnostics; }
    const std::vector<traceback_frame>& frames() const { return m_completed_frames; }
    int error_count() const;

private:
    enum class frame_substate {
        NONE,
        AWAITING_SOURCE,
        AWAITING_CARET,
        READY_FOR_EXCEPTION,
    };

    bool process_line(std::string_view raw_line);
    bool try_parse_file_header(std::string_view line);
    bool try_parse_indented_detail(std::string_view line);
    bool try_finalize_exception(std::string_view line);
    void record_diagnostic(const script_diagnostic& item);

    std::string m_workspace_root;
    bool m_require_existing_files = false;
    std::string m_line_buffer;
    bool m_in_traceback_block = false;
    bool m_has_completed_block = false;
    bool m_is_chained_continuation = false;
    bool m_clear_previous_on_finalize = false;
    frame_substate m_substate = frame_substate::NONE;
    size_t m_source_indent = 0;
    size_t m_unreported_frame_index = 0;
    std::vector<traceback_frame> m_pending_frames;
    std::vector<traceback_frame> m_completed_frames;
    std::vector<script_diagnostic> m_diagnostics;
};

std::string format_traceback_jump_lines(const std::vector<traceback_frame>& frames, std::string_view workspace_root);
bool is_python_source_path(std::string_view path);
std::vector<std::string> discover_python_targets(const std::string& workspace_root);

} // namespace sheen

#endif
