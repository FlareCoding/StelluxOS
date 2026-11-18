#include "extensions/workspace_paths.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <system_error>

namespace sheen {

static constexpr size_t MAX_DISCOVERED_FILES = 512;
static constexpr size_t MAX_DISCOVERY_DEPTH = 6;
static constexpr int MAX_DIAGNOSTIC_LOCATION = 10000000;

static bool is_ascii_space(char ch) {
    return ch == ' ' || ch == '\t';
}

std::string_view trim_ascii_spaces(std::string_view text) {
    size_t start = 0;
    while (start < text.size() && is_ascii_space(text[start])) {
        start++;
    }

    size_t end = text.size();
    while (end > start && is_ascii_space(text[end - 1])) {
        end--;
    }

    return text.substr(start, end - start);
}

static bool equals_ignore_case(std::string_view first, std::string_view second) {
    if (first.size() != second.size()) {
        return false;
    }

    for (size_t i = 0; i < first.size(); i++) {
        unsigned char lhs = static_cast<unsigned char>(first[i]);
        unsigned char rhs = static_cast<unsigned char>(second[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }

    return true;
}

bool ends_with_ignore_case(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }

    return equals_ignore_case(text.substr(text.size() - suffix.size()), suffix);
}

bool parse_positive_int_prefix(std::string_view text, int* parsed_value, size_t* consumed_chars) {
    size_t idx = 0;
    int64_t accumulator = 0;
    while (idx < text.size() && text[idx] >= '0' && text[idx] <= '9') {
        accumulator = accumulator * 10 + (text[idx] - '0');
        if (accumulator > MAX_DIAGNOSTIC_LOCATION) {
            return false;
        }

        idx++;
    }

    if (idx == 0 || accumulator <= 0) {
        return false;
    }

    *parsed_value = static_cast<int>(accumulator);
    *consumed_chars = idx;

    return true;
}

void terminal_escape_filter::reset() {
    m_state = escape_state::TEXT;
}

std::string terminal_escape_filter::feed(std::string_view chunk) {
    std::string cleaned;
    cleaned.reserve(chunk.size());

    for (char ch : chunk) {
        unsigned char byte = static_cast<unsigned char>(ch);
        switch (m_state) {
        case escape_state::TEXT:
            if (ch == '\r') {
                break;
            }

            if (ch == '\x1b') {
                m_state = escape_state::AFTER_ESC;
                break;
            }

            cleaned.push_back(ch);
            break;

        case escape_state::AFTER_ESC:
            if (ch == '[') {
                m_state = escape_state::CSI;
            } else if (ch == ']') {
                m_state = escape_state::OSC;
            } else {
                m_state = escape_state::TEXT;
            }

            break;

        case escape_state::CSI:
            if (byte >= 0x40 && byte <= 0x7E) {
                m_state = escape_state::TEXT;
            }

            break;

        case escape_state::OSC:
            if (ch == '\x07') {
                m_state = escape_state::TEXT;
            } else if (ch == '\x1b') {
                m_state = escape_state::OSC_ESC;
            }

            break;

        case escape_state::OSC_ESC:
            if (ch == '[') {
                m_state = escape_state::CSI;
            } else if (ch == ']') {
                m_state = escape_state::OSC;
            } else {
                m_state = escape_state::TEXT;
            }

            break;
        }
    }

    return cleaned;
}

std::string strip_terminal_escapes(std::string_view text) {
    terminal_escape_filter filter;
    return filter.feed(text);
}

static std::filesystem::path strip_trailing_separator(const std::filesystem::path& normalized) {
    std::string text = normalized.generic_string();
    while (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }

    return std::filesystem::path(text);
}

static std::filesystem::path normalize_against_disk(const std::filesystem::path& raw_path, bool use_canonical) {
    if (use_canonical) {
        std::error_code ec;
        std::filesystem::path canonical = std::filesystem::weakly_canonical(raw_path, ec);
        if (!ec && !canonical.empty()) {
            return strip_trailing_separator(canonical.lexically_normal());
        }
    }

    return strip_trailing_separator(raw_path.lexically_normal());
}

static bool is_strictly_within_root(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    std::filesystem::path rel = candidate.lexically_relative(root);
    if (rel.empty()) {
        return false;
    }

    std::string rel_str = rel.generic_string();
    if (rel_str == "." || rel_str == "..") {
        return false;
    }

    return !rel_str.starts_with("../");
}

std::string resolve_workspace_file_path(std::string_view workspace_root, std::string_view candidate_path,
                                        bool require_existing_file) {
    std::string_view trimmed_root = trim_ascii_spaces(workspace_root);
    std::string_view trimmed_candidate = trim_ascii_spaces(candidate_path);
    if (trimmed_root.empty() || trimmed_candidate.empty()) {
        return {};
    }

    if (trimmed_candidate.front() == '<' && trimmed_candidate.back() == '>') {
        return {};
    }

    std::filesystem::path raw_root{ std::string(trimmed_root) };
    if (!raw_root.is_absolute()) {
        return {};
    }

    std::error_code ec;
    bool root_exists = std::filesystem::exists(raw_root, ec) && !ec;
    std::filesystem::path norm_root = normalize_against_disk(raw_root, root_exists);

    std::filesystem::path raw_candidate{ std::string(trimmed_candidate) };
    std::filesystem::path combined = raw_candidate.is_absolute() ? raw_candidate : (norm_root / raw_candidate);
    std::filesystem::path norm_candidate = normalize_against_disk(combined, root_exists);
    if (!is_strictly_within_root(norm_root, norm_candidate)) {
        return {};
    }

    if (require_existing_file) {
        bool is_file = std::filesystem::is_regular_file(norm_candidate, ec) && !ec;
        if (!is_file) {
            return {};
        }
    }

    return norm_candidate.generic_string();
}

std::string relative_workspace_display_path(std::string_view workspace_root, std::string_view file_path) {
    std::string resolved = resolve_workspace_file_path(workspace_root, file_path, false);
    if (resolved.empty()) {
        return std::string(trim_ascii_spaces(file_path));
    }

    std::filesystem::path raw_root{ std::string(trim_ascii_spaces(workspace_root)) };
    std::error_code ec;
    bool root_exists = std::filesystem::exists(raw_root, ec) && !ec;
    std::filesystem::path norm_root = normalize_against_disk(raw_root, root_exists);
    std::filesystem::path rel = std::filesystem::path(resolved).lexically_relative(norm_root);

    return rel.empty() ? resolved : rel.generic_string();
}

bool is_common_ignored_directory(std::string_view dir_name) {
    if (dir_name.empty() || dir_name.front() == '.') {
        return true;
    }

    return dir_name == "__pycache__"
        || dir_name == "build"
        || dir_name == "out"
        || dir_name == "dist"
        || dir_name == "node_modules"
        || dir_name == "venv";
}

static void walk_directory_recursive(const std::filesystem::path& root, const std::filesystem::path& current,
                                     size_t depth, bool (*include_file)(std::string_view),
                                     bool (*skip_directory)(std::string_view), std::vector<std::string>& collected) {
    if (depth > MAX_DISCOVERY_DEPTH || collected.size() >= MAX_DISCOVERED_FILES) {
        return;
    }

    std::error_code ec;
    std::filesystem::directory_iterator iter(current, ec);
    if (ec) {
        return;
    }

    std::vector<std::filesystem::path> subdirs;
    for (const std::filesystem::directory_entry& entry : iter) {
        if (collected.size() >= MAX_DISCOVERED_FILES) {
            break;
        }

        std::string name = entry.path().filename().string();
        if (entry.is_symlink(ec)) {
            continue;
        }

        if (entry.is_directory(ec)) {
            if (!skip_directory(name)) {
                subdirs.push_back(entry.path());
            }

            continue;
        }

        if (entry.is_regular_file(ec) && include_file(name)) {
            std::filesystem::path normalized = normalize_against_disk(entry.path(), true);
            if (is_strictly_within_root(root, normalized)) {
                collected.push_back(normalized.generic_string());
            }
        }
    }

    std::sort(subdirs.begin(), subdirs.end());
    for (const std::filesystem::path& subdir : subdirs) {
        walk_directory_recursive(root, subdir, depth + 1, include_file, skip_directory, collected);
    }
}

std::vector<std::string> walk_workspace_files(const std::string& workspace_root,
                                              bool (*include_file)(std::string_view),
                                              bool (*skip_directory)(std::string_view)) {
    std::filesystem::path raw_root(workspace_root);
    std::error_code ec;
    if (!std::filesystem::is_directory(raw_root, ec) || ec) {
        return {};
    }

    std::filesystem::path norm_root = normalize_against_disk(raw_root, true);
    std::vector<std::string> collected;
    walk_directory_recursive(norm_root, norm_root, 0, include_file, skip_directory, collected);

    return collected;
}

} // namespace sheen
