#include "fs/path.h"
#include "fs/fs.h"
#include "common/string.h"

namespace fs {

path_iterator::path_iterator(const char* path)
    : m_path(path)
    , m_pos(0)
    , m_len(path ? string::strnlen(path, PATH_MAX) : 0) {
}

bool path_iterator::next(const char*& out_name, size_t& out_len) {
    while (true) {
        while (m_pos < m_len && m_path[m_pos] == '/') {
            m_pos++;
        }

        if (m_pos >= m_len) {
            return false;
        }

        size_t start = m_pos;
        while (m_pos < m_len && m_path[m_pos] != '/') {
            m_pos++;
        }

        out_name = m_path + start;
        out_len = m_pos - start;

        if (out_len == 1 && out_name[0] == '.') {
            continue;
        }

        return true;
    }
}

int32_t path_parent(const char* path, const char*& out_name,
                    size_t& out_name_len, size_t& out_parent_len) {
    if (!path || path[0] != '/') {
        return ERR_INVAL;
    }

    size_t len = string::strnlen(path, PATH_MAX);
    if (len == 0) {
        return ERR_INVAL;
    }

    // Find the last slash (ignoring trailing slashes)
    size_t end = len;
    while (end > 0 && path[end - 1] == '/') {
        end--;
    }
    if (end == 0) {
        return ERR_INVAL; // path is only slashes, no component
    }

    // Find start of last component
    size_t name_start = end;
    while (name_start > 0 && path[name_start - 1] != '/') {
        name_start--;
    }

    out_name = path + name_start;
    out_name_len = end - name_start;

    if (out_name_len > NAME_MAX) {
        return ERR_NAMETOOLONG;
    }

    // Parent is everything up to (but not including) the last component
    // But keep at least one '/' for the root case
    out_parent_len = name_start;
    if (out_parent_len == 0) {
        out_parent_len = 1; // parent is "/"
    }

    return OK;
}

} // namespace fs
