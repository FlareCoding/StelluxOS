#include "files.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>

constexpr size_t READ_CHUNK_BYTES = 16 * 1024;

// The directories musl's `execvp` and `posix_spawnp` search when `PATH` is unset
constexpr const char* DEFAULT_PROGRAM_PATH = "/usr/local/bin:/bin:/usr/bin";

constexpr mode_t DIRECTORY_MODE = 0755;
constexpr mode_t FILE_MODE = 0644;

// A changed file is written whole beside its target under this suffix, then renamed over it
constexpr const char* PARTIAL_SUFFIX = ".partial";

std::string join_path(std::string_view directory, std::string_view name) {
    if (directory.empty()) {
        return std::string(name);
    }

    std::string path(directory);
    if (path.back() != '/') {
        path += '/';
    }

    path += name;

    return path;
}

std::string file_name(std::string_view path) {
    size_t slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string parent_directory(std::string_view path) {
    size_t slash = path.rfind('/');
    if (slash == std::string_view::npos) {
        return {};
    }

    return std::string(slash == 0 ? path.substr(0, 1) : path.substr(0, slash));
}

static std::string describe_errno(const char* action, const std::string& path, int error_number) {
    return std::string(action) + " " + path + ": " + strerror(error_number);
}

bool read_file(const std::string& path, std::string& text, std::string& error) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = describe_errno("cannot open", path, errno);
        return false;
    }

    text.clear();
    char chunk[READ_CHUNK_BYTES];
    while (true) {
        ssize_t count = read(fd, chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (count < 0) {
            error = describe_errno("cannot read", path, errno);
            close(fd);
            return false;
        }

        if (count == 0) {
            break;
        }

        text.append(chunk, static_cast<size_t>(count));
    }

    close(fd);

    return true;
}

bool is_directory(const std::string& path) {
    struct stat status;
    return stat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}

bool is_file(const std::string& path) {
    struct stat status;
    return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

bool list_directory(const std::string& path, std::vector<std::string>& names, std::string& error) {
    DIR* directory = opendir(path.c_str());
    if (!directory) {
        error = describe_errno("cannot list", path, errno);
        return false;
    }

    names.clear();
    errno = 0;
    while (dirent* entry = readdir(directory)) {
        bool is_dot = strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0;
        if (!is_dot) {
            names.emplace_back(entry->d_name);
        }
    }

    int read_error = errno;
    closedir(directory);
    if (read_error != 0) {
        error = describe_errno("cannot list", path, read_error);
        return false;
    }

    std::sort(names.begin(), names.end());

    return true;
}

bool list_subdirectories(const std::string& path, std::vector<std::string>& names, std::string& error) {
    std::vector<std::string> entries;
    if (!list_directory(path, entries, error)) {
        return false;
    }

    names.clear();
    for (const std::string& entry : entries) {
        if (is_directory(join_path(path, entry))) {
            names.push_back(entry);
        }
    }

    return true;
}

static bool is_executable_file(const std::string& path) {
    return is_file(path) && access(path.c_str(), X_OK) == 0;
}

std::string find_program(const std::string& word) {
    if (word.find('/') != std::string::npos) {
        return is_executable_file(word) ? word : std::string();
    }

    const char* path = getenv("PATH");
    std::string_view directories = path ? path : DEFAULT_PROGRAM_PATH;
    while (true) {
        size_t colon = directories.find(':');
        std::string candidate = join_path(directories.substr(0, colon), word);
        if (is_executable_file(candidate)) {
            return candidate;
        }

        if (colon == std::string_view::npos) {
            return {};
        }

        directories.remove_prefix(colon + 1);
    }
}

bool make_directories(const std::string& path, std::string& error) {
    size_t end = 0;
    while (end != std::string::npos) {
        end = path.find('/', end + 1);
        std::string prefix = path.substr(0, end);
        if (prefix.empty() || is_directory(prefix)) {
            continue;
        }

        if (mkdir(prefix.c_str(), DIRECTORY_MODE) != 0) {
            int mkdir_error = errno;
            if (mkdir_error != EEXIST || !is_directory(prefix)) {
                error = describe_errno("cannot create", prefix, mkdir_error);
                return false;
            }
        }
    }

    return true;
}

bool remove_tree(const std::string& path, std::string& error) {
    struct stat status;
    if (lstat(path.c_str(), &status) != 0) {
        if (errno == ENOENT) {
            return true;
        }

        error = describe_errno("cannot inspect", path, errno);
        return false;
    }

    if (S_ISDIR(status.st_mode)) {
        std::vector<std::string> names;
        if (!list_directory(path, names, error)) {
            return false;
        }

        for (const std::string& name : names) {
            if (!remove_tree(join_path(path, name), error)) {
                return false;
            }
        }
    }

    if ((S_ISDIR(status.st_mode) ? rmdir(path.c_str()) : unlink(path.c_str())) != 0) {
        error = describe_errno("cannot delete", path, errno);
        return false;
    }

    return true;
}

bool write_all(int fd, const std::string& path, std::string_view bytes, std::string& error) {
    while (!bytes.empty()) {
        ssize_t count = write(fd, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (count < 0) {
            error = describe_errno("cannot write", path, errno);
            return false;
        }

        bytes.remove_prefix(static_cast<size_t>(count));
    }

    return true;
}

bool write_new_file(const std::string& path, std::string_view text, std::string& error) {
    int fd = -1;
    if (!create_new_file(path, FILE_MODE, fd, error)) {
        return false;
    }

    bool written = write_all(fd, path, text, error);
    if (close(fd) != 0 && written) {
        error = describe_errno("cannot write", path, errno);
        written = false;
    }

    return written;
}

bool create_new_file(const std::string& path, uint32_t mode, int& fd, std::string& error) {
    fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, static_cast<mode_t>(mode));
    if (fd < 0) {
        error = describe_errno("cannot create", path, errno);
        return false;
    }

    return true;
}

static bool exists(const std::string& path) {
    struct stat status;
    return lstat(path.c_str(), &status) == 0;
}

bool discard_directory(const std::string& path, const std::string& discard, std::string& error) {
    if (!exists(path)) {
        return true;
    }

    if (rename(path.c_str(), discard.c_str()) != 0) {
        error = describe_errno("cannot move aside", path, errno);
        return false;
    }

    return remove_tree(discard, error);
}

bool replace_directory(const std::string& staged, const std::string& installed, const std::string& discard,
                       std::string& error) {
    if (!make_directories(parent_directory(installed), error)) {
        return false;
    }

    bool had_installed = exists(installed);
    if (had_installed && rename(installed.c_str(), discard.c_str()) != 0) {
        error = describe_errno("cannot move aside", installed, errno);
        return false;
    }

    if (rename(staged.c_str(), installed.c_str()) != 0) {
        error = describe_errno("cannot move into place", staged, errno);
        if (had_installed && rename(discard.c_str(), installed.c_str()) != 0) {
            error += ", and " + describe_errno("cannot move back", discard, errno);
        }

        return false;
    }

    return remove_tree(discard, error);
}

bool write_file_if_changed(const std::string& path, std::string_view text, bool& changed, std::string& error) {
    std::string current;
    std::string ignored;
    changed = !read_file(path, current, ignored) || current != text;
    if (!changed) {
        return true;
    }

    std::string partial = path + PARTIAL_SUFFIX;
    int fd = open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, FILE_MODE);
    if (fd < 0) {
        error = describe_errno("cannot create", partial, errno);
        return false;
    }

    bool written = write_all(fd, partial, text, error);
    if (close(fd) != 0 && written) {
        error = describe_errno("cannot write", partial, errno);
        written = false;
    }

    if (written && rename(partial.c_str(), path.c_str()) != 0) {
        error = describe_errno("cannot replace", path, errno);
        written = false;
    }

    if (!written) {
        unlink(partial.c_str());
    }

    return written;
}

