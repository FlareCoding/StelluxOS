#include "extensions/c_run/compiler_diagnostics.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace sheen {

static constexpr uint64_t FNV1A_OFFSET_BASIS = 14695981039346656037ull;
static constexpr uint64_t FNV1A_PRIME = 1099511628211ull;
static constexpr size_t HEX_HASH_BUFFER_BYTES = 17;
static constexpr std::string_view BUILD_DIR_PREFIX = "/tmp/sheen-build-";
static constexpr std::string_view STELLUX_USR_INCLUDE_DIR = "/usr/include";
static constexpr std::string_view STELLUX_LIBSTLX_PATH = "/usr/lib/libstlx.a";

static bool parse_compiler_severity_and_message(std::string_view remainder, script_diagnostic_severity* severity,
                                                std::string* message) {
    std::string_view trimmed = trim_ascii_spaces(remainder);
    if (trimmed.starts_with("fatal error:")) {
        *severity = script_diagnostic_severity::ERROR;
        *message = std::string(trim_ascii_spaces(trimmed.substr(std::string_view("fatal error:").size())));
        return !message->empty();
    }

    if (trimmed.starts_with("error:")) {
        *severity = script_diagnostic_severity::ERROR;
        *message = std::string(trim_ascii_spaces(trimmed.substr(std::string_view("error:").size())));
        return !message->empty();
    }

    if (trimmed.starts_with("warning:")) {
        *severity = script_diagnostic_severity::WARNING;
        *message = std::string(trim_ascii_spaces(trimmed.substr(std::string_view("warning:").size())));
        return !message->empty();
    }

    if (trimmed.starts_with('(')) {
        size_t section_end = trimmed.find("):");
        if (section_end != std::string_view::npos) {
            trimmed = trim_ascii_spaces(trimmed.substr(section_end + 2));
        }
    }

    if (trimmed.starts_with("undefined reference to ") || trimmed.starts_with("multiple definition of ")) {
        *severity = script_diagnostic_severity::ERROR;
        *message = std::string(trimmed);
        return true;
    }

    return false;
}

static std::string_view strip_tool_prefix(std::string_view path_prefix) {
    size_t separator = path_prefix.rfind(": ");
    if (separator != std::string_view::npos) {
        return trim_ascii_spaces(path_prefix.substr(separator + 2));
    }

    return trim_ascii_spaces(path_prefix);
}

compiler_output_parser::compiler_output_parser(std::string workspace_root, bool require_existing_files)
    : m_workspace_root(std::move(workspace_root)), m_require_existing_files(require_existing_files) {
}

void compiler_output_parser::reset() {
    m_line_buffer.clear();
    m_diagnostics.clear();
}

bool compiler_output_parser::feed(std::string_view chunk) {
    m_line_buffer.append(chunk.data(), chunk.size());
    bool added_any = false;

    size_t start = 0;
    while (true) {
        size_t newline = m_line_buffer.find('\n', start);
        if (newline == std::string::npos) {
            break;
        }

        std::string_view line(m_line_buffer.data() + start, newline - start);
        if (process_line(line)) {
            added_any = true;
        }

        start = newline + 1;
    }

    if (start > 0) {
        m_line_buffer.erase(0, start);
    }

    return added_any;
}

bool compiler_output_parser::finish() {
    if (m_line_buffer.empty()) {
        return false;
    }

    std::string tail = std::move(m_line_buffer);
    m_line_buffer.clear();

    return process_line(tail);
}

int compiler_output_parser::error_count() const {
    int count = 0;
    for (const script_diagnostic& item : m_diagnostics) {
        if (item.severity == script_diagnostic_severity::ERROR) {
            count++;
        }
    }

    return count;
}

int compiler_output_parser::warning_count() const {
    int count = 0;
    for (const script_diagnostic& item : m_diagnostics) {
        if (item.severity == script_diagnostic_severity::WARNING) {
            count++;
        }
    }

    return count;
}

bool compiler_output_parser::process_line(std::string_view raw_line) {
    std::string cleaned = strip_terminal_escapes(raw_line);
    std::string_view line(cleaned);

    size_t search_from = 0;
    while (search_from < line.size()) {
        size_t colon1 = line.find(':', search_from);
        if (colon1 == std::string_view::npos) {
            return false;
        }

        search_from = colon1 + 1;
        if (colon1 == 0) {
            continue;
        }

        int parsed_line = 0;
        size_t line_digits = 0;
        std::string_view after_first = line.substr(colon1 + 1);
        if (!after_first.empty() && after_first.front() >= '0' && after_first.front() <= '9'
            && !parse_positive_int_prefix(after_first, &parsed_line, &line_digits)) {
            return false;
        }

        if (parsed_line <= 0) {
            continue;
        }

        if (line_digits >= after_first.size() || after_first[line_digits] != ':') {
            continue;
        }

        std::string_view after_line = after_first.substr(line_digits + 1);
        int parsed_col = 0;
        size_t col_digits = 0;
        std::string_view remainder = after_line;
        if (parse_positive_int_prefix(after_line, &parsed_col, &col_digits)
            && col_digits < after_line.size() && after_line[col_digits] == ':') {
            remainder = after_line.substr(col_digits + 1);
        } else {
            parsed_col = 0;
        }

        script_diagnostic_severity severity = script_diagnostic_severity::ERROR;
        std::string message;
        if (!parse_compiler_severity_and_message(remainder, &severity, &message)) {
            continue;
        }

        std::string_view candidate = strip_tool_prefix(line.substr(0, colon1));
        std::string resolved = resolve_workspace_file_path(m_workspace_root, candidate, m_require_existing_files);
        if (resolved.empty()) {
            return false;
        }

        record_diagnostic({ resolved, parsed_line, parsed_col, severity, std::move(message) });
        return true;
    }

    return false;
}

