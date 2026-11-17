#include "step_records.hpp"

#include "digest.hpp"
#include "files.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

constexpr std::string_view RECORD_SUFFIX   = ".step";
constexpr std::string_view STEPS_DIRECTORY = "steps";
constexpr std::string_view OBJECT_SUFFIX   = ".o";
constexpr std::string_view DEPFILE_SUFFIX  = ".d";
constexpr std::string_view COMMAND_LINE    = "command";
constexpr std::string_view WORD_PREFIX     = "word ";
constexpr std::string_view FILE_PREFIX     = "file ";
constexpr std::string_view NO_DIGEST       = "-";

static bool has_path(const std::vector<file_stamp>& stamps, const std::string& path) {
    return std::any_of(stamps.begin(), stamps.end(), [&path](const file_stamp& stamp) { return stamp.path == path; });
}

static void add_stamp(std::vector<file_stamp>& stamps, const std::string& path, bool by_content) {
    file_stamp stamp;
    if (!has_path(stamps, path) && stamp_file(path, by_content, stamp)) {
        stamps.push_back(stamp);
    }
}

static std::string format_record(const step_record& record) {
    std::string text;
    for (const std::vector<std::string>& command : record.commands) {
        text += std::string(COMMAND_LINE) + "\n";
        for (const std::string& word : command) {
            text += std::string(WORD_PREFIX) + word + "\n";
        }
    }

    for (const file_stamp& stamp : record.files) {
        text += std::string(FILE_PREFIX) + format_file_stamp(stamp) + "\n";
    }

    return text;
}

static bool parse_record(std::string_view text, step_record& record) {
    record = {};
    while (!text.empty()) {
        size_t end = text.find('\n');
        if (end == std::string_view::npos) {
            return false;
        }

        std::string_view line = text.substr(0, end);
        text.remove_prefix(end + 1);
        if (line == COMMAND_LINE) {
            record.commands.emplace_back();
            continue;
        }

        if (line.starts_with(WORD_PREFIX) && !record.commands.empty()) {
            record.commands.back().emplace_back(line.substr(WORD_PREFIX.size()));
            continue;
        }

        file_stamp stamp;
        if (!line.starts_with(FILE_PREFIX) || !parse_file_stamp(line.substr(FILE_PREFIX.size()), stamp)) {
            return false;
        }

        record.files.push_back(stamp);
    }

    return true;
}

std::string step_record_path(const build_step& step, const std::string& build_directory) {
    if (step.kind == step_kind::compile || step.kind == step_kind::archive) {
        return step.output + std::string(RECORD_SUFFIX);
    }

    bool always_runs = step.kind == step_kind::size || (step.kind == step_kind::hook && step.output.empty());
    if (always_runs) {
        return {};
    }

    return join_path(join_path(build_directory, STEPS_DIRECTORY), step.recipe + std::string(RECORD_SUFFIX));
}

bool is_response_word(std::string_view word) {
    return word.size() > 2 && word[0] == '@' && word[1] == '/';
}

std::string compile_depfile(const build_step& compile) {
    std::string_view base = compile.output;
    if (base.ends_with(OBJECT_SUFFIX)) {
        base.remove_suffix(OBJECT_SUFFIX.size());
    }

    return std::string(base) + std::string(DEPFILE_SUFFIX);
}

std::string format_file_stamp(const file_stamp& stamp) {
    std::string digest = stamp.digest.empty() ? std::string(NO_DIGEST) : stamp.digest;
    return std::to_string(stamp.size) + " " + std::to_string(stamp.modified_ns) + " " + digest + " " + stamp.path;
}

bool parse_file_stamp(std::string_view text, file_stamp& stamp) {
    size_t size_end = text.find(' ');
    size_t time_end = size_end == std::string_view::npos ? size_end : text.find(' ', size_end + 1);
    size_t digest_end = time_end == std::string_view::npos ? time_end : text.find(' ', time_end + 1);
    if (digest_end == std::string_view::npos || digest_end + 1 >= text.size()) {
        return false;
    }

    std::string size(text.substr(0, size_end));
    std::string time(text.substr(size_end + 1, time_end - size_end - 1));
    char* size_rest = nullptr;
    char* time_rest = nullptr;
    stamp.size = strtoull(size.c_str(), &size_rest, 10);
    stamp.modified_ns = strtoll(time.c_str(), &time_rest, 10);

    std::string_view digest = text.substr(time_end + 1, digest_end - time_end - 1);
    stamp.digest = digest == NO_DIGEST ? std::string() : std::string(digest);
    stamp.path = std::string(text.substr(digest_end + 1));

    return !size.empty() && *size_rest == '\0' && !time.empty() && *time_rest == '\0';
}

bool stamp_file(const std::string& path, bool by_content, file_stamp& stamp) {
    stamp = { path, 0, 0, {} };
    if (!read_file_size_and_time(path, stamp.size, stamp.modified_ns)) {
        return false;
    }

    if (!by_content) {
        return true;
    }

    std::string text;
    std::string error;
    if (!read_file(path, text, error)) {
        return false;
    }

    sha256_digest digest;
    digest.update(text);
    stamp.digest = digest.hex();
    return true;
}

