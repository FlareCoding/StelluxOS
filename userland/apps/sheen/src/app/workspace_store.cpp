#include "app/workspace_store.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace sheen {

static constexpr size_t MAX_RECENT_PATHS = 20;
static constexpr uint64_t FNV1A_OFFSET = 14695981039346656037ull;
static constexpr uint64_t FNV1A_PRIME = 1099511628211ull;

static std::string trim_ascii(std::string_view text) {
    size_t first = 0;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r')) {
        first++;
    }

    size_t last = text.size();
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r')) {
        last--;
    }

    return std::string(text.substr(first, last - first));
}

static std::vector<std::string> split_words(std::string_view value) {
    std::vector<std::string> words;
    size_t index = 0;
    while (index < value.size()) {
        while (index < value.size() && (value[index] == ' ' || value[index] == '\t')) {
            index++;
        }

        if (index >= value.size()) {
            break;
        }

        size_t start = index;
        while (index < value.size() && value[index] != ' ' && value[index] != '\t') {
            index++;
        }

        std::string word(value.substr(start, index - start));
        if (std::find(words.begin(), words.end(), word) == words.end()) {
            words.push_back(std::move(word));
        }
    }

    return words;
}

static std::string join_words(const std::vector<std::string>& words) {
    std::string joined;
    for (const std::string& word : words) {
        if (word.empty()) {
            continue;
        }

        if (!joined.empty()) {
            joined += ' ';
        }

        joined += word;
    }

    return joined;
}

static bool write_file_atomically(const std::filesystem::path& target, const std::string& contents) {
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::string pattern = (target.parent_path() / ".sheen-tmp-XXXXXX").string();
    std::vector<char> mutable_path(pattern.begin(), pattern.end());
    mutable_path.push_back('\0');

    int fd = ::mkstemp(mutable_path.data());
    if (fd < 0) {
        return false;
    }

    size_t offset = 0;
    bool ok = true;
    while (offset < contents.size()) {
        ssize_t written = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (written <= 0) {
            ok = false;
            break;
        }

        offset += static_cast<size_t>(written);
    }

    ::close(fd);
    if (!ok || ::rename(mutable_path.data(), target.c_str()) != 0) {
        ::unlink(mutable_path.data());
        return false;
    }

    return true;
}

static int parse_bounded_int(const std::string& text, int minimum, int maximum, int fallback) {
    char* end = nullptr;
    long parsed = std::strtol(text.c_str(), &end, 10);
    if (!end || *end != '\0') {
        return fallback;
    }

    return static_cast<int>(std::clamp<long>(parsed, minimum, maximum));
}

workspace_store::workspace_store(std::string base_dir) : m_base_dir(std::move(base_dir)) {
}

std::string workspace_store::normalize_workspace_path(std::string_view root_path) {
    std::string trimmed = trim_ascii(root_path);
    if (trimmed.empty()) {
        return "/";
    }

    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(std::filesystem::path(trimmed), ec);
    std::string normalized = ec ? std::filesystem::path(trimmed).lexically_normal().string() : canonical.string();
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }

    return normalized.empty() ? "/" : normalized;
}

std::string workspace_store::workspace_file_key(std::string_view root_path) {
    std::string normalized = normalize_workspace_path(root_path);
    uint64_t hash = FNV1A_OFFSET;
    for (unsigned char byte : normalized) {
        hash ^= byte;
        hash *= FNV1A_PRIME;
    }

    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));

    return std::string(hex);
}

std::string workspace_store::recent_file_path() const {
    return (std::filesystem::path(m_base_dir) / "recent.conf").string();
}

std::string workspace_store::workspace_conf_path(std::string_view normalized_root) const {
    std::string filename = workspace_file_key(normalized_root) + ".conf";
    return (std::filesystem::path(m_base_dir) / "workspaces" / filename).string();
}