void compiler_output_parser::record_diagnostic(const script_diagnostic& item) {
    if (std::find(m_diagnostics.begin(), m_diagnostics.end(), item) == m_diagnostics.end()) {
        m_diagnostics.push_back(item);
    }
}

bool is_cpp_source_path(std::string_view path) {
    std::string_view trimmed = trim_ascii_spaces(path);

    return ends_with_ignore_case(trimmed, ".cpp")
        || ends_with_ignore_case(trimmed, ".cc")
        || ends_with_ignore_case(trimmed, ".cxx");
}

bool is_c_or_cpp_source_path(std::string_view path) {
    std::string_view trimmed = trim_ascii_spaces(path);
    return ends_with_ignore_case(trimmed, ".c") || is_cpp_source_path(trimmed);
}

static int c_run_target_rank(std::string_view rel_path) {
    if (rel_path == "main.c" || rel_path == "main.cpp" || rel_path == "main.cc" || rel_path == "main.cxx") {
        return 0;
    }

    return rel_path.find('/') == std::string_view::npos ? 1 : 2;
}

std::vector<std::string> discover_c_run_targets(const std::string& workspace_root) {
    std::vector<std::string> targets = walk_workspace_files(
        workspace_root, is_c_or_cpp_source_path, is_common_ignored_directory);

    std::sort(targets.begin(), targets.end(), [&workspace_root](const std::string& lhs, const std::string& rhs) {
        std::string lhs_rel = relative_workspace_display_path(workspace_root, lhs);
        std::string rhs_rel = relative_workspace_display_path(workspace_root, rhs);
        int lhs_rank = c_run_target_rank(lhs_rel);
        int rhs_rank = c_run_target_rank(rhs_rel);
        if (lhs_rank != rhs_rank) {
            return lhs_rank < rhs_rank;
        }

        return lhs_rel < rhs_rel;
    });

    return targets;
}

static bool matches_dir_name(std::string_view dir_name, std::string_view target) {
    return dir_name.size() == target.size() && ends_with_ignore_case(dir_name, target);
}

static bool is_excluded_project_directory(std::string_view dir_name) {
    if (is_common_ignored_directory(dir_name)) {
        return true;
    }

    return matches_dir_name(dir_name, "test")
        || matches_dir_name(dir_name, "tests")
        || matches_dir_name(dir_name, "example")
        || matches_dir_name(dir_name, "examples");
}

std::vector<std::string> collect_project_c_cxx_sources(const std::string& workspace_root) {
    std::vector<std::string> sources = walk_workspace_files(
        workspace_root, is_c_or_cpp_source_path, is_excluded_project_directory);
    std::sort(sources.begin(), sources.end());

    return sources;
}

static std::string strip_trailing_slash(std::string text) {
    while (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }

    return text;
}

static uint64_t fnv1a_hash(std::string_view bytes) {
    uint64_t hash = FNV1A_OFFSET_BASIS;
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= FNV1A_PRIME;
    }

    return hash;
}

std::string compute_build_directory_path(std::string_view workspace_root) {
    std::filesystem::path raw_root{ std::string(trim_ascii_spaces(workspace_root)) };
    std::string normalized = strip_trailing_slash(raw_root.lexically_normal().generic_string());

    char hex[HEX_HASH_BUFFER_BYTES];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(fnv1a_hash(normalized)));

    return std::string(BUILD_DIR_PREFIX) + hex;
}

static std::string sanitize_binary_token(std::string_view raw_token) {
    std::string sanitized;
    sanitized.reserve(raw_token.size());

    for (unsigned char ch : raw_token) {
        if (std::isalnum(ch) || ch == '_' || ch == '-') {
            sanitized.push_back(static_cast<char>(ch));
        } else if (ch == '/' || ch == '.' || ch == ' ') {
            sanitized.push_back('_');
        }
    }

    while (!sanitized.empty() && sanitized.front() == '_') {
        sanitized.erase(sanitized.begin());
    }

    while (!sanitized.empty() && sanitized.back() == '_') {
        sanitized.pop_back();
    }

    return sanitized.empty() ? std::string("app") : sanitized;
}

