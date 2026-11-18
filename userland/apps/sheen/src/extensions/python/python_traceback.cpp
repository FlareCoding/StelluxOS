#include "extensions/python/python_traceback.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace sheen {

static constexpr size_t MIN_TRACEBACK_FILE_INDENT = 2;
static constexpr size_t PYTHON_SNIPPET_INDENT = 4;

static constexpr std::string_view TRACEBACK_HEADER = "Traceback (most recent call last):";
static constexpr std::string_view CHAINED_DURING_HEADER =
    "During handling of the above exception, another exception occurred:";
static constexpr std::string_view CHAINED_CAUSE_HEADER =
    "The above exception was the direct cause of the following exception:";
static constexpr std::string_view FILE_PREFIX = "File \"";
static constexpr std::string_view LINE_DELIMITER = "\", line ";
static constexpr std::string_view SCOPE_DELIMITER = ", in ";

static size_t count_leading_spaces(std::string_view line) {
    size_t count = 0;
    while (count < line.size() && line[count] == ' ') {
        count++;
    }

    return count;
}

static size_t read_file_line_leading_whitespace(const std::string& resolved_path, int target_line) {
    if (resolved_path.empty() || target_line <= 0) {
        return 0;
    }

    std::ifstream input(resolved_path, std::ios::binary);
    if (!input.is_open()) {
        return 0;
    }

    std::string current;
    for (int line_no = 1; line_no <= target_line; line_no++) {
        if (!std::getline(input, current)) {
            return 0;
        }
    }

    size_t indent = 0;
    while (indent < current.size() && (current[indent] == ' ' || current[indent] == '\t')) {
        indent++;
    }

    return indent;
}

static bool extract_caret_index(std::string_view line, size_t* first_caret) {
    size_t indent = count_leading_spaces(line);
    if (indent == 0 || indent >= line.size()) {
        return false;
    }

    size_t caret_pos = std::string_view::npos;
    for (size_t i = indent; i < line.size(); i++) {
        char ch = line[i];
        if (ch == '^') {
            if (caret_pos == std::string_view::npos) {
                caret_pos = i;
            }
        } else if (ch != '~' && ch != ' ') {
            return false;
        }
    }

    if (caret_pos == std::string_view::npos) {
        return false;
    }

    *first_caret = caret_pos;

    return true;
}

static bool is_python_identifier_segment(std::string_view segment) {
    if (segment.empty()) {
        return false;
    }

    unsigned char first = static_cast<unsigned char>(segment.front());
    if (!std::isalpha(first) && first != '_') {
        return false;
    }

    for (size_t i = 1; i < segment.size(); i++) {
        unsigned char ch = static_cast<unsigned char>(segment[i]);
        if (!std::isalnum(ch) && ch != '_') {
            return false;
        }
    }

    return true;
}

static bool is_qualified_exception_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }

    size_t start = 0;
    while (start <= name.size()) {
        size_t dot = name.find('.', start);
        std::string_view part = dot == std::string_view::npos ? name.substr(start) : name.substr(start, dot - start);
        if (!is_python_identifier_segment(part)) {
            return false;
        }

        if (dot == std::string_view::npos) {
            break;
        }

        start = dot + 1;
    }

    return true;
}

static bool is_standalone_syntax_exception_name(std::string_view qualified_name) {
    size_t dot = qualified_name.rfind('.');
    std::string_view leaf = dot == std::string_view::npos ? qualified_name : qualified_name.substr(dot + 1);

    return leaf.ends_with("Error") || leaf.ends_with("Warning") || leaf.ends_with("Exception");
}

static bool parse_exception_summary_line(std::string_view line, bool require_error_suffix,
                                         script_diagnostic_severity* severity, std::string* full_message) {
    if (line.empty() || line.front() == ' ' || line.front() == '\t') {
        return false;
    }

    size_t colon = line.find(':');
    std::string_view exc_name = colon == std::string_view::npos ? trim_ascii_spaces(line) : line.substr(0, colon);
    if (!is_qualified_exception_name(exc_name)) {
        return false;
    }

    if (require_error_suffix && !is_standalone_syntax_exception_name(exc_name)) {
        return false;
    }

    *severity = exc_name.ends_with("Warning") ? script_diagnostic_severity::WARNING
                                              : script_diagnostic_severity::ERROR;
    *full_message = std::string(trim_ascii_spaces(line));

    return true;
}

python_traceback_parser::python_traceback_parser(std::string workspace_root, bool require_existing_files)
    : m_workspace_root(std::move(workspace_root)), m_require_existing_files(require_existing_files) {
}

