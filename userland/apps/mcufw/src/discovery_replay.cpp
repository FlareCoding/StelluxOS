#include "discovery_replay.hpp"

#include "digest.hpp"
#include "files.hpp"

#include <string_view>

constexpr size_t           COMMAND_DIGEST_DIGITS = 16;
constexpr std::string_view PROBE_PREFIX          = "probe ";
constexpr std::string_view DEPFILE_PREFIX        = "depfile ";
constexpr std::string_view MISSING_PREFIX        = "missing ";
constexpr std::string_view COMPLETE_LINE         = "complete";
constexpr std::string_view RESPONSE_PREFIX       = "response ";
constexpr std::string_view COMPILER_PREFIX       = "compiler ";

static std::string digest_words(const std::vector<std::string>& words) {
    sha256_digest digest;
    for (const std::string& word : words) {
        digest.update(word);
        digest.update(std::string_view("\0", 1));
    }

    return digest.hex().substr(0, COMMAND_DIGEST_DIGITS);
}

static std::vector<std::string> response_files_of(const std::vector<std::string>& words) {
    std::vector<std::string> files;
    for (const std::string& word : words) {
        if (is_response_word(word)) {
            files.push_back(word.substr(1));
        }
    }

    return files;
}

// arduino-cli's check of a file's `.libsdetect.d`, which its last probe wrote for every probe of the file: no older
// than the source and each file it lists, and naming the source first
static bool depfile_vouches(const std::string& source, const std::string& depfile) {
    uint64_t size = 0;
    int64_t source_time = 0;
    int64_t depfile_time = 0;
    if (!read_file_size_and_time(source, size, source_time) || !read_file_size_and_time(depfile, size, depfile_time) ||
        source_time > depfile_time) {
        return false;
    }

    std::vector<std::string> files;
    std::string error;
    if (!read_depfile(depfile, files, error)) {
        return false;
    }

    if (files.empty()) {
        return true;
    }

    if (files[0] != source) {
        return false;
    }

    for (size_t index = 1; index < files.size(); index++) {
        int64_t time = 0;
        if (!read_file_size_and_time(files[index], size, time) || time > depfile_time) {
            return false;
        }
    }

    return true;
}

// Whether each response file holds what it held, `count` of them as the probe's words name
static bool has_same_content(const std::vector<file_stamp>& recorded, size_t count) {
    if (recorded.size() != count) {
        return false;
    }

    for (const file_stamp& stamp : recorded) {
        file_stamp now;
        if (!stamp_file(stamp.path, true, now) || now.digest != stamp.digest) {
            return false;
        }
    }

    return true;
}

static bool has_same_compiler(const file_stamp& recorded) {
    if (recorded.path.empty()) {
        return true;
    }

    file_stamp now;
    return stamp_file(recorded.path, false, now) && now.size == recorded.size &&
           now.modified_ns == recorded.modified_ns;
}

static bool parse_record(std::string_view text, std::vector<recorded_probe>& recorded) {
    while (!text.empty()) {
        size_t end = text.find('\n');
        if (end == std::string_view::npos) {
            return false;
        }

        std::string_view line = text.substr(0, end);
        text.remove_prefix(end + 1);
        if (line.starts_with(PROBE_PREFIX)) {
            std::string_view fields = line.substr(PROBE_PREFIX.size());
            size_t space = fields.find(' ');
            if (space != COMMAND_DIGEST_DIGITS || space + 1 >= fields.size()) {
                return false;
            }

            recorded_probe next;
            next.command_digest = std::string(fields.substr(0, space));
            next.source = std::string(fields.substr(space + 1));
            next.result = { probe_status::failed, {}, {} };
            recorded.push_back(next);
            continue;
        }

        if (recorded.empty()) {
            return false;
        }

        recorded_probe& current = recorded.back();
        file_stamp stamp;
        if (line.starts_with(DEPFILE_PREFIX)) {
            current.depfile = std::string(line.substr(DEPFILE_PREFIX.size()));
        } else if (line == COMPLETE_LINE) {
            current.result.status = probe_status::complete;
        } else if (line.starts_with(MISSING_PREFIX)) {
            current.result = { probe_status::missing, std::string(line.substr(MISSING_PREFIX.size())), {} };
        } else if (line.starts_with(RESPONSE_PREFIX) && parse_file_stamp(line.substr(RESPONSE_PREFIX.size()), stamp)) {
            current.response_files.push_back(stamp);
        } else if (line.starts_with(COMPILER_PREFIX) && parse_file_stamp(line.substr(COMPILER_PREFIX.size()), stamp)) {
            current.compiler = stamp;
        } else {
            return false;
        }
    }

    for (const recorded_probe& probe : recorded) {
        if (probe.depfile.empty() || probe.result.status == probe_status::failed) {
            return false;
        }
    }

    return true;
}

bool discovery_replay::load(const std::string& record_path, std::string& error) {
    m_recorded.clear();
    m_next = 0;
    m_vouched_depfile.clear();
    if (!is_file(record_path)) {
        return true;
    }

    std::string text;
    if (!read_file(record_path, text, error)) {
        return false;
    }

    if (!parse_record(text, m_recorded)) {
        m_recorded.clear();
    }

    return true;
}

bool discovery_replay::answer(const probe& turn, probe_result& result) {
    if (m_next >= m_recorded.size()) {
        return false;
    }

    const recorded_probe& recorded = m_recorded[m_next];
    bool is_answered = !turn.words.empty() && recorded.command_digest == digest_words(turn.words) &&
                       recorded.source == turn.source && recorded.depfile == turn.depfile &&
                       (turn.depfile == m_vouched_depfile || depfile_vouches(turn.source, turn.depfile)) &&
                       has_same_content(recorded.response_files, response_files_of(turn.words).size()) &&
                       has_same_compiler(recorded.compiler);
    if (!is_answered) {
        m_recorded.resize(m_next);
        return false;
    }

    m_vouched_depfile = turn.depfile;
    result = recorded.result;
    m_next++;
    return true;
}

bool save_discovery_record(const std::string& record_path, const std::vector<reached_probe>& reached,
                           std::string& error) {
    std::string text;
    for (const reached_probe& entry : reached) {
        const probe& turn = entry.turn;
        bool is_missing = entry.result.status == probe_status::missing;
        std::string result =
            is_missing ? std::string(MISSING_PREFIX) + entry.result.header : std::string(COMPLETE_LINE);
        text += std::string(PROBE_PREFIX) + digest_words(turn.words) + " " + turn.source + "\n";
        text += std::string(DEPFILE_PREFIX) + turn.depfile + "\n" + result + "\n";
        for (const std::string& path : response_files_of(turn.words)) {
            file_stamp stamp;
            if (stamp_file(path, true, stamp)) {
                text += std::string(RESPONSE_PREFIX) + format_file_stamp(stamp) + "\n";
            }
        }

        file_stamp compiler;
        if (!turn.words.empty() && turn.words[0].starts_with('/') && stamp_file(turn.words[0], false, compiler)) {
            text += std::string(COMPILER_PREFIX) + format_file_stamp(compiler) + "\n";
        }
    }

    bool changed = false;
    return make_directories(parent_directory(record_path), error) &&
           write_file_if_changed(record_path, text, changed, error);
}
