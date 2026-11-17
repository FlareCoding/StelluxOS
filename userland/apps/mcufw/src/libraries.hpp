#ifndef MCUFW_LIBRARIES_HPP
#define MCUFW_LIBRARIES_HPP

// Arduino libraries as arduino-cli 1.6.0 loads them: those the board's core bundles in its libraries folder, and those
// in mcufw's libraries folder, installed from Arduino's library index or placed by a person

#include "layout.hpp"

#include <string>
#include <string_view>
#include <vector>

enum class library_location { core, installed };

enum class library_layout { flat, recursive };

struct library {
    std::string              name;               // library.properties' `name`, `-` without one, a legacy one's folder
    std::string              version;            // empty when library.properties states none
    std::string              directory;
    std::string              folder_name;        // its folder's name, as `Network` for library `Networking`
    library_location         location;
    bool                     is_legacy;          // no library.properties, so it counts only with a header at its top
    library_layout           layout;
    std::vector<std::string> architectures;      // `*` when library.properties names none
    std::vector<std::string> headers;            // the .h, .hpp and .hh files directly in its source directory
    std::string              utility_directory;  // a flat library's utility/ folder, else empty
    bool                     dot_a_linkage;      // its objects link as one archive, which only a src/ library does
    std::string              refusal;            // why discovery may not add it, empty when nothing is wrong
};

// A library's folder as arduino-cli names it: letters and digits stay, so do `-` and `.` after the first character,
// every other character becomes one `_`, and the name stops at 64 bytes
std::string library_folder_name(std::string_view name);

// The libraries in `core_libraries`, empty for none, then those in the libraries folder, skipping hidden folders and a
// library that cannot load, as arduino-cli skips them
bool find_libraries(const std::string& core_libraries, const directory_layout& layout, std::vector<library>& libraries,
                    std::string& error);

// The files the library compiles: src/ recursively, or a flat library's top folder and then its utility/ folder
bool library_sources(const library& library, std::vector<std::string>& sources, std::string& error);

// Its source directory, and for its own files a flat library's utility/ folder too
std::vector<std::string> library_include_directories(const library& library, bool for_its_own_files);

// The folder holding a library's sources and headers: src/ for a recursive library, else its own folder
std::string library_source_directory(const library& library);

// The build tree's folder of library builds, and one library's folder in it, holding its objects and its depfiles
std::string library_builds_directory(const std::string& build_path);
std::string library_build_directory(const std::string& build_path, const library& library);

// The libraries arduino-cli warns about: those listing none of `architectures` and not `*`
std::vector<const library*> incompatible_libraries(const std::vector<const library*>& added,
                                                   const std::vector<std::string>& architectures);

// The board's architecture, then those `architecture.override_check` adds, split on commas as arduino-cli splits them
std::vector<std::string> board_architectures(const std::string& architecture, const std::string& override_check);

// arduino-cli's ranking of a library for `header` on `architecture`: listing the architecture, then `*`, then any
// other, then how closely its name and folder name match the header, then an installed library over the core's copy
int library_priority(const std::string& name, const std::string& folder_name,
                     const std::vector<std::string>& architectures, library_location location,
                     const std::string& header, const std::string& architecture);

// The libraries whose source directory holds `header`
std::vector<const library*> libraries_providing(const std::vector<library>& libraries, const std::string& header);

// The one of `candidates` that ranking picks, the first name in alphabetical order among equal ranks
const library* pick_library(const std::vector<const library*>& candidates, const std::string& header,
                            const std::string& architecture);

#endif // MCUFW_LIBRARIES_HPP
