#ifndef MCUFW_INSTALLER_HPP
#define MCUFW_INSTALLER_HPP

// Installs and removes cores, the data tools their boards need, and libraries. An installed item's archive streams
// through SHA-256 and the zip reader into a staged folder, which replaces the installed one only after its checks pass.

#include "fqbn.hpp"
#include "indexes.hpp"
#include "layout.hpp"
#include "libraries.hpp"
#include "report.hpp"
#include "requirements.hpp"
#include "zip.hpp"

#include <string>
#include <string_view>
#include <vector>

enum class install_kind { core, board, library };

// A library release and where it lands
struct library_install {
    release_name release;
    std::string  directory;  // `<libraries>/<library_folder_name>`
    std::string  replaced;   // the installed copy's folder, which the install deletes, empty when there is none
};

struct install_target {
    install_kind  kind;
    board_request board;    // a core's packager and architecture, and for a board its ID and options
    std::string   library;  // a library's name as the index spells it
    std::string   version;  // after `@`, empty for the newest release the overrides or constraints allow
};

// `esp32:esp32` names a core, `esp32:esp32:esp32da` a board and `FastLED` a library, any word without a colon, each
// with an optional `@version`
bool parse_install_target(std::string_view text, install_target& target, std::string& error);

// Gives the archive's SHA-256 as lowercase hex. Fails, before any download, on an archive with no URL, not a zip by its
// file name, without a SHA-256 checksum in hex of either case, or without a stated size.
bool check_archive(const archive_download& archive, std::string& sha256, std::string& error);

// Fails once more bytes arrived than the index states, and when the download is complete with fewer. Unpacking runs
// before the checksum, so this bounds what an archive can write.
bool check_received_size(const archive_download& archive, uint64_t received, bool is_complete, std::string& error);

// The most bytes an archive may unpack to, 16 times the size its index states
uint64_t unpacked_bytes_max(const archive_download& archive);

// The path an archive entry takes in its staged folder, its top folder dropped. Fails on a path past 4096 bytes or with
// a part past 255, an unsafe path, a file outside any top folder, or a top folder other than `top_folder`'s.
bool staged_entry_path(std::string_view entry_path, std::string& top_folder, std::string& staged_path,
                       std::string& error);

// The staged paths to mark executable, files with an execute bit in their Unix mode. Fails on a symbolic link, which
// mcufw never installs, or on a path staged_entry_path refuses.
bool check_member_modes(const std::vector<zip_member>& members, std::vector<std::string>& executables,
                        std::string& error);

// The release to install: `version` when given, else the newest, among `overridden` when it holds any. Fails when no
// release qualifies, naming the overridden versions when there are any.
bool choose_core_release(const std::vector<core_release>& releases, const std::string& version,
                         const std::vector<std::string>& overridden, const core_release*& chosen, std::string& error);

// The missing tools a board's requirements name that the index lists as data, to install. One the index lists per host
// is a program only a Stellux package provides. Fails on a tool the core does not depend on or the index does not list.
bool plan_data_tools(const board_requirements& requirements, const std::vector<tool_release>& tools,
                     std::vector<const tool_release*>& data_tools, std::string& error);

// The library releases to install, dependencies first: the targets resolved together as arduino-cli 1.6.0 resolves one,
// installed libraries no target names kept. Leaves out releases already installed, and fails naming the fix it knows.
bool resolve_libraries(const std::vector<library_release>& releases, const std::vector<install_target>& targets,
                       const std::vector<library>& installed, std::vector<release_name>& chosen, std::string& error);

// Whether a target is a library, the only kind that reads the library index, so the only one a failed fetch of it fails
bool needs_library_index(const std::vector<install_target>& targets);

// Where each chosen release installs, as arduino-cli 1.6.0 decides: `<libraries>/<library_folder_name>`, replacing the
// installed copy of its name. Fails before any download on a name installed twice or a place something else holds.
bool plan_library_installs(const std::vector<release_name>& chosen, const std::vector<library>& installed,
                           const std::string& libraries_folder, const std::vector<std::string>& folder_entries,
                           std::vector<library_install>& installs, std::string& error);

// The installed data tools no core's record names, which every install or removal deletes
std::vector<tool_dependency> unused_data_tools(const std::vector<installed_core>& cores,
                                               const std::vector<tool_dependency>& installed);

// Fetches both indexes, installs the core and board targets in order, then the library targets with their dependencies.
// Stops at the first failure, keeping what it installed before, and only warns of a library index it lacks no use for.
bool install_targets(const directory_layout& layout, const std::vector<install_target>& targets, reporter& report,
                     failure& failed);

// Removes each core or library, then every data tool no installed core's record names, without the network
bool remove_targets(const directory_layout& layout, const std::vector<install_target>& targets, reporter& report,
                    failure& failed);

#endif // MCUFW_INSTALLER_HPP
