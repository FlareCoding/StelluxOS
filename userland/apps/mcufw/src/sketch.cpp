#include "sketch.hpp"

#include "files.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <regex>
#include <string_view>

// The extensions of the files a build copies beside the merged sketch, the sources among them compiled when they sit in
// its top folder or `src/`
constexpr std::string_view SOURCE_EXTENSIONS[] = { ".c", ".cpp", ".cxx", ".cc", ".S" };
constexpr std::string_view COPIED_EXTENSIONS[] = { ".h", ".hpp", ".hh", ".adoc", ".md", ".json", ".tpp", ".ipp" };
constexpr std::string_view SKETCH_EXTENSIONS[] = { ".ino", ".pde" };

// A folder holding this file is a build tree, which a sketch never takes files from
constexpr const char* BUILD_OPTIONS_FILE = "build.options.json";

constexpr const char* SETTINGS_FILE = "sketch.yaml";
constexpr const char* SETTINGS_FILE_ALTERNATE = "sketch.yml";

constexpr std::string_view UTF8_BYTE_ORDER_MARK = "\xEF\xBB\xBF";

static std::string extension_of(const std::string& name) {
    size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot);
}

template <size_t count>
static bool is_one_of(const std::string& extension, const std::string_view (&extensions)[count]) {
    for (std::string_view candidate : extensions) {
        if (extension == candidate) {
            return true;
        }
    }

    return false;
}

static bool canonical_path(const std::string& path, std::string& canonical, std::string& error) {
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) == nullptr) {
        error = "no sketch at " + path;
        return false;
    }

    canonical = resolved;
    return true;
}

// Every file below `folder` a sketch may hold, by its path relative to the sketch, skipping build trees and files
// whose names start with a dot. Dot folders are walked, and a symbolic link loop fails, as arduino-cli walks a sketch.
static bool collect_files(const std::string& folder, const std::string& relative, std::vector<std::string>& walked,
                          std::vector<std::string>& files, std::string& error) {
    std::vector<std::string> names;
    if (!enter_folder(folder, walked, error) || !list_directory(folder, names, error)) {
        return false;
    }

    for (const std::string& name : names) {
        std::string path = join_path(folder, name);
        std::string child = relative.empty() ? name : relative + "/" + name;
        if (is_directory(path)) {
            bool is_build_tree = is_file(join_path(path, BUILD_OPTIONS_FILE));
            if (!is_build_tree && !collect_files(path, child, walked, files, error)) {
                return false;
            }

            continue;
        }

        std::string extension = extension_of(name);
        bool is_wanted = is_one_of(extension, SOURCE_EXTENSIONS) || is_one_of(extension, COPIED_EXTENSIONS) ||
                         is_one_of(extension, SKETCH_EXTENSIONS);
        if (name[0] != '.' && is_wanted) {
            files.push_back(child);
        }
    }

    walked.pop_back();

    return true;
}

bool load_sketch(const std::string& path, sketch& found, std::string& error) {
    std::string folder;
    if (!canonical_path(path, folder, error)) {
        return false;
    }

    if (!is_directory(folder) && is_one_of(extension_of(file_name(folder)), SKETCH_EXTENSIONS)) {
        folder = parent_directory(folder);
    }

    found = {};
    found.folder = folder;
    found.name = file_name(folder);
    for (std::string_view extension : SKETCH_EXTENSIONS) {
        std::string candidate = found.name + std::string(extension);
        if (!is_file(join_path(folder, candidate))) {
            continue;
        }

        if (!found.main_file.empty()) {
            error = found.folder + " holds two main files, " + found.main_file + " and " + candidate;
            return false;
        }

        found.main_file = candidate;
    }

    if (found.main_file.empty()) {
        error = "no main file " + join_path(folder, found.name + ".ino") + " in the sketch";
        return false;
    }

    std::vector<std::string> walked;
    std::vector<std::string> files;
    if (!collect_files(folder, "", walked, files, error)) {
        return false;
    }

    for (const std::string& file : files) {
        std::string extension = extension_of(file);
        bool in_root = file.find('/') == std::string::npos;
        if (is_one_of(extension, SKETCH_EXTENSIONS)) {
            if (in_root && file != found.main_file) {
                found.other_ino_files.push_back(file);
            }
        } else if (file.rfind("build/", 0) != 0) {
            bool compiles = is_one_of(extension, SOURCE_EXTENSIONS) && (in_root || file.rfind("src/", 0) == 0);
            (compiles ? found.source_files : found.copied_files).push_back(file);
        }
    }

    auto in_root_then_by_path = [](const std::string& a, const std::string& b) {
        bool a_in_root = a.find('/') == std::string::npos;
        bool b_in_root = b.find('/') == std::string::npos;
        return a_in_root != b_in_root ? a_in_root : a < b;
    };

    std::sort(found.source_files.begin(), found.source_files.end(), in_root_then_by_path);
    return true;
}

