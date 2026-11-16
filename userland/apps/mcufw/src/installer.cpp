#include "installer.hpp"

#include "boards.hpp"
#include "cores.hpp"
#include "download.hpp"
#include "file_lock.hpp"
#include "files.hpp"
#include "tools.hpp"
#include "zip.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>

// An archive unpacking into a staged folder as it downloads
struct staged_unpack {
    std::string directory;          // where entries land, their top folder dropped
    std::string top_folder;         // the first entry's, which every entry must share
    int         fd = -1;            // the file being written
    std::string path;               // its path
    bool        io_failed = false;  // a write failed, rather than the archive's checks
};

constexpr const char* STAGING_FOLDER = "staging";
constexpr const char* STAGED_TOOLS_FOLDER = "tools";
constexpr const char* DISCARDED_PREFIX = "discarded-";
constexpr const char* DOWNLOAD_SUFFIX = ".download";
constexpr uint32_t NEW_FILE_MODE = 0644;
constexpr mode_t EXECUTABLE_FILE_MODE = 0755;
constexpr uint64_t NS_PER_SECOND = 1'000'000'000;

// A gzipped index is a few MB, so a download past this is no index
constexpr uint64_t INDEX_DOWNLOAD_MAX = 64 << 20;

static uint64_t monotonic_ns() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return static_cast<uint64_t>(now.tv_sec) * NS_PER_SECOND + static_cast<uint64_t>(now.tv_nsec);
}

static std::string core_item(const std::string& packager, const std::string& architecture) {
    return packager + ":" + architecture;
}

static std::string staging_folder(const directory_layout& layout) {
    return join_path(layout.state, STAGING_FOLDER);
}

// A name in the staging folder that nothing holds, where an installed folder moves before it is deleted, so a delete
// cut short leaves nothing that load_installed_cores reads
static bool free_discard_path(const directory_layout& layout, std::string& path, std::string& error) {
    std::string staging = staging_folder(layout);
    if (!make_directories(staging, error)) {
        return false;
    }

    size_t index = 0;
    do {
        path = join_path(staging, DISCARDED_PREFIX + std::to_string(index++));
    } while (is_directory(path) || is_file(path));

    return true;
}

static zip_handlers unpack_handlers(staged_unpack& unpack) {
    zip_handlers handlers;
    handlers.begin_entry = [&unpack](const zip_entry& entry, std::string& error) {
        std::string staged;
        if (!staged_entry_path(entry.path, unpack.top_folder, staged, error)) {
            return false;
        }

        std::string path = join_path(unpack.directory, staged);
        bool made = false;
        if (entry.path.back() == '/') {
            made = make_directories(path, error);
        } else {
            unpack.path = path;
            made = make_directories(parent_directory(path), error) &&
                   create_new_file(path, NEW_FILE_MODE, unpack.fd, error);
        }

        unpack.io_failed = !made;

        return made;
    };

    handlers.entry_bytes = [&unpack](std::string_view bytes, std::string& error) {
        unpack.io_failed = !write_all(unpack.fd, unpack.path, bytes, error);
        return !unpack.io_failed;
    };

    handlers.end_entry = [&unpack](std::string& error) {
        int fd = unpack.fd;
        unpack.fd = -1;
        if (fd >= 0 && close(fd) != 0) {
            error = "cannot write " + unpack.path + ": " + strerror(errno);
            unpack.io_failed = true;
            return false;
        }

        return true;
    };

    return handlers;
}

static bool mark_executables(const std::vector<std::string>& executables, const std::string& directory,
                             failure& failed) {
    for (const std::string& staged : executables) {
        std::string path = join_path(directory, staged);
        if (chmod(path.c_str(), EXECUTABLE_FILE_MODE) != 0) {
            failed = { failure_kind::io, "cannot mark " + path + " executable: " + strerror(errno) };
            return false;
        }
    }

    return true;
}

