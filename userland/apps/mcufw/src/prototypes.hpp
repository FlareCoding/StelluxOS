#ifndef MCUFW_PROTOTYPES_HPP
#define MCUFW_PROTOTYPES_HPP

// The prototypes arduino-cli adds to a sketch's merged C++, so a function can be called above its definition. Stellux
// has no ctags, so mcufw's own scanner reads the preprocessed sketch as ctags would and applies arduino-cli's rules.

#include "sketch.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct function_prototype {
    std::string declaration;  // such as `void setup();`
    std::string modifiers;    // `static`, then ` extern "C"` with its space, as arduino-cli adds them
    std::string file;         // the .ino file holding the definition
    uint32_t    line;         // the definition's line, or the line above its return type when that starts earlier
};

struct prototype_plan {
    std::vector<function_prototype> prototypes;
    uint32_t                        insert_line;  // the line the prototypes go before, 0 for above the main file
};

// A sketch's .ino file as it is on disk, by the path its line markers name
struct ino_text {
    std::string path;
    std::string text;
};

// The lines of `preprocessed` that come from `ino_files`, by the line markers the preprocessor writes, up to a line of
// 1 MiB or more, where arduino-cli's filter stops and sets `line_too_long`
std::string sketch_lines(std::string_view preprocessed, const std::vector<std::string>& ino_files,
                         bool& line_too_long);

// The prototypes for the functions `lines` defines at file scope and the line they go before, with arduino-cli's checks
// of multi-line definitions, templates and `extern "C"` reading `files`, the .ino files as they are on disk
prototype_plan find_prototypes(std::string_view lines, const std::string& main_file,
                               const std::vector<ino_text>& files);

// `merged` with line ends made `\n` and the prototypes before the insertion line as arduino-cli writes them, any holding
// `=` left out. Fails when the line is past the merge, as arduino-cli then writes nothing.
bool insert_prototypes(const merged_sketch& merged, const prototype_plan& plan, std::string& source);

#endif // MCUFW_PROTOTYPES_HPP
