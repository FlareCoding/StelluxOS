#ifndef MCUFW_SKETCH_HPP
#define MCUFW_SKETCH_HPP

// A sketch folder as Arduino defines it: a main `.ino` or `.pde` named after the folder, the files compiled and copied
// with it, and its sketch.yaml, of which mcufw reads only the board and the port, and writes the board

#include <cstdint>
#include <string>
#include <vector>

struct sketch {
    std::string              folder;           // absolute
    std::string              name;             // the folder's name
    std::string              main_file;        // its name, such as `Lamp.ino`
    std::vector<std::string> other_ino_files;  // by name
    std::vector<std::string> source_files;     // .c .cpp .cc .cxx .S: the folder's by name, then src/'s by path
    std::vector<std::string> copied_files;     // the other files arduino-cli copies, headers among them
};

// `path` is the sketch's folder or its main file
bool load_sketch(const std::string& path, sketch& found, std::string& error);

struct merged_sketch {
    std::string text;
};

// The C++ the .ino files compile as, arduino-cli's merge: `#include <Arduino.h>` first unless the main file includes
// it, then the main file and the others, each after a `#line 1` mark naming it, so GCC's messages name .ino lines
bool merge_ino_files(const sketch& sketch, merged_sketch& merged, std::string& error);

// `#line <line> "<path>"` with the path's `\` and `"` escaped, which makes GCC name that file in its messages
std::string line_mark(uint32_t line, const std::string& path);

// The text a build writes for a copied sketch file, as arduino-cli copies it: a leading UTF-8 byte order mark dropped
// and a `#line 1` mark naming the original before it
bool copied_file_text(const sketch& sketch, const std::string& relative, std::string& text, std::string& error);

struct sketch_settings {
    std::string fqbn;  // `default_fqbn`, empty when unset
    std::string port;  // `default_port`, empty when unset
};

bool read_sketch_settings(const sketch& sketch, sketch_settings& settings, std::string& error);

// Sets sketch.yaml's `default_fqbn`, creating the file when missing and leaving every other line as it was
bool save_sketch_fqbn(const sketch& sketch, const std::string& fqbn, std::string& error);

#endif // MCUFW_SKETCH_HPP