// Writes entries into `directory` before the SHA-256 is checked, so it must be a staged folder. Bytes past the index's
// stated size never reach the reader, so the index bounds what an archive writes.
static bool unpack_download(const archive_download& archive, const std::string& item, const std::string& version,
                            const std::string& directory, reporter& report, failure& failed) {
    std::string sha256;
    std::string error;
    if (!check_archive(archive, sha256, error)) {
        failed = { failure_kind::verify, error };
        return false;
    }

    if (!make_directories(directory, error)) {
        failed = { failure_kind::io, error };
        return false;
    }

    staged_unpack unpack = { directory, "", -1, "", false };
    zip_reader reader(unpack_handlers(unpack), unpacked_bytes_max(archive));
    uint64_t done = 0;
    auto consume = [&](std::string_view bytes, failure& consume_failed) {
        done += bytes.size();
        std::string consume_error;
        if (!check_received_size(archive, done, false, consume_error) || !reader.feed(bytes, consume_error)) {
            consume_failed = { unpack.io_failed ? failure_kind::io : failure_kind::verify, consume_error };
            return false;
        }

        report.install_progress(item, done, archive.size, monotonic_ns());

        return true;
    };

    report.install_started(item, version, archive.size);
    bool fetched = download(archive.url, sha256, consume, failed);
    if (unpack.fd >= 0) {
        close(unpack.fd);
    }

    if (!fetched) {
        return false;
    }

    std::vector<std::string> executables;
    bool checked = check_received_size(archive, done, true, error) && reader.finish(error) &&
                   check_member_modes(reader.members(), executables, error);
    if (!checked) {
        failed = { failure_kind::verify, error };
        return false;
    }

    return mark_executables(executables, directory, failed);
}

static bool install_core(const directory_layout& layout, const core_release& release, reporter& report,
                         failure& failed) {
    std::string item = core_item(release.packager, release.architecture);
    std::string staged_core = join_path(join_path(staging_folder(layout), release.packager), release.architecture);
    std::string staged = join_path(staged_core, release.version);
    if (!unpack_download(release.archive, item, release.version, staged, report, failed)) {
        return false;
    }

    std::string installed = join_path(join_path(layout.hardware, release.packager), release.architecture);
    std::string discard;
    std::string error;
    bool replaced = write_install_record(staged, release.tools, error) &&
                    free_discard_path(layout, discard, error) &&
                    replace_directory(staged_core, installed, discard, error);
    if (!replaced) {
        failed = { failure_kind::io, error };
        return false;
    }

    report.installed(item, release.version, false);

    return true;
}

static std::string tool_folder(const std::string& root, const tool_release& tool) {
    return join_path(join_path(join_path(root, tool.packager), tool.name), tool.version);
}

static bool install_data_tool(const directory_layout& layout, const tool_release& tool, reporter& report,
                              failure& failed) {
    std::string item = tool.packager + ":" + tool.name;
    std::string staged = tool_folder(join_path(staging_folder(layout), STAGED_TOOLS_FOLDER), tool);
    if (!unpack_download(tool.archive, item, tool.version, staged, report, failed)) {
        return false;
    }

    std::string discard;
    std::string error;
    bool replaced = free_discard_path(layout, discard, error) &&
                    replace_directory(staged, tool_folder(layout.data_tools, tool), discard, error);
    if (!replaced) {
        failed = { failure_kind::io, error };
        return false;
    }

    report.installed(item, tool.version, false);

    return true;
}

static bool remove_unused_data_tools(const directory_layout& layout, reporter& report, failure& failed) {
    std::vector<installed_core> cores;
    std::vector<tool_dependency> installed;
    std::string error;
    if (!load_installed_cores(layout, cores, error) || !list_data_tools(layout, installed, error)) {
        failed = { failure_kind::io, error };
        return false;
    }

    for (const tool_dependency& tool : unused_data_tools(cores, installed)) {
        std::string folder = join_path(join_path(join_path(layout.data_tools, tool.packager), tool.name), tool.version);
        std::string discard;
        if (!free_discard_path(layout, discard, error) || !discard_directory(folder, discard, error)) {
            failed = { failure_kind::io, error };
            return false;
        }

        report.removed(tool.packager + ":" + tool.name, tool.version);
    }

    return true;
}

