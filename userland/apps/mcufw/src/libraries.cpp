#include "libraries.hpp"

#include "files.hpp"
#include "properties.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cctype>

constexpr const char* LIBRARY_PROPERTIES = "library.properties";
constexpr const char* UNNAMED_LIBRARY = "-";
constexpr size_t FOLDER_NAME_MAX = 64;
constexpr std::array<std::string_view, 3> HEADER_EXTENSIONS = { ".h", ".hpp", ".hh" };
constexpr const char* SOURCE_FOLDER = "src";
constexpr const char* UTILITY_FOLDER = "utility";
constexpr const char* ARCH_FOLDER = "arch";
constexpr const char* ANY_ARCHITECTURE = "*";
constexpr const char* LIBRARIES_FOLDER = "libraries";

// arduino-cli's ranking bonuses, in `ComputePriority`'s order
constexpr int LISTS_ARCHITECTURE = 1010;
constexpr int ANY_ARCHITECTURE_BONUS = 1000;
constexpr int NAME_AND_FOLDER_MATCH = 700;
constexpr int NAME_OR_FOLDER_MATCH = 600;
constexpr int MAIN_SUFFIX_MATCH = 500;
constexpr int MASTER_SUFFIX_MATCH = 400;
constexpr int PREFIX_MATCH = 300;
constexpr int SUFFIX_MATCH = 200;
constexpr int CONTAINS_MATCH = 100;
constexpr int CORE_LOCATION = 2;
constexpr int INSTALLED_LOCATION = 3;

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

static bool exists(const std::string& path) {
    struct stat status;
    return stat(path.c_str(), &status) == 0;
}

static std::string trimmed(std::string_view text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    size_t last = text.find_last_not_of(" \t\r\n");
    return first == std::string_view::npos ? std::string() : std::string(text.substr(first, last - first + 1));
}

static std::string lowered(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }

    return text;
}

static std::vector<std::string> comma_separated(std::string_view text) {
    std::vector<std::string> items;
    while (true) {
        size_t comma = text.find(',');
        items.push_back(trimmed(text.substr(0, comma)));
        if (comma == std::string_view::npos) {
            return items;
        }

        text.remove_prefix(comma + 1);
    }
}

std::string library_source_directory(const library& library) {
    bool is_recursive = library.layout == library_layout::recursive;
    return is_recursive ? join_path(library.directory, SOURCE_FOLDER) : library.directory;
}

std::string library_builds_directory(const std::string& build_path) {
    return join_path(build_path, LIBRARIES_FOLDER);
}

std::string library_build_directory(const std::string& build_path, const library& library) {
    return join_path(library_builds_directory(build_path), library.folder_name);
}

// The headers arduino-cli's resolver indexes a library by, none when its source directory cannot be read
static std::vector<std::string> source_headers(const std::string& directory) {
    std::vector<std::string> names;
    std::vector<std::string> headers;
    std::string ignored;
    if (!list_directory(directory, names, ignored)) {
        return headers;
    }

    for (const std::string& name : names) {
        for (std::string_view extension : HEADER_EXTENSIONS) {
            if (name.ends_with(extension)) {
                headers.push_back(name);
            }
        }
    }

    return headers;
}

// A library as arduino-cli's loader reads it: its name, layout, architectures, headers and linkage. A folder without
// library.properties is a legacy library, flat and for every architecture, which counts only with a header at its top.
static bool read_library(const std::string& root, const std::string& folder, library_location location,
                         library& found, std::string& error) {
    std::string directory = join_path(root, folder);
    std::string properties_path = join_path(directory, LIBRARY_PROPERTIES);
    bool is_legacy = !is_file(properties_path);
    bool has_header = false;
    property_table properties;
    if (is_legacy && (!holds_header(directory, has_header, error) || !has_header)) {
        return false;
    }

    if (!is_legacy && !load_properties(properties_path, properties, error)) {
        return false;
    }

    std::string name = properties.get("name");
    found = {};
    found.name = is_legacy ? folder : name.empty() ? UNNAMED_LIBRARY : name;
    found.version = properties.get("version");
    found.directory = directory;
    found.folder_name = folder;

    found.location = location;
    found.is_legacy = is_legacy;

    bool is_recursive = !is_legacy && exists(join_path(directory, SOURCE_FOLDER));
    std::string architectures = properties.get("architectures");
    found.layout = is_recursive ? library_layout::recursive : library_layout::flat;
    found.architectures = comma_separated(architectures.empty() ? ANY_ARCHITECTURE : architectures);
    found.headers = source_headers(library_source_directory(found));
    found.dot_a_linkage = is_recursive && trimmed(properties.get("dot_a_linkage")) == "true";

    std::string utility = join_path(directory, UTILITY_FOLDER);
    if (!is_recursive && is_directory(utility)) {
        found.utility_directory = utility;
    }

    std::string precompiled = properties.get("precompiled");
    if (!is_legacy && is_directory(join_path(directory, ARCH_FOLDER))) {
        found.refusal = "keeps an arch folder, which libraries may no longer have";
    } else if (precompiled == "true" || precompiled == "full") {
        found.refusal = "is precompiled, which mcufw does not link";
    }

    return true;
}

