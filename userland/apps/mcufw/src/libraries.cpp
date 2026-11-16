#include "libraries.hpp"

#include "files.hpp"
#include "properties.hpp"

#include <algorithm>
#include <array>

constexpr const char* LIBRARY_PROPERTIES = "library.properties";
constexpr const char* UNNAMED_LIBRARY = "-";
constexpr size_t FOLDER_NAME_MAX = 64;
constexpr std::array<std::string_view, 3> HEADER_EXTENSIONS = { ".h", ".hpp", ".hh" };

// Go's range over a string steps one character at a time and one byte at a time where no character starts
std::string library_folder_name(std::string_view name) {
    std::string folder;
    size_t index = 0;
    while (index < name.size()) {
        char c = name[index];
        bool is_kept = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (index > 0 && (c == '-' || c == '.'));
        folder += is_kept ? c : '_';
        index += std::max<size_t>(utf8_character_size(name, index), 1);
    }

    return folder.substr(0, FOLDER_NAME_MAX);
}

// arduino-cli loads a folder without library.properties as a library only when a header sits at its top
static bool holds_header(const std::string& directory, bool& has_header, std::string& error) {
    std::vector<std::string> names;
    if (!list_directory(directory, names, error)) {
        return false;
    }

    has_header = false;
    for (const std::string& name : names) {
        for (std::string_view extension : HEADER_EXTENSIONS) {
            has_header = has_header || (name.ends_with(extension) && is_file(join_path(directory, name)));
        }
    }

    return true;
}

static bool read_library(const std::string& root, const std::string& folder, library_location location,
                         std::vector<library>& libraries, std::string& error) {
    std::string directory = join_path(root, folder);
    std::string properties_path = join_path(directory, LIBRARY_PROPERTIES);
    if (!is_file(properties_path)) {
        bool has_header = false;
        if (!holds_header(directory, has_header, error)) {
            return false;
        }

        if (has_header) {
            libraries.push_back({ folder, "", directory, folder, location, true });
        }

        return true;
    }

    property_table properties;
    if (!load_properties(properties_path, properties, error)) {
        return false;
    }

    std::string name = properties.get("name").empty() ? UNNAMED_LIBRARY : properties.get("name");
    libraries.push_back({ name, properties.get("version"), directory, folder, location, false });

    return true;
}

static bool read_folder(const std::string& root, library_location location, std::vector<library>& libraries,
                        std::string& error) {
    if (root.empty() || !is_directory(root)) {
        return true;
    }

    std::vector<std::string> folders;
    if (!list_subdirectories(root, folders, error)) {
        return false;
    }

    for (const std::string& folder : folders) {
        if (!folder.starts_with('.') && !read_library(root, folder, location, libraries, error)) {
            return false;
        }
    }

    return true;
}

bool find_libraries(const std::string& core_libraries, const directory_layout& layout, std::vector<library>& libraries,
                    std::string& error) {
    libraries.clear();

    return read_folder(core_libraries, library_location::core, libraries, error) &&
           read_folder(layout.libraries, library_location::installed, libraries, error);
}