// The version of core `packager:architecture` installed, empty when none is
static bool installed_version(const directory_layout& layout, const std::string& packager,
                              const std::string& architecture, std::string& version, failure& failed) {
    std::vector<installed_core> cores;
    std::string error;
    if (!load_installed_cores(layout, cores, error)) {
        failed = { failure_kind::io, error };
        return false;
    }

    version.clear();
    for (const installed_core& core : cores) {
        if (core.packager == packager && core.architecture == architecture) {
            version = core.version;
        }
    }

    return true;
}

// Installs the data tools the board's recipes name, then names the programs still missing
static bool install_board_tools(const directory_layout& layout, const board_request& request,
                                const std::vector<tool_release>& tools, reporter& report, failure& failed) {
    std::vector<installed_core> cores;
    selected_board board;
    std::string error;
    if (!load_installed_cores(layout, cores, error)) {
        failed = { failure_kind::io, error };
        return false;
    }

    if (!select_board(cores, request, board, error)) {
        failed = { failure_kind::board, error };
        return false;
    }

    std::vector<const tool_release*> data_tools;
    board_requirements requirements = check_requirements(board, locate_tools(board.core->tools, layout));
    if (!plan_data_tools(requirements, tools, data_tools, error)) {
        failed = { failure_kind::core, error };
        return false;
    }

    for (const tool_release* tool : data_tools) {
        if (!install_data_tool(layout, *tool, report, failed)) {
            return false;
        }
    }

    requirements = check_requirements(board, locate_tools(board.core->tools, layout));
    for (const tool_dependency& tool : requirements.missing_tools) {
        report.program_missing(tool);
    }

    if (!requirements.missing_program.empty()) {
        report.program_missing({ "", requirements.missing_program, "" });
    }

    return true;
}

static bool install_core_or_board(const directory_layout& layout, const std::string& index_path,
                                  const install_target& target, reporter& report, failure& failed) {
    const board_request& request = target.board;
    std::string item = core_item(request.packager, request.architecture);

    std::vector<core_release> releases;
    std::vector<tool_release> tools;
    std::vector<std::string> overridden;
    std::string error;
    if (!read_package_index(index_path, request.packager, request.architecture, releases, tools, error) ||
        !overridden_versions(layout, request.packager, request.architecture, overridden, error)) {
        failed = { failure_kind::io, error };
        return false;
    }

    if (releases.empty()) {
        failed = { failure_kind::core, "the package index lists no core " + item };
        return false;
    }

    const core_release* chosen = nullptr;
    if (!choose_core_release(releases, target.version, overridden, chosen, error)) {
        failed = { failure_kind::core, error };
        return false;
    }

    std::string installed;
    if (!installed_version(layout, request.packager, request.architecture, installed, failed)) {
        return false;
    }

    if (installed == chosen->version) {
        report.installed(item, installed, true);
    } else if (!install_core(layout, *chosen, report, failed)) {
        return false;
    }

    if (!remove_unused_data_tools(layout, report, failed)) {
        failed.reason = item + " " + chosen->version + " is installed, but the data tools no core uses were not " +
                        "removed: " + failed.reason;
        return false;
    }

    return target.kind == install_kind::core || install_board_tools(layout, request, tools, report, failed);
}

