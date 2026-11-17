#ifndef MCUFW_DISCOVERY_REPLAY_HPP
#define MCUFW_DISCOVERY_REPLAY_HPP

// The probes library discovery reached, kept in turn order as arduino-cli keeps its includes cache, and replayed to a
// later build turn by turn until the first probe the record cannot answer

#include "discovery.hpp"
#include "step_records.hpp"

#include <cstddef>
#include <string>
#include <vector>

constexpr const char* DISCOVERY_RECORD_NAME = "libraries.record";

struct recorded_probe {
    std::string             command_digest;  // the first 16 hex digits of the SHA-256 of the probe's words
    std::string             source;
    std::string             depfile;
    probe_result            result;          // complete or missing
    std::vector<file_stamp> response_files;  // each `@file` of the words, by content
    file_stamp              compiler;        // its path empty when the probe's program is no file path
};

class discovery_replay {
public:
    // Reads the record at `record_path`, holding no probe when there is none or it cannot be parsed
    bool load(const std::string& record_path, std::string& error);

    // The recorded result of `turn` when it is the record's next probe, with the same words, response file contents and
    // compiler, and a `.libsdetect.d` no older than its source or any file it lists. A miss drops the rest.
    bool answer(const probe& turn, probe_result& result);

private:
    std::vector<recorded_probe> m_recorded;
    size_t                      m_next = 0;
    std::string                 m_vouched_depfile;  // checked for the turn before, as a file's probes come in a row
};

// Writes the record of a discovery that finished, from its `reached()`
bool save_discovery_record(const std::string& record_path, const std::vector<reached_probe>& reached,
                           std::string& error);

#endif // MCUFW_DISCOVERY_REPLAY_HPP
