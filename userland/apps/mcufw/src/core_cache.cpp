#include "core_cache.hpp"

#include "digest.hpp"
#include "files.hpp"
#include "step_records.hpp"

#include <unistd.h>

#include <algorithm>
#include <set>
#include <string_view>

constexpr size_t           KEY_DIGITS              = 16;
constexpr std::string_view BUILD_PATH_PLACEHOLDER  = "{build.path}";
constexpr std::string_view SOURCE_PATH_PLACEHOLDER = "{build.source.path}";
constexpr std::string_view STAGING_INFIX           = ".staging-";

static bool is_inside(std::string_view path, const std::string& directory) {
    return path.size() > directory.size() && path.starts_with(directory) && path[directory.size()] == '/';
}

// `word` with `path` written as `placeholder` wherever it stands as whole path components
static std::string write_as_placeholder(std::string_view word, const std::string& path, std::string_view placeholder) {
    if (path.empty()) {
        return std::string(word);
    }

    std::string written;
    size_t at = 0;
    for (size_t found = word.find(path); found != std::string_view::npos; found = word.find(path, at)) {
        size_t after = found + path.size();
        bool ends_component = after == word.size() || word[after] == '/';
        written += word.substr(at, found - at);
        written += ends_component ? placeholder : std::string_view(path);
        at = after;
    }

    written += word.substr(at);
    return written;
}

bool core_cache_key(const std::vector<build_step>& core_steps, const std::string& build_directory,
                    const std::string& sketch_folder, const std::string& compiler_version, std::string& key,
                    std::string& error) {
    sha256_digest digest;
    std::vector<std::string> response_files;
    for (const build_step& step : core_steps) {
        for (const std::vector<std::string>& command : step.commands) {
            for (const std::string& word : command) {
                std::string written = write_as_placeholder(word, build_directory, BUILD_PATH_PLACEHOLDER);
                digest.update(write_as_placeholder(written, sketch_folder, SOURCE_PATH_PLACEHOLDER));
                digest.update(std::string_view("\0", 1));

                std::string file = is_response_word(word) ? word.substr(1) : std::string();
                bool reads_build_file = !file.empty() && is_inside(file, build_directory);
                if (reads_build_file &&
                    std::find(response_files.begin(), response_files.end(), file) == response_files.end()) {
                    response_files.push_back(file);
                }
            }

            digest.update("\n");
        }
    }

    for (const std::string& file : response_files) {
        std::string text;
        if (!read_file(file, text, error)) {
            return false;
        }

        sha256_digest content;
        content.update(text);
        digest.update(write_as_placeholder(file, build_directory, BUILD_PATH_PLACEHOLDER) + " " + content.hex() + "\n");
    }

    digest.update(compiler_version);
    key = digest.hex().substr(0, KEY_DIGITS);
    return true;
}

std::string core_cache_entry(const directory_layout& layout, const std::string& board_id, const std::string& key) {
    return join_path(layout.core_cache, board_id + "-" + key);
}

bool core_reads_sketch(const std::vector<build_step>& core_compiles, const std::string& sketch_folder,
                       bool missing_reads, bool& reads, std::string& error) {
    reads = false;
    for (const build_step& compile : core_compiles) {
        std::string depfile = compile_depfile(compile);
        if (!is_file(depfile)) {
            reads = missing_reads;
            if (reads) {
                return true;
            }

            continue;
        }

        std::vector<std::string> files;
        if (!read_depfile(depfile, files, error)) {
            return false;
        }

        bool reads_sketch_file = std::any_of(files.begin(), files.end(), [&sketch_folder](const std::string& file) {
            return is_inside(file, sketch_folder);
        });

        if (reads_sketch_file) {
            reads = true;
            return true;
        }
    }

    return true;
}

bool core_header_names(const std::vector<build_step>& core_compiles, std::vector<std::string>& names,
                       std::string& error) {
    std::set<std::string> found;
    for (const build_step& compile : core_compiles) {
        std::vector<std::string> files;
        if (!read_depfile(compile_depfile(compile), files, error)) {
            return false;
        }

        for (size_t index = 1; index < files.size(); index++) {
            found.insert(file_name(files[index]));
        }
    }

    names.assign(found.begin(), found.end());
    return true;
}

bool sketch_shadows(const std::string& sketch_folder, const std::vector<std::string>& header_names, bool& shadows,
                    std::string& error) {
    std::vector<std::string> names;
    if (!list_directory(sketch_folder, names, error)) {
        return false;
    }

    shadows = std::any_of(names.begin(), names.end(), [&header_names](const std::string& name) {
        return std::find(header_names.begin(), header_names.end(), name) != header_names.end();
    });

    return true;
}

bool publish_core(const std::string& archive, const std::vector<std::string>& header_names, const std::string& entry,
                  std::string& error) {
    std::string placed = join_path(entry, CORE_ARCHIVE_NAME);
    if (is_file(placed)) {
        return remove_tree(archive, error);
    }

    std::string listed;
    for (const std::string& name : header_names) {
        listed += name + "\n";
    }

    std::string staging = entry + std::string(STAGING_INFIX) + std::to_string(getpid());
    bool is_staged = remove_tree(staging, error) && make_directories(staging, error) &&
                     write_new_file(join_path(staging, CORE_HEADERS_NAME), listed, error) &&
                     rename_path(archive, join_path(staging, CORE_ARCHIVE_NAME), error);
    if (!is_staged) {
        return false;
    }

    std::string rename_error;
    if (rename_path(staging, entry, rename_error)) {
        return true;
    }

    if (!is_file(placed)) {
        error = rename_error;
        return false;
    }

    return remove_tree(staging, error);
}

bool published_header_names(const std::string& entry, std::vector<std::string>& names) {
    std::string listed;
    std::string error;
    if (!read_file(join_path(entry, CORE_HEADERS_NAME), listed, error)) {
        return false;
    }

    names.clear();
    for (size_t start = 0; start < listed.size();) {
        size_t end = std::min(listed.find('\n', start), listed.size());
        names.push_back(listed.substr(start, end - start));
        start = end + 1;
    }

    return true;
}
