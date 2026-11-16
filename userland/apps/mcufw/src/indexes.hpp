#ifndef MCUFW_INDEXES_HPP
#define MCUFW_INDEXES_HPP

// Arduino's package and library indexes, the files the Arduino IDE installs cores, tools and libraries from. Each
// install run fetches them gzipped into the state folder, then streams them through inflate and the JSON reader.

#include "cores.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

constexpr const char* PACKAGE_INDEX_URL  = "https://downloads.arduino.cc/packages/package_index.json.gz";
constexpr const char* PACKAGE_INDEX_FILE = "package_index.json.gz";
constexpr const char* LIBRARY_INDEX_URL  = "https://downloads.arduino.cc/libraries/library_index.json.gz";
constexpr const char* LIBRARY_INDEX_FILE = "library_index.json.gz";

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

struct library_dependency {
    std::string name;
    std::string constraint;  // such as `>=1.3.0` or `>=2.3.4 && <3.0.0`, empty for any release
};

struct library_release {
    std::string                     name;
    std::string                     version;
    std::vector<std::string>        architectures;
    std::vector<library_dependency> dependencies;
    std::vector<std::string>        provided_headers;  // the index's `providesIncludes`
};

struct release_name {
    std::string name;
    std::string version;
};

// Reads every release of core `packager:architecture` and every tool the index lists. Fails on an index that inflates
// past 256 MiB or lists more than 256 packages or 4096 tools, ten times the real index at least.
bool read_package_index(const std::string& path, const std::string& packager, const std::string& architecture,
                        std::vector<core_release>& cores, std::vector<tool_release>& tools, std::string& error);

// The library index is read twice, first for each release without its download, then for the chosen releases'
// downloads, so only those are kept. Returning false from `take` stops this first pass.
bool read_library_releases(const std::string& path,
                           const std::function<bool(library_release& release, std::string& error)>& take,
                           std::string& error);

// The download of each release `wanted` names, in its order. Fails naming a release the index does not list.
bool read_library_downloads(const std::string& path, const std::vector<release_name>& wanted,
                            std::vector<archive_download>& downloads, std::string& error);

#endif // MCUFW_INDEXES_HPP