std::string compute_target_output_binary_path(std::string_view workspace_root, std::string_view target_path) {
    std::string build_dir = compute_build_directory_path(workspace_root);
    std::string rel = relative_workspace_display_path(workspace_root, target_path);
    std::filesystem::path rel_path(rel);
    std::filesystem::path without_ext = rel_path.parent_path() / rel_path.stem();

    return build_dir + "/" + sanitize_binary_token(without_ext.generic_string());
}

std::string compute_project_output_binary_path(std::string_view workspace_root) {
    std::string build_dir = compute_build_directory_path(workspace_root);
    std::string normalized = strip_trailing_slash(
        std::filesystem::path{ std::string(trim_ascii_spaces(workspace_root)) }.lexically_normal().generic_string());
    std::string folder = std::filesystem::path(normalized).filename().string();

    return build_dir + "/" + sanitize_binary_token(folder);
}

static void append_common_compile_flags(const compiler_environment& env, const std::string& language_std,
                                        std::vector<std::string>& args) {
    std::string root = strip_trailing_slash(
        std::filesystem::path{ std::string(trim_ascii_spaces(env.workspace_root)) }.lexically_normal().generic_string());

    args.push_back(language_std);
    args.push_back("-Wall");
    args.push_back("-Wextra");
    args.push_back("-g");

    args.push_back("-iquote");
    args.push_back(root);
    if (env.has_src_dir) {
        args.push_back("-iquote");
        args.push_back(root + "/src");
    }

    if (env.has_include_dir) {
        args.push_back("-I" + root + "/include");
    }

    if (env.uses_stellux_gcc && env.has_stellux_usr_include) {
        args.push_back("-idirafter");
        args.push_back(std::string(STELLUX_USR_INCLUDE_DIR));
    }
}

static void append_link_tail(const compiler_environment& env, const std::string& output_binary,
                             std::vector<std::string>& args) {
    if (env.uses_stellux_gcc && env.has_stellux_libstlx) {
        args.push_back(std::string(STELLUX_LIBSTLX_PATH));
    }

    args.push_back("-lm");
    args.push_back("-o");
    args.push_back(output_binary);
}

compiler_build_plan build_compiler_plan(const compiler_environment& env, const std::vector<std::string>& sources,
                                        bool is_project_build) {
    compiler_build_plan plan;
    if (sources.empty()) {
        return plan;
    }

    plan.build_dir = compute_build_directory_path(env.workspace_root);
    plan.output_binary = (is_project_build || sources.size() != 1)
                       ? compute_project_output_binary_path(env.workspace_root)
                       : compute_target_output_binary_path(env.workspace_root, sources.front());

    std::vector<std::string> c_sources;
    std::vector<std::string> cxx_sources;
    for (const std::string& source : sources) {
        std::string rel = relative_workspace_display_path(env.workspace_root, source);
        if (is_cpp_source_path(rel)) {
            cxx_sources.push_back(std::move(rel));
        } else {
            c_sources.push_back(std::move(rel));
        }
    }

    if (cxx_sources.empty()) {
        compiler_step step{ env.c_compiler, {} };
        append_common_compile_flags(env, "-std=gnu17", step.arguments);
        step.arguments.insert(step.arguments.end(), c_sources.begin(), c_sources.end());
        append_link_tail(env, plan.output_binary, step.arguments);
        plan.steps.push_back(std::move(step));
        return plan;
    }

    if (c_sources.empty()) {
        compiler_step step{ env.cxx_compiler, {} };
        append_common_compile_flags(env, "-std=c++20", step.arguments);
        step.arguments.insert(step.arguments.end(), cxx_sources.begin(), cxx_sources.end());
        append_link_tail(env, plan.output_binary, step.arguments);
        plan.steps.push_back(std::move(step));
        return plan;
    }

    for (size_t i = 0; i < c_sources.size(); i++) {
        std::string obj_name = std::to_string(i) + "_" + sanitize_binary_token(c_sources[i]) + ".o";
        std::string obj_path = plan.build_dir + "/" + obj_name;
        plan.object_files.push_back(obj_path);

        compiler_step c_step{ env.c_compiler, {} };
        append_common_compile_flags(env, "-std=gnu17", c_step.arguments);
        c_step.arguments.insert(c_step.arguments.end(), { "-c", c_sources[i], "-o", obj_path });
        plan.steps.push_back(std::move(c_step));
    }

    compiler_step link_step{ env.cxx_compiler, {} };
    append_common_compile_flags(env, "-std=c++20", link_step.arguments);
    link_step.arguments.insert(link_step.arguments.end(), cxx_sources.begin(), cxx_sources.end());
    link_step.arguments.insert(link_step.arguments.end(), plan.object_files.begin(), plan.object_files.end());
    append_link_tail(env, plan.output_binary, link_step.arguments);
    plan.steps.push_back(std::move(link_step));

    return plan;
}

} // namespace sheen
