#include "fw/fw.h"
#include "fs/fs.h"
#include "common/string.h"
#include "mm/heap.h"

namespace fw {

// The longest name whose full path, `DIRECTORY`, a '/', the name and a NUL, fits in
// `fs::PATH_MAX`. `fs::` truncates a longer path without an error.
constexpr size_t MAX_NAME_BYTES = fs::PATH_MAX - sizeof(DIRECTORY) - 1;

static bool is_valid_component(const char* component, size_t length) {
    bool is_dot = length == 1 && component[0] == '.';
    bool is_dot_dot = length == 2 && component[0] == '.' && component[1] == '.';
    return length >= 1 && length <= fs::NAME_MAX && !is_dot && !is_dot_dot;
}

static bool is_valid_name(const char* name) {
    size_t length = string::strnlen(name, MAX_NAME_BYTES + 1);
    if (length > MAX_NAME_BYTES) {
        return false;
    }

    size_t component_start = 0;
    for (size_t i = 0; i <= length; i++) {
        bool ends_component = i == length || name[i] == '/';
        if (!ends_component) {
            continue;
        }

        if (!is_valid_component(name + component_start, i - component_start)) {
            return false;
        }

        component_start = i + 1;
    }

    return true;
}

// After `fs::open` found nothing at `path`, tells a missing directory from a
// missing file by looking up the file's directory. Ends `path` at that directory.
static int32_t result_for_missing_file(char* path) {
    char* last_slash = path + string::strlen(path);
    while (*last_slash != '/') {
        last_slash--;
    }

    *last_slash = '\0';

    fs::vattr attr = {};
    int32_t stat_result = fs::stat(path, &attr);
    if (stat_result == fs::ERR_NOENT) {
        return ERR_NOT_INSTALLED;
    }

    bool is_directory = stat_result == fs::OK && attr.type == fs::node_type::directory;

    return is_directory ? ERR_MISSING : ERR_UNREADABLE;
}

static int32_t open_regular_file(char* path, fs::file*& file_out, uint64_t& size_out) {
    int32_t open_result = fs::OK;
    fs::file* opened = fs::open(path, fs::O_RDONLY, &open_result);
    if (!opened) {
        return open_result == fs::ERR_NOENT ? result_for_missing_file(path) : ERR_UNREADABLE;
    }

    fs::vattr attr = {};
    int32_t stat_result = fs::fstat(opened, &attr);
    bool is_regular = stat_result == fs::OK && attr.type == fs::node_type::regular;
    if (!is_regular) {
        fs::close(opened);
        return ERR_UNREADABLE;
    }

    file_out = opened;
    size_out = attr.size;

    return OK;
}

file::~file() {
    close();
}

int32_t file::open(const char* name) {
    if (m_file || !is_valid_name(name)) {
        return ERR_INVALID_ARG;
    }

    char* path = static_cast<char*>(heap::ualloc(fs::PATH_MAX));
    if (!path) {
        return ERR_UNREADABLE;
    }

    size_t directory_length = sizeof(DIRECTORY) - 1;
    size_t name_length = string::strlen(name);

    string::memcpy(path, DIRECTORY, directory_length);
    path[directory_length] = '/';
    string::memcpy(path + directory_length + 1, name, name_length + 1);

    int32_t rc = open_regular_file(path, m_file, m_size);
    heap::ufree(path);

    return rc;
}

int32_t file::read(uint64_t offset, void* dst, size_t count) const {
    uint64_t end = offset + count;
    bool is_inside_file = end >= offset && end <= m_size;
    if (!m_file || !is_inside_file) {
        return ERR_INVALID_ARG;
    }

    ssize_t copied = fs::read_at(m_file, dst, count, offset);
    if (copied != static_cast<ssize_t>(count)) {
        return ERR_UNREADABLE;
    }

    return OK;
}

void file::close() {
    if (!m_file) {
        return;
    }

    fs::close(m_file);
    m_file = nullptr;
    m_size = 0;
}

} // namespace fw