// Downloads an index into a temporary file in the state folder and renames it over the last copy only once the
// download succeeds
static bool fetch_package_index(const std::string& path, failure& failed) {
    std::string temporary = path + DOWNLOAD_SUFFIX;
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, NEW_FILE_MODE);
    if (fd < 0) {
        failed = { failure_kind::io, "cannot create " + temporary + ": " + strerror(errno) };
        return false;
    }

    uint64_t received = 0;
    auto consume = [&](std::string_view bytes, failure& consume_failed) {
        received += bytes.size();
        if (received > INDEX_DOWNLOAD_MAX) {
            consume_failed = { failure_kind::verify, std::string(PACKAGE_INDEX_URL) + " holds more than " +
                                                         std::to_string(INDEX_DOWNLOAD_MAX >> 20) + " MiB" };
            return false;
        }

        std::string error;
        if (!write_all(fd, temporary, bytes, error)) {
            consume_failed = { failure_kind::io, error };
            return false;
        }

        return true;
    };

    bool fetched = download(PACKAGE_INDEX_URL, "", consume, failed);
    if (close(fd) != 0 && fetched) {
        failed = { failure_kind::io, "cannot write " + temporary + ": " + strerror(errno) };
        fetched = false;
    }

    if (fetched && rename(temporary.c_str(), path.c_str()) != 0) {
        failed = { failure_kind::io, "cannot replace " + path + ": " + strerror(errno) };
        fetched = false;
    }

    if (!fetched) {
        unlink(temporary.c_str());
    }

    return fetched;
}

// Empties the staging folder at the end of a run, a failure to do so joining any earlier one
static bool empty_staging(const std::string& staging, bool succeeded, failure& failed) {
    std::string error;
    if (remove_tree(staging, error)) {
        return succeeded;
    }

    if (succeeded) {
        failed = { failure_kind::io, error };
    } else {
        failed.reason += ", and mcufw " + error;
    }

    return false;
}

static bool lock_state(const directory_layout& layout, file_lock& lock, failure& failed) {
    std::string error;
    if (!make_directories(layout.state, error) ||
        !lock.acquire(join_path(layout.state, STATE_LOCK_NAME), lock_mode::exclusive, error)) {
        failed = { failure_kind::lock, error };
        return false;
    }

    return true;
}

bool install_targets(const directory_layout& layout, const std::vector<install_target>& targets, reporter& report,
                     failure& failed) {
    file_lock lock;
    if (!lock_state(layout, lock, failed)) {
        return false;
    }

    std::string staging = staging_folder(layout);
    std::string index_path = join_path(layout.state, PACKAGE_INDEX_FILE);
    std::string error;
    if (!remove_tree(staging, error)) {
        failed = { failure_kind::io, error };
        return false;
    }

    bool installed = fetch_package_index(index_path, failed);
    for (size_t i = 0; installed && i < targets.size(); i++) {
        installed = install_core_or_board(layout, index_path, targets[i], report, failed);
    }

    return empty_staging(staging, installed, failed);
}

static bool remove_core(const directory_layout& layout, const board_request& request, reporter& report,
                        failure& failed) {
    std::string item = core_item(request.packager, request.architecture);
    std::string version;
    if (!installed_version(layout, request.packager, request.architecture, version, failed)) {
        return false;
    }

    if (version.empty()) {
        failed = { failure_kind::core, item + " is not installed" };
        return false;
    }

    std::string installed = join_path(join_path(layout.hardware, request.packager), request.architecture);
    std::string discard;
    std::string error;
    if (!free_discard_path(layout, discard, error) || !discard_directory(installed, discard, error)) {
        failed = { failure_kind::io, error };
        return false;
    }

    report.removed(item, version);

    return true;
}

bool remove_targets(const directory_layout& layout, const std::vector<install_target>& targets, reporter& report,
                    failure& failed) {
    file_lock lock;
    if (!lock_state(layout, lock, failed)) {
        return false;
    }

    std::string staging = staging_folder(layout);
    std::string error;
    if (!remove_tree(staging, error)) {
        failed = { failure_kind::io, error };
        return false;
    }

    bool removed = true;
    for (size_t i = 0; removed && i < targets.size(); i++) {
        removed = remove_core(layout, targets[i].board, report, failed);
    }

    removed = removed && remove_unused_data_tools(layout, report, failed);

    return empty_staging(staging, removed, failed);
}
