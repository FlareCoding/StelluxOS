#include "installer.hpp"

#include "versions.hpp"

#include <cctype>

constexpr std::string_view ZIP_EXTENSION = ".zip";
constexpr std::string_view SHA256_PREFIX = "SHA-256:";
constexpr size_t SHA256_HEX_DIGITS = 64;

// Three times the largest unpacked to packed ratio among Arduino-ESP32 3.3.12's archives (3.8) and common libraries
// (TFT_eSPI's 5.4), since staging is RAM and an archive unpacks before its SHA-256 is checked
constexpr uint64_t UNPACKED_RATIO_MAX = 16;

// Stellux's PATH_MAX and NAME_MAX, kernel/fs/fstypes.h, so no longer path could be created anyway
constexpr size_t ENTRY_PATH_MAX = 4096;
constexpr size_t ENTRY_NAME_MAX = 255;

constexpr uint32_t FILE_TYPE_BITS = 0170000;
constexpr uint32_t SYMBOLIC_LINK_TYPE = 0120000;
constexpr uint32_t EXECUTABLE_BITS = 0111;

static std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    while (true) {
        size_t end = text.find(separator);
        parts.push_back(text.substr(0, end));
        if (end == std::string_view::npos) {
            return parts;
        }

        text.remove_prefix(end + 1);
    }
}

bool parse_install_target(std::string_view text, install_target& target, std::string& error) {
    target = {};
    size_t at = text.rfind('@');
    std::string_view name = text.substr(0, at);
    if (at != std::string_view::npos) {
        target.version = text.substr(at + 1);
        if (target.version.empty()) {
            error = std::string(text) + " names no version after its @";
            return false;
        }
    }

    std::vector<std::string_view> parts = split(name, ':');
    if (parts.size() == 2 && !parts[0].empty() && !parts[1].empty()) {
        target.kind = install_kind::core;
        target.board.packager = parts[0];
        target.board.architecture = parts[1];
        return true;
    }

    if (parts.size() < 3) {
        error = std::string(name) + " names no core or board, so name a core as packager:architecture or a board "
                                    "by its FQBN";
        return false;
    }

    target.kind = install_kind::board;

    return parse_board_request(name, target.board, error);
}

// The hex digits of `text` in lowercase, or empty when it holds anything else
static std::string lowercase_hex(std::string_view text) {
    std::string hex;
    for (char c : text) {
        if (!isxdigit(static_cast<unsigned char>(c))) {
            return {};
        }

        hex += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }

    return hex;
}

bool check_archive(const archive_download& archive, std::string& sha256, std::string& error) {
    if (archive.url.empty()) {
        error = "the index gives no URL for " + archive.file_name;
        return false;
    }

    if (!archive.file_name.ends_with(ZIP_EXTENSION)) {
        error = archive.file_name + " is no zip, which is the only archive mcufw unpacks";
        return false;
    }

    std::string_view checksum = archive.checksum;
    bool is_sha256 = checksum.starts_with(SHA256_PREFIX) && checksum.size() == SHA256_PREFIX.size() + SHA256_HEX_DIGITS;
    sha256 = is_sha256 ? lowercase_hex(checksum.substr(SHA256_PREFIX.size())) : std::string();
    if (sha256.empty()) {
        error = archive.file_name + " has no SHA-256 checksum in the index, so mcufw cannot verify it";
        return false;
    }

    if (archive.size == 0) {
        error = "the index states no size for " + archive.file_name + ", which bounds what mcufw unpacks";
        return false;
    }

    return true;
}

bool check_received_size(const archive_download& archive, uint64_t received, bool is_complete, std::string& error) {
    if (received > archive.size) {
        error = archive.url + " holds more than the " + std::to_string(archive.size) + " bytes its index states";
        return false;
    }

    if (is_complete && received != archive.size) {
        error = archive.url + " holds " + std::to_string(received) + " of the " + std::to_string(archive.size) +
                " bytes its index states";
        return false;
    }

    return true;
}

uint64_t unpacked_bytes_max(const archive_download& archive) {
    return archive.size > UINT64_MAX / UNPACKED_RATIO_MAX ? UINT64_MAX : archive.size * UNPACKED_RATIO_MAX;
}

