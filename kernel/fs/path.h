#ifndef STELLUX_FS_PATH_H
#define STELLUX_FS_PATH_H

#include "fs/fstypes.h"

namespace fs {

/**
 * Iterates over path components, splitting by '/'.
 * Skips empty components (consecutive slashes) and '.'.
 * Does NOT handle '..' -- that's the path resolver's job.
 *
 * Usage:
 *   path_iterator it("/foo/bar/baz");
 *   const char* comp;
 *   size_t len;
 *   while (it.next(comp, len)) {
 *       // comp="foo" len=3, then "bar" len=3, then "baz" len=3
 *   }
 */
class path_iterator {
public:
    explicit path_iterator(const char* path);

    /**
     * Advance to the next component.
     * @param out_name Pointer into the original path string (not null-terminated).
     * @param out_len Length of the component.
     * @return true if a component was found, false if end of path.
     */
    bool next(const char*& out_name, size_t& out_len);

private:
    const char* m_path;
    size_t m_pos;
    size_t m_len;
};

} // namespace fs

#endif // STELLUX_FS_PATH_H
