#include "files.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

constexpr size_t READ_CHUNK_BYTES = 16 * 1024;

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
