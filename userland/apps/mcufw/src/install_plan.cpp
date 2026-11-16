#include "installer.hpp"

#include "files.hpp"
#include "versions.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <set>

// Each library's releases the resolver chooses among, newest first
using release_table = std::map<std::string, std::vector<const library_release*>>;

// A dependency still to settle, with the release that states it
struct pending_dependency {
    const library_release*    dependent;
    const library_dependency* dependency;
};

// A library target: its newest release or the one it names, which a pinned target keeps in every attempt
struct library_goal {
    const library_release* release;
    bool                   is_pinned;
};

// The targets' resolution as relaxed-semver's resolver runs it: the first pending dependency takes the newest release
// meeting it whose dependencies the index lists, backtracking when the rest fail, and failed dependencies move first
struct library_search {
    const release_table*                          table = nullptr;
    std::map<std::string, const library_release*> solution;
    std::vector<pending_dependency>               pending;
    std::map<std::string, size_t>                 failures;  // per dependency as `name/constraint`
    size_t                                        steps = 0;
    std::string                                   error;     // set when the search must stop
};

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

// Backtracking can take exponential time on a hostile index, so the search stops here
constexpr size_t RESOLUTION_STEPS_MAX = 100000;

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
    if (parts.size() == 1 && !name.empty()) {
        target.kind = install_kind::library;
        target.library = name;
        return true;
    }

    if (parts.size() == 2 && !parts[0].empty() && !parts[1].empty()) {
        target.kind = install_kind::core;
        target.board.packager = parts[0];
        target.board.architecture = parts[1];
        return true;
    }

    if (parts.size() < 3) {
        error = std::string(name) + " names no library, core or board: a core is packager:architecture with both parts";
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

static bool is_installed_copy(const library& present, const std::string& name) {
    return present.location == library_location::installed && present.name == name;
}

// An installed library as a message names it, with its version when it states one
static std::string described(const library& present) {
    return present.version.empty() ? present.name : present.name + " " + present.version;
}

// Each library's releases, newest first: the override alone where one is given, else every release the index lists
static release_table build_table(const std::vector<library_release>& releases,
                                 const std::map<std::string, const library_release*>& overrides) {
    release_table table;
    for (const library_release& release : releases) {
        if (!overrides.contains(release.name)) {
            table[release.name].push_back(&release);
        }
    }

    for (auto& [name, listed] : table) {
        std::stable_sort(listed.begin(), listed.end(), [](const library_release* a, const library_release* b) {
            return compare_versions(a->version, b->version) > 0;
        });
    }

    for (const auto& [name, release] : overrides) {
        table[name] = { release };
    }

    return table;
}

static const library_release* find_version(const release_table& table, const std::string& name,
                                           const std::string& version) {
    auto listed = table.find(name);
    if (listed == table.end()) {
        return nullptr;
    }

    for (const library_release* release : listed->second) {
        if (version.empty() || compare_versions(release->version, version) == 0) {
            return release;
        }
    }

    return nullptr;
}

// The release an installed library stands for: as arduino-cli's --no-overwrite takes it, the index's release of its
// version or the newest when its version does not read, else its own version without dependencies
static const library_release* installed_release(const release_table& index, const library& present,
                                                std::deque<library_release>& unlisted) {
    if (present.is_legacy || !index.contains(present.name)) {
        return nullptr;
    }

    bool has_version = !present.version.empty() && is_semantic_version(present.version);
    const library_release* listed = find_version(index, present.name, has_version ? present.version : "");
    if (listed) {
        return listed;
    }

    unlisted.push_back({ present.name, present.version, {}, {}, {} });

    return &unlisted.back();
}

static size_t failure_count(const library_search& search, const pending_dependency& pending) {
    auto counted = search.failures.find(pending.dependency->name + "/" + pending.dependency->constraint);
    return counted == search.failures.end() ? 0 : counted->second;
}

// Whether `release` meets the constraint `pending` states, stopping the search on one it cannot read
static bool read_constraint(library_search& search, const pending_dependency& pending, const library_release& release,
                            bool& meets) {
    std::string error;
    if (check_constraint(release.version, pending.dependency->constraint, meets, error)) {
        return true;
    }

    search.error = pending.dependent->name + " " + pending.dependent->version + "'s dependency on " +
                   pending.dependency->name + ": " + error;

    return false;
}

static bool lists_dependencies(const release_table& table, const library_release& release) {
    for (const library_dependency& dependency : release.dependencies) {
        if (!table.contains(dependency.name)) {
            return false;
        }
    }

    return true;
}

static bool search_releases(library_search& search);

// Settles the first pending dependency on a release already in the solution, which must meet it
static bool settle_on_chosen(library_search& search, const library_release& chosen) {
    bool meets = false;
    if (!read_constraint(search, search.pending.front(), chosen, meets) || !meets) {
        return false;
    }

    std::vector<pending_dependency> saved = search.pending;
    search.pending.erase(search.pending.begin());
    if (search_releases(search)) {
        return true;
    }

    search.pending = saved;

    return false;
}

static bool search_releases(library_search& search) {
    if (search.pending.empty()) {
        return true;
    }

    if (++search.steps > RESOLUTION_STEPS_MAX) {
        search.error = "the dependencies take more than " + std::to_string(RESOLUTION_STEPS_MAX) + " steps to resolve";
        return false;
    }

    pending_dependency next = search.pending.front();
    const std::string& name = next.dependency->name;
    auto chosen = search.solution.find(name);
    if (chosen != search.solution.end()) {
        return settle_on_chosen(search, *chosen->second);
    }

    auto listed = search.table->find(name);
    std::vector<const library_release*> candidates;
    if (listed != search.table->end()) {
        candidates = listed->second;
    }

    for (const library_release* candidate : candidates) {
        bool meets = false;
        if (!read_constraint(search, next, *candidate, meets)) {
            return false;
        }

        if (!meets || !lists_dependencies(*search.table, *candidate)) {
            continue;
        }

        std::vector<pending_dependency> saved = search.pending;
        search.solution[name] = candidate;
        search.pending.erase(search.pending.begin());
        for (const library_dependency& dependency : candidate->dependencies) {
            search.pending.push_back({ candidate, &dependency });
        }

        std::stable_sort(search.pending.begin(), search.pending.end(), [&](const auto& a, const auto& b) {
            return failure_count(search, a) > failure_count(search, b);
        });

        if (search_releases(search)) {
            return true;
        }

        if (!search.error.empty()) {
            return false;
        }

        search.pending = saved;
        search.solution.erase(name);
    }

    search.failures[name + "/" + next.dependency->constraint]++;

    return false;
}

// The releases the targets need with them, found together as relaxed-semver's resolver finds one release's
static bool search_targets(const release_table& table, const std::vector<library_goal>& goals,
                           std::map<std::string, const library_release*>& solution, std::string& error) {
    library_release root = { "the targets", "", {}, {}, {} };
    for (const library_goal& goal : goals) {
        root.dependencies.push_back({ goal.release->name, "" });
    }

    library_search search;
    search.table = &table;
    for (const library_dependency& dependency : root.dependencies) {
        search.pending.push_back({ &root, &dependency });
    }

    bool solved = search_releases(search);
    solution = search.solution;
    error = search.error;

    return solved;
}

// An installed library the open solution moves, as the failure names it with the install that fixes it, or empty
static std::string blocking_library(const std::map<std::string, const library_release*>& open_solution,
                                    const std::map<std::string, const library_release*>& kept,
                                    const std::vector<library>& installed) {
    for (const library& present : installed) {
        auto replacing = open_solution.find(present.name);
        bool may_block = present.location == library_location::installed && replacing != open_solution.end() &&
                         kept.contains(present.name);
        if (!may_block) {
            continue;
        }

        for (const auto& [name, dependent] : open_solution) {
            for (const library_dependency& dependency : dependent->dependencies) {
                if (dependency.name == present.name && !dependency.constraint.empty()) {
                    return described(present) + " is installed, but " + name + " " + dependent->version + " needs " +
                           dependency.constraint + ", so run mcufw install \"" + present.name + "@" +
                           replacing->second->version + "\"";
                }
            }
        }
    }

    return {};
}

// Why the targets have no solution, as arduino-cli first checks the named releases' own dependencies
static std::string explain_failure(const release_table& index, const std::vector<library_goal>& goals) {
    for (const library_goal& goal : goals) {
        for (const library_dependency& dependency : goal.release->dependencies) {
            if (!index.contains(dependency.name)) {
                return "the library index lists no library " + dependency.name + ", which " + goal.release->name +
                       " " + goal.release->version + " depends on";
            }
        }
    }

    std::string targets;
    for (const library_goal& goal : goals) {
        targets += (targets.empty() ? "" : ", ") + goal.release->name;
    }

    return "no library releases meet every dependency of " + targets;
}

// The index's releases with these fixed: the targets' when `fixes_targets`, else only those given a version, and the
// installed libraries `kept`
static release_table attempt_table(const std::vector<library_release>& releases, const std::vector<library_goal>& goals,
                                   bool fixes_targets, const std::map<std::string, const library_release*>& kept) {
    std::map<std::string, const library_release*> overrides = kept;
    for (const library_goal& goal : goals) {
        if (fixes_targets || goal.is_pinned) {
            overrides[goal.release->name] = goal.release;
        }
    }

    return build_table(releases, overrides);
}

// Named libraries get their newest release, as arduino-cli installs a named library at its latest. Only when those
// cannot go together do they take the newest releases that do, as a second arduino-cli install would replace the first.
static bool find_solution(const std::vector<library_release>& releases, const release_table& index,
                          const std::vector<library_goal>& goals,
                          const std::map<std::string, const library_release*>& kept,
                          const std::vector<library>& installed,
                          std::map<std::string, const library_release*>& solution, std::string& error) {
    const std::map<std::string, const library_release*> none;
    for (bool fixes_targets : { true, false }) {
        if (search_targets(attempt_table(releases, goals, fixes_targets, kept), goals, solution, error)) {
            return true;
        }

        std::map<std::string, const library_release*> open_solution;
        bool is_open = error.empty() &&
                       search_targets(attempt_table(releases, goals, fixes_targets, none), goals, open_solution, error);
        if (!error.empty()) {
            return false;
        }

        if (is_open) {
            error = blocking_library(open_solution, kept, installed);
            error = error.empty() ? "the installed libraries leave no releases that meet every dependency" : error;
            return false;
        }
    }

    error = explain_failure(index, goals);

    return false;
}

static bool is_target(const std::vector<library_goal>& goals, const std::string& name) {
    return std::any_of(goals.begin(), goals.end(), [&](const library_goal& goal) {
        return goal.release->name == name;
    });
}

static bool find_goals(const release_table& index, const std::vector<install_target>& targets,
                       std::vector<library_goal>& goals, std::string& error) {
    std::map<std::string, const library_release*> by_name;
    for (const install_target& target : targets) {
        if (target.kind != install_kind::library) {
            continue;
        }

        const library_release* release = find_version(index, target.library, target.version);
        if (!release) {
            error = !index.contains(target.library)
                        ? "the library index lists no library " + target.library +
                              ", and a core or board is named with colons, as esp32:esp32:esp32da"
                        : "the library index lists no " + target.library + " " + target.version;
            return false;
        }

        auto [named, is_new] = by_name.emplace(release->name, release);
        if (!is_new && compare_versions(named->second->version, release->version) != 0) {
            error = "two different versions of the library " + release->name + " are required: " +
                    named->second->version + " and " + release->version;
            return false;
        }

        if (is_new) {
            goals.push_back({ release, !target.version.empty() });
        }
    }

    return true;
}

// Fails on an installed library no target names that the solution would move, which mcufw leaves to the person
static bool check_installed_stay(const std::map<std::string, const library_release*>& solution,
                                 const std::vector<library_goal>& goals, const std::vector<library>& installed,
                                 std::string& error) {
    for (const library& present : installed) {
        auto chosen = solution.find(present.name);
        bool is_moved = present.location == library_location::installed && chosen != solution.end() &&
                        !is_target(goals, present.name) &&
                        compare_versions(present.version, chosen->second->version) != 0;
        if (is_moved) {
            error = described(present) + " is installed, but the libraries being installed need " +
                    chosen->second->version + ", so run mcufw install \"" + present.name + "@" +
                    chosen->second->version + "\"";
            return false;
        }
    }

    return true;
}

// Each release after the releases it depends on, so a failure partway leaves no library without its dependencies
static void append_in_order(const std::map<std::string, const library_release*>& solution,
                            const library_release& release, std::set<std::string>& placed,
                            std::vector<const library_release*>& ordered) {
    if (!placed.insert(release.name).second) {
        return;
    }

    for (const library_dependency& dependency : release.dependencies) {
        auto chosen = solution.find(dependency.name);
        if (chosen != solution.end()) {
            append_in_order(solution, *chosen->second, placed, ordered);
        }
    }

    ordered.push_back(&release);
}

static bool is_installed_at(const std::vector<library>& installed, const library_release& release) {
    for (const library& present : installed) {
        if (is_installed_copy(present, release.name) && compare_versions(present.version, release.version) == 0) {
            return true;
        }
    }

    return false;
}

bool resolve_libraries(const std::vector<library_release>& releases, const std::vector<install_target>& targets,
                       const std::vector<library>& installed, std::vector<release_name>& chosen, std::string& error) {
    release_table index = build_table(releases, {});
    std::vector<library_goal> goals;
    if (!find_goals(index, targets, goals, error)) {
        return false;
    }

    std::map<std::string, const library_release*> kept;
    std::deque<library_release> unlisted;
    for (const library& present : installed) {
        bool may_stay = present.location == library_location::installed && !is_target(goals, present.name);
        const library_release* stays = may_stay ? installed_release(index, present, unlisted) : nullptr;
        if (stays) {
            kept.emplace(present.name, stays);
        }
    }

    std::map<std::string, const library_release*> solution;
    bool solved = find_solution(releases, index, goals, kept, installed, solution, error) &&
                  check_installed_stay(solution, goals, installed, error);
    if (!solved) {
        return false;
    }

    std::set<std::string> placed;
    std::vector<const library_release*> ordered;
    for (const library_goal& goal : goals) {
        append_in_order(solution, *solution.at(goal.release->name), placed, ordered);
    }

    chosen.clear();
    for (const library_release* release : ordered) {
        if (!is_installed_at(installed, *release)) {
            chosen.push_back({ release->name, release->version });
        }
    }

    return true;
}

bool needs_library_index(const std::vector<install_target>& targets) {
    return std::any_of(targets.begin(), targets.end(), [](const install_target& target) {
        return target.kind == install_kind::library;
    });
}

bool plan_library_installs(const std::vector<release_name>& chosen, const std::vector<library>& installed,
                           const std::string& libraries_folder, const std::vector<std::string>& folder_entries,
                           std::vector<library_install>& installs, std::string& error) {
    installs.clear();
    std::set<std::string> taken(folder_entries.begin(), folder_entries.end());
    for (const release_name& release : chosen) {
        std::vector<const library*> copies;
        for (const library& present : installed) {
            if (is_installed_copy(present, release.name)) {
                copies.push_back(&present);
            }
        }

        if (copies.size() > 1) {
            error = release.name + " is installed twice, in " + copies[0]->directory + " and " + copies[1]->directory +
                    ", so remove one before installing it";
            return false;
        }

        std::string folder = library_folder_name(release.name);
        std::string replaced = copies.empty() ? std::string() : copies[0]->directory;
        std::string destination = join_path(libraries_folder, folder);
        if (taken.contains(folder) && replaced != destination) {
            error = destination + " already holds something other than " + release.name +
                    ", so mcufw cannot install it there";
            return false;
        }

        taken.insert(folder);
        installs.push_back({ release, destination, replaced });
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
