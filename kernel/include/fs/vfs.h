#ifndef VFS_H
#define VFS_H
#include "filesystem.h"

namespace fs {
/**
 * @struct mount_point
 * @brief Represents a mounted filesystem within the virtual filesystem.
 * 
 * Contains the mount path, the root node of the mounted filesystem, and a reference to the owning filesystem.
 */
struct mount_point {
    kstl::string                    path;
    kstl::shared_ptr<vfs_node>      root_node;
    kstl::shared_ptr<filesystem>    owner_fs;
};

/**
 * @class virtual_filesystem
 * @brief Manages the virtual filesystem, providing a unified interface for all mounted filesystems.
 * 
 * The virtual filesystem (VFS) abstracts the details of individual filesystems, allowing seamless
 * interaction with files and directories across multiple mounts.
 */
class virtual_filesystem {
public:
    /**
     * @brief Retrieves the singleton instance of the virtual filesystem.
     * @return Reference to the singleton instance of the `virtual_filesystem`.
     */
    static virtual_filesystem& get();

    /**
     * @brief Mounts a filesystem at a specified path.
     * @param path The path where the filesystem should be mounted.
     * @param fs Shared pointer to the filesystem to mount.
     * @return `fs_error::success` on success, or an appropriate error code.
     */
    fs_error mount(const kstl::string& path, const kstl::shared_ptr<filesystem>& fs);

    /**
     * @brief Unmounts a filesystem from a specified path.
     * @param path The path where the filesystem is mounted.
     * @return `fs_error::success` on success, or an appropriate error code.
     */
    fs_error unmount(const kstl::string& path);

    /**
     * @brief Creates a file or directory at a specified path.
     * @param path The path where the file or directory should be created.
     * @param type The type of node to create (file or directory).
     * @param perms The permissions for the new node.
     * @return `fs_error::success` on success, or an appropriate error code.
     */
    fs_error create(const kstl::string& path, fs::vfs_node_type type, uint32_t perms);

    /**
     * @brief Removes a file or directory at a specified path.
     * @param path The path to the file or directory to remove.
     * @return `fs_error::success` on success, or an appropriate error code.
     */
    fs_error remove(const kstl::string& path);

    /**
     * @brief Reads data from a file at a specified path.
     * @param path The path to the file to read from.
     * @param buffer Buffer to store the read data.
     * @param size Number of bytes to read.
     * @param offset Offset in the file to start reading from.
     * @return Number of bytes read on success, or a negative error code.
     */
    ssize_t read(const kstl::string& path, void* buffer, size_t size, uint64_t offset);

    /**
     * @brief Writes data to a file at a specified path.
     * @param path The path to the file to write to.
     * @param buffer Buffer containing the data to write.
     * @param size Number of bytes to write.
     * @param offset Offset in the file to start writing at.
     * @return Number of bytes written on success, or a negative error code.
     */
    ssize_t write(const kstl::string& path, const void* buffer, size_t size, uint64_t offset);

    /**
     * @brief Lists entries in a directory at a specified path.
     * @param path The path to the directory.
     * @param entries Vector to populate with directory entry names.
     * @return `fs_error::success` on success, or an appropriate error code.
     */
    fs_error listdir(const kstl::string& path, kstl::vector<kstl::string>& entries);

    /**
     * @brief Retrieves metadata for a file or directory at a specified path.
     * @param path The path to the file or directory.
     * @param info Reference to a `vfs_stat_struct` to populate with metadata.
     * @return `fs_error::success` on success, or an appropriate error code.
     */
    fs_error stat(const kstl::string& path, vfs_stat_struct& info);

    /**
     * @brief Checks whether a given path exists in the virtual filesystem.
     * @param path The path to check.
     * @return True if the path exists, false otherwise.
     */
    bool path_exists(const kstl::string& path);

    /**
     * @brief Extracts the filename from a given path.
     * @param path The full path to extract the filename from.
     * @return The filename as a `kstl::string`, or an empty string if the path is invalid.
     */
    static kstl::string get_filename_from_path(const kstl::string& path);

private:
    kstl::vector<mount_point> m_mount_points; /** List of all mounted filesystems */
    mutex m_vfs_lock = mutex(); /** Mutex to synchronize VFS operations */

    /**
     * @brief Checks if a path corresponds to a mount point.
     * @param path The path to check.
     * @param out_root_node Reference to store the root node of the mount point, if found.
     * @return `fs_error::success` if the path is a mount point, or an appropriate error code.
     */
    fs_error is_mount_point(const kstl::string& path, kstl::shared_ptr<vfs_node>& out_root_node);

    /**
     * @brief Resolves a path to its corresponding VFS node.
     * @param path The path to resolve.
     * @param out_node Reference to store the resolved VFS node.
     * @return `fs_error::success` on success, or an appropriate error code.
     */
    fs_error _resolve_path(const kstl::string& path, kstl::shared_ptr<vfs_node>& out_node);

    /**
     * @brief Splits a path into its components.
     * @param path The path to split.
     * @param components Vector to populate with the individual components of the path.
     */
    static void _split_path(const kstl::string& path, kstl::vector<kstl::string>& components);
};
} // namespace fs

#endif // VFS_H