bool read_depfile(const std::string& path, std::vector<std::string>& files, std::string& error) {
    std::string text;
    if (!read_file(path, text, error)) {
        return false;
    }

    std::vector<std::string> items;
    std::string item;
    bool after_backslash = false;
    bool after_dollar = false;
    for (char c : text) {
        if (c == '\r') {
            continue;
        }

        if (after_backslash) {
            after_backslash = false;
            if (c == ' ' || c == '#' || c == '\\') {
                item += c;
            } else if (c != '\n') {
                item += '\\';
                item += c;
            }

            continue;
        }

        if (after_dollar) {
            if (c != '$') {
                error = path + " holds a $ that does not start $$";
                return false;
            }

            item += '$';
            after_dollar = false;
            continue;
        }

        if (c == '\\' || c == '$') {
            after_backslash = c == '\\';
            after_dollar = c == '$';
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!item.empty()) {
                items.push_back(item);
                item.clear();
            }

            continue;
        }

        item += c;
    }

    if (after_dollar) {
        error = path + " ends inside a $$";
        return false;
    }

    if (!item.empty()) {
        items.push_back(item);
    }

    files.clear();
    if (items.empty()) {
        return true;
    }

    if (!items[0].ends_with(':')) {
        error = path + " names no target before its files";
        return false;
    }

    files.assign(items.begin() + 1, items.end());
    return true;
}

bool record_matches(const step_record& record, const build_step& step, const std::vector<file_stamp>& current,
                    std::string& reason) {
    if (record.commands != step.commands) {
        reason = std::string(COMMAND_CHANGED_REASON);
        return false;
    }

    for (size_t index = 0; index < record.files.size(); index++) {
        const file_stamp& recorded = record.files[index];
        if (index >= current.size()) {
            reason = file_name(recorded.path) + " is gone";
            return false;
        }

        const file_stamp& now = current[index];
        bool is_same = recorded.digest.empty() ? now.size == recorded.size && now.modified_ns == recorded.modified_ns
                                               : now.digest == recorded.digest;
        if (!is_same) {
            reason = file_name(recorded.path) + " changed";
            return false;
        }
    }

    return true;
}

bool step_is_current(const build_step& step, const std::string& build_directory, std::string& reason,
                     std::string& error) {
    reason.clear();
    std::string path = step_record_path(step, build_directory);
    if (path.empty() || !is_file(path)) {
        return false;
    }

    std::string text;
    if (!read_file(path, text, error)) {
        return false;
    }

    step_record record;
    if (!parse_record(text, record)) {
        return false;
    }

    if (record.commands != step.commands) {
        reason = std::string(COMMAND_CHANGED_REASON);
        return false;
    }

    // Stamping stops at a missing file, which is then named only when no file before it differs, so a compile that
    // failed and deleted its object names its edited source
    std::vector<file_stamp> current;
    for (const file_stamp& recorded : record.files) {
        file_stamp now;
        if (!stamp_file(recorded.path, false, now)) {
            break;
        }

        if (!recorded.digest.empty()) {
            bool is_untouched = now.size == recorded.size && now.modified_ns == recorded.modified_ns;
            if (is_untouched) {
                now.digest = recorded.digest;
            } else if (!stamp_file(recorded.path, true, now)) {
                break;
            }
        }

        current.push_back(now);
    }

    return record_matches(record, step, current, reason);
}

bool record_finished_step(const build_step& step, const std::string& build_directory, int64_t started_ns,
                          std::string& error) {
    std::string path = step_record_path(step, build_directory);
    if (path.empty()) {
        return true;
    }

    step_record record;
    record.commands = step.commands;
    if (step.kind == step_kind::compile) {
        std::string depfile = compile_depfile(step);
        if (!is_file(depfile) || !is_file(step.output)) {
            return true;
        }

        std::vector<std::string> depends;
        if (!read_depfile(depfile, depends, error)) {
            return false;
        }

        for (const std::string& depend : depends) {
            file_stamp stamp;
            bool is_settled = stamp_file(depend, false, stamp) && stamp.modified_ns < started_ns;
            if (!is_settled) {
                return true;
            }

            if (!has_path(record.files, depend)) {
                record.files.push_back(stamp);
            }
        }

        add_stamp(record.files, find_program(step.commands[0][0]), false);
        add_stamp(record.files, step.output, false);
    } else if (!step.output.empty() && !is_file(step.output)) {
        return true;
    } else {
        for (const std::vector<std::string>& command : step.commands) {
            for (const std::string& word : command) {
                if (word.starts_with('/')) {
                    add_stamp(record.files, word, false);
                }
            }
        }

        add_stamp(record.files, step.output, false);
    }

    for (const std::vector<std::string>& command : step.commands) {
        for (const std::string& word : command) {
            if (is_response_word(word)) {
                add_stamp(record.files, word.substr(1), true);
            }
        }
    }

    bool changed = false;
    return make_directories(parent_directory(path), error) &&
           write_file_if_changed(path, format_record(record), changed, error);
}
