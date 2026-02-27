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

/**
 * Extract the parent path and final component from a full path.
 * E.g. "/foo/bar/baz" -> parent_end=8 (points past "/foo/bar"), name="baz", name_len=3.
 *
 * @param path Full absolute path.
 * @param out_name Pointer to the last component within path.
 * @param out_name_len Length of the last component.
 * @param out_parent_len Length of the parent portion (including leading /).
 * @return OK on success, ERR_INVAL if path is empty or has no components.
 */
int32_t path_parent(const char* path, const char*& out_name,
                    size_t& out_name_len, size_t& out_parent_len);

} // namespace fs

#endif // STELLUX_FS_PATH_H
