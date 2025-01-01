#ifndef VFS_H
#define VFS_H
#include "filesystem.h"

namespace fs {
struct mount_point {
    kstl::string                    path;
    kstl::shared_ptr<vfs_node>      root_node;
    kstl::shared_ptr<filesystem>    owner_fs;
};

class virtual_filesystem {
public:
    static virtual_filesystem& get();

    fs_error mount(const kstl::string& path, const kstl::shared_ptr<filesystem>& fs);
    fs_error unmount(const kstl::string& path);

    fs_error create(const kstl::string& path, fs::vfs_node_type type, uint32_t perms);
    fs_error remove(const kstl::string& path);

    ssize_t read(const kstl::string& path, void* buffer, size_t size, uint64_t offset);
    ssize_t write(const kstl::string& path, const void* buffer, size_t size, uint64_t offset);

    fs_error listdir(const kstl::string& path, kstl::vector<kstl::string>& entries);

    bool path_exists(const kstl::string& path);

private:
    kstl::vector<mount_point> m_mount_points;
    mutex m_vfs_lock = mutex();

    fs_error is_mount_point(const kstl::string& path, kstl::shared_ptr<vfs_node>& out_root_node);

    /**
     * @brief Resolves a path string to a `vfs_node`.
     *
     * This function traverses the path string, resolving each component
     * to the corresponding node. If the path cannot be resolved, an
     * appropriate error is returned. The resulting `vfs_node` is stored
     * in the `out_node` parameter.
     *
     * @param path The input path string to resolve.
     * @param out_node A reference to a shared pointer that will store the resolved node.
     * @return fs_error An error code indicating success or the type of failure.
     */
    fs_error _resolve_path(const kstl::string& path, kstl::shared_ptr<vfs_node>& out_node);

    /**
     * @brief Splits a path into its components.
     *
     * This function splits the input `path` string into individual components,
     * preserving the root (`/`) as the first component if the path starts with `/`.
     * The resulting components are stored in the provided `components` vector.
     *
     * @param path The input path string to split.
     * @param components The vector to store the resulting path components.
     */
    void _split_path(const kstl::string& path, kstl::vector<kstl::string>& components);
};
} // namespace fs

#endif // VFS_H