bool staged_entry_path(std::string_view entry_path, std::string& top_folder, std::string& staged_path,
                       std::string& error) {
    if (entry_path.size() > ENTRY_PATH_MAX) {
        error = "the archive names a path longer than " + std::to_string(ENTRY_PATH_MAX) + " bytes";
        return false;
    }

    std::string path(entry_path);
    bool is_unsafe = entry_path.empty() || entry_path.front() == '/' ||
                     entry_path.find_first_of(std::string_view("\\\0", 2)) != std::string_view::npos;
    std::vector<std::string_view> parts = split(entry_path, '/');
    if (!is_unsafe && parts.back().empty()) {
        parts.pop_back();
    }

    bool is_too_long = false;
    for (std::string_view part : parts) {
        is_unsafe = is_unsafe || part.empty() || part == "." || part == "..";
        is_too_long = is_too_long || part.size() > ENTRY_NAME_MAX;
    }

    if (is_unsafe) {
        error = "the archive names an unsafe path, " + path;
        return false;
    }

    if (is_too_long) {
        error = "the archive names a path with a part longer than " + std::to_string(ENTRY_NAME_MAX) + " bytes";
        return false;
    }

    bool is_directory = entry_path.back() == '/';
    if (parts.size() == 1 && !is_directory) {
        error = path + " lies outside the archive's top folder";
        return false;
    }

    if (top_folder.empty()) {
        top_folder = parts[0];
    } else if (parts[0] != top_folder) {
        error = path + " lies outside the archive's top folder, " + top_folder;
        return false;
    }

    staged_path.clear();
    for (size_t i = 1; i < parts.size(); i++) {
        staged_path += (i == 1 ? "" : "/") + std::string(parts[i]);
    }

    return true;
}

bool check_member_modes(const std::vector<zip_member>& members, std::vector<std::string>& executables,
                        std::string& error) {
    executables.clear();
    std::string top_folder;
    for (const zip_member& member : members) {
        std::string staged;
        if (!staged_entry_path(member.path, top_folder, staged, error)) {
            return false;
        }

        if ((member.unix_mode & FILE_TYPE_BITS) == SYMBOLIC_LINK_TYPE) {
            error = "the archive holds a symbolic link, " + member.path + ", which mcufw does not install";
            return false;
        }

        if (member.path.back() != '/' && (member.unix_mode & EXECUTABLE_BITS) != 0) {
            executables.push_back(staged);
        }
    }

    return true;
}

static std::string listed_versions(const std::vector<std::string>& versions) {
    std::string text;
    for (const std::string& version : versions) {
        text += (text.empty() ? "" : ", ") + version;
    }

    return text;
}

static bool is_allowed(const std::vector<std::string>& overridden, const std::string& version) {
    for (const std::string& allowed : overridden) {
        if (allowed == version) {
            return true;
        }
    }

    return overridden.empty();
}

bool choose_core_release(const std::vector<core_release>& releases, const std::string& version,
                         const std::vector<std::string>& overridden, const core_release*& chosen, std::string& error) {
    chosen = nullptr;
    for (const core_release& release : releases) {
        bool is_candidate = is_allowed(overridden, release.version) && (version.empty() || release.version == version);
        if (is_candidate && (!chosen || compare_versions(release.version, chosen->version) > 0)) {
            chosen = &release;
        }
    }

    if (chosen) {
        return true;
    }

    if (releases.empty()) {
        error = "the package index lists no such core";
        return false;
    }

    std::string core = releases[0].packager + ":" + releases[0].architecture;
    if (overridden.empty()) {
        error = "the package index lists no " + core + " " + version;
        return false;
    }

    std::string versions = overridden.size() == 1 ? "the version" : "the versions";
    error = "mcufw installs " + core + " only at " + listed_versions(overridden) + ", " + versions +
            " Stellux has overrides for, and the index lists " +
            (version.empty() ? std::string("none of them") : "no " + version + " among them");

    return false;
}

static const tool_release* find_tool(const std::vector<tool_release>& tools, const tool_dependency& wanted) {
    for (const tool_release& tool : tools) {
        if (tool.packager == wanted.packager && tool.name == wanted.name && tool.version == wanted.version) {
            return &tool;
        }
    }

    return nullptr;
}

bool plan_data_tools(const board_requirements& requirements, const std::vector<tool_release>& tools,
                     std::vector<const tool_release*>& data_tools, std::string& error) {
    data_tools.clear();
    for (const tool_dependency& missing : requirements.missing_tools) {
        if (missing.packager.empty()) {
            error = "the board's recipes name tool " + missing.name + ", which the core does not depend on";
            return false;
        }

        const tool_release* release = find_tool(tools, missing);
        if (!release) {
            error = "the board's recipes name tool " + missing.packager + ":" + missing.name + " " + missing.version +
                    ", which the package index does not list";
            return false;
        }

        if (release->is_data) {
            data_tools.push_back(release);
        }
    }

    return true;
}

static bool is_named(const std::vector<installed_core>& cores, const tool_dependency& tool) {
    for (const installed_core& core : cores) {
        for (const tool_dependency& named : core.tools) {
            if (named.packager == tool.packager && named.name == tool.name && named.version == tool.version) {
                return true;
            }
        }
    }

    return false;
}

std::vector<tool_dependency> unused_data_tools(const std::vector<installed_core>& cores,
                                               const std::vector<tool_dependency>& installed) {
    std::vector<tool_dependency> unused;
    for (const tool_dependency& tool : installed) {
        if (!is_named(cores, tool)) {
            unused.push_back(tool);
        }
    }

    return unused;
}
