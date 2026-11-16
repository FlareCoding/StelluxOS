#ifndef MCUFW_INDEXES_HPP
#define MCUFW_INDEXES_HPP

// Arduino's package index, the file the Arduino IDE installs cores and tools from. Each install run fetches it gzipped
// into the state folder, then streams it through inflate and the JSON reader.

#include "cores.hpp"

#include <cstdint>
#include <string>
#include <vector>

constexpr const char* PACKAGE_INDEX_URL  = "https://downloads.arduino.cc/packages/package_index.json.gz";
constexpr const char* PACKAGE_INDEX_FILE = "package_index.json.gz";

struct archive_download {
    std::string url;
    std::string file_name;  // the index's `archiveFileName`, whose extension names the archive's format
    std::string checksum;   // as the index writes it, such as `SHA-256:87401004...`
    uint64_t    size;       // bytes, as the index states, 0 when it states none
};

struct core_release {
    std::string                  packager;
    std::string                  architecture;
    std::string                  version;
    std::string                  name;
    archive_download             archive;
    std::vector<tool_dependency> tools;
};

struct tool_release {
    std::string      packager;
    std::string      name;
    std::string      version;
    bool             is_data;  // `"host": "all"`, or every host's entry, at least two with Linux's, shares a checksum
    archive_download archive;  // set only when `is_data`
};

// Reads every release of core `packager:architecture` and every tool the index lists. Fails on an index that inflates
// past 256 MiB or lists more than 256 packages or 4096 tools, ten times the real index at least.
bool read_package_index(const std::string& path, const std::string& packager, const std::string& architecture,
                        std::vector<core_release>& cores, std::vector<tool_release>& tools, std::string& error);

#endif // MCUFW_INDEXES_HPP
