#include "fs/path.h"
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

} // namespace fs