bool copy_file_if_changed(const std::string& from, const std::string& to, bool& changed, std::string& error) {
    std::string text;
    return read_file(from, text, error) && write_file_if_changed(to, text, changed, error);
}

static bool is_version_control_folder(const std::string& name) {
    static const std::vector<std::string> folders = {
        "CVS", "RCS", ".git", ".github", ".svn", ".hg", ".bzr", ".vscode", ".settings", ".pioenvs", ".piolibdeps",
    };

    return std::find(folders.begin(), folders.end(), name) != folders.end();
}

static bool is_source_file(const std::string& name) {
    static const std::vector<std::string> extensions = { ".c", ".cpp", ".cxx", ".cc", ".S" };
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const std::string& extension) { return name.ends_with(extension); });
}

bool enter_folder(const std::string& folder, std::vector<std::string>& walked, std::string& error) {
    char resolved[PATH_MAX];
    if (realpath(folder.c_str(), resolved) == nullptr) {
        error = describe_errno("cannot read", folder, errno);
        return false;
    }

    if (std::find(walked.begin(), walked.end(), resolved) != walked.end()) {
        error = folder + " leads back to a folder holding it, a symbolic link loop";
        return false;
    }

    walked.push_back(resolved);

    return true;
}

static bool add_source_files(const std::string& folder, bool recursive, std::vector<std::string>& walked,
                             std::vector<std::string>& sources, std::string& error) {
    std::vector<std::string> names;
    if (!enter_folder(folder, walked, error) || !list_directory(folder, names, error)) {
        return false;
    }

    for (const std::string& name : names) {
        if (name[0] == '.' || is_version_control_folder(name)) {
            continue;
        }

        std::string path = join_path(folder, name);
        if (!is_directory(path)) {
            if (is_source_file(name)) {
                sources.push_back(path);
            }
        } else if (recursive && !add_source_files(path, recursive, walked, sources, error)) {
            return false;
        }
    }

    walked.pop_back();

    return true;
}

bool find_source_files(const std::string& folder, bool recursive, std::vector<std::string>& sources,
                       std::string& error) {
    std::vector<std::string> walked;
    sources.clear();

    return add_source_files(folder, recursive, walked, sources, error);
}