// A library that cannot load is left out, as arduino-cli's libraries manager leaves it out
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
        library found;
        std::string unloadable;
        if (!folder.starts_with('.') && read_library(root, folder, location, found, unloadable)) {
            libraries.push_back(std::move(found));
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

bool library_sources(const library& library, std::vector<std::string>& sources, std::string& error) {
    bool is_recursive = library.layout == library_layout::recursive;
    if (!find_source_files(library_source_directory(library), is_recursive, sources, error)) {
        return false;
    }

    std::vector<std::string> utility_sources;
    if (!library.utility_directory.empty() &&
        !find_source_files(library.utility_directory, false, utility_sources, error)) {
        return false;
    }

    sources.insert(sources.end(), utility_sources.begin(), utility_sources.end());
    return true;
}

std::vector<std::string> library_include_directories(const library& library, bool for_its_own_files) {
    std::vector<std::string> directories = { library_source_directory(library) };
    if (for_its_own_files && !library.utility_directory.empty()) {
        directories.push_back(library.utility_directory);
    }

    return directories;
}

// A name as arduino-cli compares it with a header
static std::string simplified(std::string_view name) {
    return lowered(library_folder_name(name));
}

static int architecture_bonus(const std::vector<std::string>& architectures, const std::string& architecture) {
    auto lists = [&](const std::string& wanted) {
        return std::find(architectures.begin(), architectures.end(), wanted) != architectures.end();
    };

    if (lists(architecture)) {
        return LISTS_ARCHITECTURE;
    }

    return lists(ANY_ARCHITECTURE) || architectures.empty() ? ANY_ARCHITECTURE_BONUS : 0;
}

static int name_bonus(const std::string& name, const std::string& folder, const std::string& header) {
    if (name == header && folder == header) {
        return NAME_AND_FOLDER_MATCH;
    }

    if (name == header || folder == header) {
        return NAME_OR_FOLDER_MATCH;
    }

    if (name == header + "-main" || folder == header + "-main") {
        return MAIN_SUFFIX_MATCH;
    }

    if (name == header + "-master" || folder == header + "-master") {
        return MASTER_SUFFIX_MATCH;
    }

    if (name.starts_with(header) || folder.starts_with(header)) {
        return PREFIX_MATCH;
    }

    if (name.ends_with(header) || folder.ends_with(header)) {
        return SUFFIX_MATCH;
    }

    bool contains = name.find(header) != std::string::npos || folder.find(header) != std::string::npos;
    return contains ? CONTAINS_MATCH : 0;
}

int library_priority(const std::string& name, const std::string& folder_name,
                     const std::vector<std::string>& architectures, library_location location,
                     const std::string& header, const std::string& architecture) {
    size_t slash = header.rfind('/');
    size_t dot = header.rfind('.');
    bool has_extension = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    std::string stem = simplified(has_extension ? header.substr(0, dot) : header);

    int priority = architecture_bonus(architectures, architecture);
    priority += name_bonus(simplified(name), simplified(folder_name), stem);
    return priority + (location == library_location::core ? CORE_LOCATION : INSTALLED_LOCATION);
}

std::vector<const library*> libraries_providing(const std::vector<library>& libraries, const std::string& header) {
    std::vector<const library*> providing;
    for (const library& candidate : libraries) {
        if (std::find(candidate.headers.begin(), candidate.headers.end(), header) != candidate.headers.end()) {
            providing.push_back(&candidate);
        }
    }

    return providing;
}

const library* pick_library(const std::vector<const library*>& candidates, const std::string& header,
                            const std::string& architecture) {
    const library* picked = nullptr;
    int picked_priority = 0;
    for (const library* candidate : candidates) {
        int priority = library_priority(candidate->name, candidate->folder_name, candidate->architectures,
                                        candidate->location, header, architecture);
        bool ranks_higher = !picked || priority > picked_priority;
        bool breaks_tie = picked && priority == picked_priority && candidate->name < picked->name;
        if (ranks_higher || breaks_tie) {
            picked = candidate;
            picked_priority = priority;
        }
    }

    return picked;
}

std::vector<const library*> incompatible_libraries(const std::vector<const library*>& added,
                                                   const std::vector<std::string>& architectures) {
    std::vector<const library*> incompatible;
    for (const library* candidate : added) {
        const std::vector<std::string>& listed = candidate->architectures;
        auto lists = [&](const std::string& wanted) {
            return std::find(listed.begin(), listed.end(), wanted) != listed.end();
        };

        bool supported = listed.empty() || lists(ANY_ARCHITECTURE) ||
                         std::any_of(architectures.begin(), architectures.end(), [&](const std::string& architecture) {
                             return architecture == ANY_ARCHITECTURE || lists(architecture);
                         });
        if (!supported) {
            incompatible.push_back(candidate);
        }
    }

    return incompatible;
}

std::vector<std::string> board_architectures(const std::string& architecture, const std::string& override_check) {
    std::vector<std::string> architectures = { architecture };
    for (size_t start = 0; !override_check.empty() && start <= override_check.size();) {
        size_t comma = std::min(override_check.find(',', start), override_check.size());
        architectures.push_back(override_check.substr(start, comma - start));
        start = comma + 1;
    }

    return architectures;
}
