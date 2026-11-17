#ifndef MCUFW_CORE_CACHE_HPP
#define MCUFW_CORE_CACHE_HPP

// Compiled cores, as `<core cache>/<board>-<key>/core.a`, keyed by what decides the core's code and nothing else, so
// sketches for one board share an entry unless their build_opt.h differs or the sketch could change the core

#include "build_plan.hpp"
#include "layout.hpp"

#include <string>
#include <vector>

constexpr const char* CORE_ARCHIVE_NAME = "core.a";
constexpr const char* CORE_HEADERS_NAME = "headers";

// The first 16 hex digits of a SHA-256 over the core's compile, archive and hook words, build tree and sketch paths as
// placeholders, the build tree response files they name by content and the compiler's version. Fails on a missing one.
bool core_cache_key(const std::vector<build_step>& core_steps, const std::string& build_directory,
                    const std::string& sketch_folder, const std::string& compiler_version, std::string& key,
                    std::string& error);

// The entry's folder, which holds the archive as `CORE_ARCHIVE_NAME`
std::string core_cache_entry(const directory_layout& layout, const std::string& board_id, const std::string& key);

// Whether the core's `.d` files name a file inside `sketch_folder`, as ESP32's `-I{build.source.path}` allows, a
// missing one counting when `missing_reads`. A tree's first build has none, so only `sketch_shadows` vets its hit.
bool core_reads_sketch(const std::vector<build_step>& core_compiles, const std::string& sketch_folder,
                       bool missing_reads, bool& reads, std::string& error);

// The names of the headers the core's `.d` files name, kept beside the archive it publishes
bool core_header_names(const std::vector<build_step>& core_compiles, std::vector<std::string>& names,
                       std::string& error);

// Whether `sketch_folder` holds a file named like one of `header_names`, which ESP32's `-I{build.source.path}` could
// find before the header the core read, so such a sketch neither publishes its core nor links a cached one
bool sketch_shadows(const std::string& sketch_folder, const std::vector<std::string>& header_names, bool& shadows,
                    std::string& error);

// Moves the archive the build made into its entry with `header_names` beside it, through a folder renamed into place,
// so a build alongside never reads half an archive, and deletes it instead when such a build placed the entry first
bool publish_core(const std::string& archive, const std::vector<std::string>& header_names, const std::string& entry,
                  std::string& error);

// The header names kept beside an entry's archive, false when it kept none or they cannot be read
bool published_header_names(const std::string& entry, std::vector<std::string>& names);

#endif // MCUFW_CORE_CACHE_HPP