std::vector<std::string> workspace_store::load_recent_paths() const {
    std::ifstream input(recent_file_path());
    if (!input.is_open()) {
        return {};
    }

    std::vector<std::string> paths;
    std::string line;
    while (std::getline(input, line)) {
        std::string trimmed = trim_ascii(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        if (std::find(paths.begin(), paths.end(), trimmed) == paths.end()) {
            paths.push_back(std::move(trimmed));
            if (paths.size() >= MAX_RECENT_PATHS) {
                break;
            }
        }
    }

    return paths;
}

bool workspace_store::record_recent_path(std::string_view path) {
    std::string clean = trim_ascii(path);
    if (clean.empty()) {
        return false;
    }

    std::vector<std::string> paths = load_recent_paths();
    paths.erase(std::remove(paths.begin(), paths.end(), clean), paths.end());
    paths.insert(paths.begin(), clean);
    if (paths.size() > MAX_RECENT_PATHS) {
        paths.resize(MAX_RECENT_PATHS);
    }

    std::ostringstream out;
    for (const std::string& entry : paths) {
        out << entry << '\n';
    }

    return write_file_atomically(recent_file_path(), out.str());
}

bool workspace_store::clear_recent_paths() {
    return write_file_atomically(recent_file_path(), "");
}

workspace_record workspace_store::load_workspace(std::string_view root_path) const {
    workspace_record record;
    record.root_path = normalize_workspace_path(root_path);

    std::ifstream input(workspace_conf_path(record.root_path));
    if (!input.is_open()) {
        return record;
    }

    std::string line;
    while (std::getline(input, line)) {
        std::string trimmed = trim_ascii(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == '[') {
            continue;
        }

        size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        std::string key = trim_ascii(std::string_view(trimmed).substr(0, equals));
        std::string value = trim_ascii(std::string_view(trimmed).substr(equals + 1));
        if (key == "enabled") {
            record.enabled_extensions = split_words(value);
        } else if (key == "dismissed") {
            record.dismissed_offers = split_words(value);
        } else if (key == "pinned_strip") {
            record.pinned_strip_extension = value;
        } else if (key == "dock_height") {
            record.dock_height = parse_bounded_int(value, MIN_DOCK_HEIGHT, MAX_DOCK_HEIGHT, DEFAULT_DOCK_HEIGHT);
        } else if (key == "dock_collapsed") {
            record.is_dock_collapsed = (value == "true");
        } else if (key == "active_dock") {
            record.active_dock_panel = value;
        } else if (key == "sidebar_visible") {
            record.is_sidebar_visible = (value != "false");
        } else if (key == "sidebar_width") {
            record.sidebar_width = parse_bounded_int(value, MIN_SIDEBAR_WIDTH, MAX_SIDEBAR_WIDTH,
                                                     DEFAULT_SIDEBAR_WIDTH);
        }
    }

    return record;
}

bool workspace_store::save_workspace(const workspace_record& record) {
    std::string normalized = normalize_workspace_path(record.root_path);
    int dock_height = std::clamp(record.dock_height, MIN_DOCK_HEIGHT, MAX_DOCK_HEIGHT);
    int sidebar_width = std::clamp(record.sidebar_width, MIN_SIDEBAR_WIDTH, MAX_SIDEBAR_WIDTH);

    std::ostringstream out;
    out << "[workspace]\n";
    out << "root = " << normalized << '\n';
    out << "enabled = " << join_words(record.enabled_extensions) << '\n';
    out << "dismissed = " << join_words(record.dismissed_offers) << '\n';
    out << "pinned_strip = " << record.pinned_strip_extension << '\n';

    out << "dock_height = " << dock_height << '\n';
    out << "dock_collapsed = " << (record.is_dock_collapsed ? "true" : "false") << '\n';
    out << "active_dock = " << record.active_dock_panel << '\n';
    out << "sidebar_visible = " << (record.is_sidebar_visible ? "true" : "false") << '\n';
    out << "sidebar_width = " << sidebar_width << '\n';

    return write_file_atomically(workspace_conf_path(normalized), out.str());
}

} // namespace sheen