void python_traceback_parser::reset() {
    m_line_buffer.clear();
    m_in_traceback_block = false;
    m_has_completed_block = false;
    m_is_chained_continuation = false;
    m_clear_previous_on_finalize = false;

    m_substate = frame_substate::NONE;
    m_source_indent = 0;
    m_unreported_frame_index = 0;
    m_pending_frames.clear();
    m_completed_frames.clear();
    m_diagnostics.clear();
}

bool python_traceback_parser::feed(std::string_view chunk) {
    m_line_buffer.append(chunk.data(), chunk.size());
    bool updated = false;

    size_t start = 0;
    while (true) {
        size_t newline = m_line_buffer.find('\n', start);
        if (newline == std::string::npos) {
            break;
        }

        std::string_view line(m_line_buffer.data() + start, newline - start);
        if (process_line(line)) {
            updated = true;
        }

        start = newline + 1;
    }

    if (start > 0) {
        m_line_buffer.erase(0, start);
    }

    return updated;
}

bool python_traceback_parser::finish() {
    if (m_line_buffer.empty()) {
        return false;
    }

    std::string tail = std::move(m_line_buffer);
    m_line_buffer.clear();

    return process_line(tail);
}

std::string python_traceback_parser::take_pending_jump_lines() {
    if (m_unreported_frame_index >= m_completed_frames.size()) {
        return {};
    }

    std::vector<traceback_frame> recent(
        m_completed_frames.begin() + static_cast<std::ptrdiff_t>(m_unreported_frame_index),
        m_completed_frames.end());
    m_unreported_frame_index = m_completed_frames.size();

    return format_traceback_jump_lines(recent, m_workspace_root);
}

int python_traceback_parser::error_count() const {
    int count = 0;
    for (const script_diagnostic& item : m_diagnostics) {
        if (item.severity == script_diagnostic_severity::ERROR) {
            count++;
        }
    }

    return count;
}

bool python_traceback_parser::process_line(std::string_view raw_line) {
    std::string cleaned = strip_terminal_escapes(raw_line);
    std::string_view line(cleaned);
    std::string_view trimmed = trim_ascii_spaces(line);

    if (trimmed == TRACEBACK_HEADER) {
        m_clear_previous_on_finalize = m_has_completed_block && !m_is_chained_continuation;
        m_is_chained_continuation = false;
        m_in_traceback_block = true;

        m_substate = frame_substate::NONE;
        m_source_indent = 0;
        m_pending_frames.clear();
        return false;
    }

    if (trimmed == CHAINED_DURING_HEADER || trimmed == CHAINED_CAUSE_HEADER) {
        m_is_chained_continuation = true;
        m_in_traceback_block = false;
        m_substate = frame_substate::NONE;
        m_source_indent = 0;
        m_pending_frames.clear();
        return false;
    }

    if (try_parse_file_header(line)) {
        return false;
    }

    if (m_pending_frames.empty()) {
        return false;
    }

    if (try_parse_indented_detail(line)) {
        return false;
    }

    return try_finalize_exception(line);
}

bool python_traceback_parser::try_parse_file_header(std::string_view line) {
    size_t indent = count_leading_spaces(line);
    if (indent < MIN_TRACEBACK_FILE_INDENT) {
        return false;
    }

    std::string_view body = line.substr(indent);
    if (!body.starts_with(FILE_PREFIX)) {
        return false;
    }

    std::string_view after_prefix = body.substr(FILE_PREFIX.size());
    size_t delim = after_prefix.rfind(LINE_DELIMITER);
    if (delim == std::string_view::npos || delim == 0) {
        return false;
    }

    std::string_view raw_path = after_prefix.substr(0, delim);
    std::string_view after_delim = after_prefix.substr(delim + LINE_DELIMITER.size());
    int line_number = 0;
    size_t digits_len = 0;
    if (!parse_positive_int_prefix(after_delim, &line_number, &digits_len)) {
        return false;
    }

    std::string_view remainder = trim_ascii_spaces(after_delim.substr(digits_len));
    std::string scope;
    if (!remainder.empty()) {
        if (!remainder.starts_with(SCOPE_DELIMITER)) {
            return false;
        }

        scope = std::string(trim_ascii_spaces(remainder.substr(SCOPE_DELIMITER.size())));
    }

    if (m_pending_frames.empty() && !m_in_traceback_block) {
        m_clear_previous_on_finalize = m_has_completed_block && !m_is_chained_continuation;
        m_is_chained_continuation = false;
    }

    traceback_frame frame{
        std::string(raw_path),
        resolve_workspace_file_path(m_workspace_root, raw_path, m_require_existing_files),
        line_number,
        0,
        std::move(scope),
    };

    m_pending_frames.push_back(std::move(frame));
    m_substate = frame_substate::AWAITING_SOURCE;
    m_source_indent = 0;

    return true;
}

