#ifndef MCUFW_LIBRARIES_HPP
#define MCUFW_LIBRARIES_HPP

// Arduino libraries as arduino-cli 1.6.0 loads them: those the board's core bundles in its libraries folder, and those
// in mcufw's libraries folder, installed from Arduino's library index or placed by a person

#include "layout.hpp"

#include <string>
#include <string_view>
#include <vector>

enum class library_location { core, installed };

struct library {
    std::string      name;         // library.properties' `name`, `-` when it has none, or a legacy library's folder
    std::string      version;      // empty when library.properties states none
    std::string      directory;
    std::string      folder_name;  // the folder it was found in, such as `Network` for the library `Networking`
    library_location location;
    bool             is_legacy;    // no library.properties, so it counts only with a header at its top
};

// A library's folder as arduino-cli names it: letters and digits stay, so do `-` and `.` after the first character,
// every other character becomes one `_`, and the name stops at 64 bytes
std::string library_folder_name(std::string_view name);

// The libraries in `core_libraries`, empty for none, then those in the libraries folder, skipping hidden folders. Fails
// on a library.properties that cannot be read, naming it.
bool find_libraries(const std::string& core_libraries, const directory_layout& layout, std::vector<library>& libraries,
                    std::string& error);

#endif // MCUFW_LIBRARIES_HPP
