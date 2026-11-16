#ifndef MCUFW_INSTALLER_HPP
#define MCUFW_INSTALLER_HPP

// Installs and removes cores and the data tools their boards need. Each archive streams through SHA-256 and the zip
// reader into a staged folder, which replaces the installed one by rename only after the archive's checks pass.

#include "fqbn.hpp"
#include "indexes.hpp"
#include "layout.hpp"
#include "report.hpp"
#include "requirements.hpp"
#include "zip.hpp"

#include <string>
#include <string_view>
#include <vector>

enum class install_kind { core, board };

struct install_target {
    install_kind  kind;
    board_request board;    // a core's packager and architecture, and for a board its ID and options
    std::string   version;  // after `@`, empty for the newest release the overrides allow
};

// `esp32:esp32` names a core and `esp32:esp32:esp32da` a board, each with an optional `@version`
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

// The installed data tools no core's record names, which every install or removal deletes
std::vector<tool_dependency> unused_data_tools(const std::vector<installed_core>& cores,
                                               const std::vector<tool_dependency>& installed);

// Fetches the package index, then installs each target in order, stopping at the first failure, and keeping the items
// installed before it. A board installs its core and the data tools its recipes name, and reports missing programs.
bool install_targets(const directory_layout& layout, const std::vector<install_target>& targets, reporter& report,
                     failure& failed);

// Removes each core, then every data tool no installed core's record names, without the network
bool remove_targets(const directory_layout& layout, const std::vector<install_target>& targets, reporter& report,
                    failure& failed);

#endif // MCUFW_INSTALLER_HPP