bool python_traceback_parser::try_parse_indented_detail(std::string_view line) {
    if (line.empty()) {
        if (m_substate == frame_substate::AWAITING_SOURCE) {
            m_source_indent = PYTHON_SNIPPET_INDENT;
            m_substate = frame_substate::AWAITING_CARET;
        }

        return true;
    }

    if (line.front() != ' ' && line.front() != '\t') {
        return false;
    }

    if (m_substate == frame_substate::AWAITING_CARET) {
        size_t caret_pos = 0;
        if (extract_caret_index(line, &caret_pos)) {
            traceback_frame& current = m_pending_frames.back();
            size_t relative_col = caret_pos >= m_source_indent ? (caret_pos - m_source_indent) : caret_pos;
            size_t disk_indent = read_file_line_leading_whitespace(current.resolved_path, current.line);
            current.column = static_cast<int>(disk_indent + relative_col + 1);
            m_substate = frame_substate::READY_FOR_EXCEPTION;
            return true;
        }
    }

    if (m_substate == frame_substate::AWAITING_SOURCE) {
        m_source_indent = count_leading_spaces(line);
        m_substate = frame_substate::AWAITING_CARET;
        return true;
    }

    return true;
}

bool python_traceback_parser::try_finalize_exception(std::string_view line) {
    script_diagnostic_severity severity = script_diagnostic_severity::ERROR;
    std::string message;
    bool require_suffix = !m_in_traceback_block;
    if (!parse_exception_summary_line(line, require_suffix, &severity, &message)) {
        if (!trim_ascii_spaces(line).empty()) {
            m_in_traceback_block = false;
            m_clear_previous_on_finalize = false;
            m_substate = frame_substate::NONE;
            m_pending_frames.clear();
        }

        return false;
    }

    bool had_previous = !m_diagnostics.empty();
    if (m_clear_previous_on_finalize) {
        m_diagnostics.clear();
        m_completed_frames.clear();
        m_unreported_frame_index = 0;
        m_clear_previous_on_finalize = false;
    }

    bool emitted = false;
    for (auto it = m_pending_frames.rbegin(); it != m_pending_frames.rend(); ++it) {
        if (it->resolved_path.empty()) {
            continue;
        }

        record_diagnostic({ it->resolved_path, it->line, it->column, severity, message });
        emitted = true;
        break;
    }

    for (traceback_frame& frame : m_pending_frames) {
        m_completed_frames.push_back(std::move(frame));
    }

    m_pending_frames.clear();
    m_has_completed_block = true;
    m_in_traceback_block = false;
    m_substate = frame_substate::NONE;
    m_source_indent = 0;

    return emitted || had_previous;
}

void python_traceback_parser::record_diagnostic(const script_diagnostic& item) {
    if (std::find(m_diagnostics.begin(), m_diagnostics.end(), item) == m_diagnostics.end()) {
        m_diagnostics.push_back(item);
    }
}

std::string format_traceback_jump_lines(const std::vector<traceback_frame>& frames, std::string_view workspace_root) {
    std::string formatted;
    std::vector<std::string> seen;

    for (const traceback_frame& frame : frames) {
        if (frame.resolved_path.empty()) {
            continue;
        }

        std::string display = relative_workspace_display_path(workspace_root, frame.resolved_path);
        std::string location = display + ":" + std::to_string(frame.line);
        if (frame.column > 0) {
            location += ":" + std::to_string(frame.column);
        }

        if (std::find(seen.begin(), seen.end(), location) != seen.end()) {
            continue;
        }

        seen.push_back(location);
        formatted += "  at " + location;
        if (!frame.scope_name.empty()) {
            formatted += " in " + frame.scope_name;
        }

        formatted += "\n";
    }

    return formatted;
}

bool is_python_source_path(std::string_view path) {
    return ends_with_ignore_case(trim_ascii_spaces(path), ".py");
}

static int python_target_rank(std::string_view rel_path) {
    if (rel_path == "main.py") {
        return 0;
    }

    if (rel_path == "app.py") {
        return 1;
    }

    return rel_path.find('/') == std::string_view::npos ? 2 : 3;
}

std::vector<std::string> discover_python_targets(const std::string& workspace_root) {
    std::vector<std::string> targets = walk_workspace_files(
        workspace_root, is_python_source_path, is_common_ignored_directory);

    std::sort(targets.begin(), targets.end(), [&workspace_root](const std::string& lhs, const std::string& rhs) {
        std::string lhs_rel = relative_workspace_display_path(workspace_root, lhs);
        std::string rhs_rel = relative_workspace_display_path(workspace_root, rhs);
        int lhs_rank = python_target_rank(lhs_rel);
        int rhs_rank = python_target_rank(rhs_rel);
        if (lhs_rank != rhs_rank) {
            return lhs_rank < rhs_rank;
        }

        return lhs_rel < rhs_rel;
    });

    return targets;
}

} // namespace sheen
