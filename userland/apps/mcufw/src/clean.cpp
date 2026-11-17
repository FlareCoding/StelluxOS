#include "clean.hpp"

#include "build.hpp"
#include "build_plan.hpp"
#include "file_lock.hpp"
#include "files.hpp"
#include "sketch.hpp"

#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <string_view>
#include <vector>

constexpr std::string_view REMOVING_INFIX = ".removing-";

// `path` made absolute against the working directory, with `.` dropped and each `..` dropping the component before
// it, for a folder that may be gone
static std::string absolute_path(const std::string& path) {
    std::string whole = path;
    char working_directory[PATH_MAX];
    if (!path.starts_with('/') && getcwd(working_directory, sizeof(working_directory))) {
        whole = join_path(working_directory, path);
    }

    std::vector<std::string_view> components;
    std::string_view rest = whole;
    while (!rest.empty()) {
        size_t slash = rest.find('/');
        std::string_view component = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
        if (component == ".." && !components.empty()) {
            components.pop_back();
        } else if (!component.empty() && component != "." && component != "..") {
            components.push_back(component);
        }
    }

    std::string folded;
    for (std::string_view component : components) {
        folded += "/" + std::string(component);
    }

    return folded.empty() ? "/" : folded;
}

// The folder whose build tree `operand` names, resolved as a build resolves it while it exists
static std::string sketch_folder_named(const std::string& operand) {
    std::string path = operand.empty() ? "." : operand;
    char resolved[PATH_MAX];
    return sketch_folder_of(realpath(path.c_str(), resolved) ? std::string(resolved) : absolute_path(path));
}

// Renames the tree aside under its lock and then deletes it, so a build that starts meanwhile makes a fresh tree
static bool remove_build_tree(const std::string& tree, failure& failed) {
    if (!is_directory(tree)) {
        return true;
    }

    file_lock lock;
    std::string error;
    if (!lock.acquire(join_path(tree, BUILD_LOCK_NAME), lock_mode::exclusive, error)) {
        failed = { failure_kind::lock, error };
        return false;
    }

    std::string aside = tree + std::string(REMOVING_INFIX) + std::to_string(getpid());
    if (!rename_path(tree, aside, error) || !remove_tree(aside, error)) {
        failed = { failure_kind::io, error };
        return false;
    }

    return true;
}

// Builds hold the state lock shared while they link from cache entries, so deleting the cache holds it exclusively,
// taken before the tree goes so that a held lock leaves both
bool clean_sketch(const directory_layout& layout, const std::string& operand, bool cores, failure& failed) {
    file_lock state_lock;
    std::string error;
    if (cores && (!make_directories(layout.state, error) ||
                  !state_lock.acquire(join_path(layout.state, STATE_LOCK_NAME), lock_mode::exclusive, error))) {
        failed = { failure_kind::lock, error };
        return false;
    }

    if (!remove_build_tree(build_directory_for(layout, sketch_folder_named(operand)), failed)) {
        return false;
    }

    if (cores && is_directory(layout.core_cache) && !remove_tree(layout.core_cache, error)) {
        failed = { failure_kind::io, error };
        return false;
    }

    return true;
}