std::string line_mark(uint32_t line, const std::string& path) {
    std::string mark = "#line " + std::to_string(line) + " \"";
    for (char c : path) {
        if (c == '\\' || c == '"') {
            mark += '\\';
        }

        mark += c;
    }

    return mark + "\"";
}

static bool read_sketch_file(const sketch& sketch, const std::string& name, std::string& text, std::string& error) {
    if (!read_file(join_path(sketch.folder, name), text, error)) {
        return false;
    }

    if (text.rfind(UTF8_BYTE_ORDER_MARK, 0) == 0) {
        text.erase(0, UTF8_BYTE_ORDER_MARK.size());
    }

    return true;
}

bool copied_file_text(const sketch& sketch, const std::string& relative, std::string& text, std::string& error) {
    if (!read_sketch_file(sketch, relative, text, error)) {
        return false;
    }

    text = line_mark(1, join_path(sketch.folder, relative)) + "\n" + text;

    return true;
}

bool merge_ino_files(const sketch& sketch, merged_sketch& merged, std::string& error) {
    static const std::regex includes_arduino(R"(^\s*#\s*include\s*[<"]Arduino\.h[>"])", std::regex::multiline);

    std::string main_text;
    if (!read_sketch_file(sketch, sketch.main_file, main_text, error)) {
        return false;
    }

    merged = {};
    if (!std::regex_search(main_text, includes_arduino)) {
        merged.text = "#include <Arduino.h>\n";
        merged.line_offset++;
    }

    merged.text += line_mark(1, join_path(sketch.folder, sketch.main_file)) + "\n" + main_text + "\n";
    merged.line_offset++;
    for (const std::string& name : sketch.other_ino_files) {
        std::string text;
        if (!read_sketch_file(sketch, name, text, error)) {
            return false;
        }

        merged.text += line_mark(1, join_path(sketch.folder, name)) + "\n" + text + "\n";
    }

    return true;
}

static std::string settings_path(const sketch& sketch) {
    std::string path = join_path(sketch.folder, SETTINGS_FILE);
    std::string alternate = join_path(sketch.folder, SETTINGS_FILE_ALTERNATE);
    return !is_file(path) && is_file(alternate) ? alternate : path;
}

// The value of a top-level `key: value` line, quotes and a trailing comment removed, or false for any other line
static bool read_setting(std::string_view line, std::string_view key, std::string& value) {
    if (line.substr(0, key.size()) != key || line.substr(key.size(), 1) != ":") {
        return false;
    }

    std::string_view rest = line.substr(key.size() + 1);
    size_t first = rest.find_first_not_of(" \t");
    rest = first == std::string_view::npos ? std::string_view() : rest.substr(first);
    if (!rest.empty() && (rest[0] == '"' || rest[0] == '\'')) {
        size_t close = rest.find(rest[0], 1);
        value = rest.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1);
        return true;
    }

    size_t comment = rest.find(" #");
    rest = rest.substr(0, comment);
    size_t last = rest.find_last_not_of(" \t\r");
    value = last == std::string_view::npos ? std::string() : std::string(rest.substr(0, last + 1));
    return true;
}

bool read_sketch_settings(const sketch& sketch, sketch_settings& settings, std::string& error) {
    settings = {};
    std::string path = settings_path(sketch);
    if (!is_file(path)) {
        return true;
    }

    std::string text;
    if (!read_file(path, text, error)) {
        return false;
    }

    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        std::string_view line = std::string_view(text).substr(start, end - start);
        if (!read_setting(line, "default_fqbn", settings.fqbn)) {
            read_setting(line, "default_port", settings.port);
        }

        start = end == std::string::npos ? text.size() : end + 1;
    }

    return true;
}

bool save_sketch_fqbn(const sketch& sketch, const std::string& fqbn, std::string& error) {
    std::string path = settings_path(sketch);
    std::string text;
    if (is_file(path) && !read_file(path, text, error)) {
        return false;
    }

    std::string setting = "default_fqbn: " + fqbn;
    std::string updated;
    bool replaced = false;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        std::string_view line = std::string_view(text).substr(start, end - start);
        std::string ignored;
        if (!replaced && read_setting(line, "default_fqbn", ignored)) {
            updated += setting;
            replaced = true;
        } else {
            updated += line;
        }

        if (end != std::string::npos) {
            updated += '\n';
        }

        start = end == std::string::npos ? text.size() : end + 1;
    }

    if (!replaced) {
        if (!updated.empty() && updated.back() != '\n') {
            updated += '\n';
        }

        updated += setting + "\n";
    }

    bool changed = false;

    return write_file_if_changed(path, updated, changed, error);
}
