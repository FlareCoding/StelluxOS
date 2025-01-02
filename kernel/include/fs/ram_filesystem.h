#ifndef RAM_FILESYSTEM_H
#define RAM_FILESYSTEM_H
#include "filesystem.h"
#include <kstl/vector.h>

namespace fs {
struct ramfs_node;

/**
 * @struct ramfs_direntry
 * @brief Represents a directory entry in the RAM filesystem.
 * 
 * Contains the name and a pointer to the associated node in the RAM filesystem.
 */
struct ramfs_direntry {
    kstl::string name;
    ramfs_node* node;
};

/**
 * @struct ramfs_node
 * @brief Represents a node in the RAM filesystem.
 * 
 * A node can be a file or a directory, with associated metadata and data storage.
 */
struct ramfs_node {
    kstl::string name;
    vfs_node_type type;
    uint32_t permissions;
    uint64_t creation_ts;
    uint64_t modification_ts;
    uint64_t access_ts;
    uint8_t* data; // File data (only for files)
    size_t data_size; // Size of the file data
    kstl::vector<ramfs_direntry*> children; // Directory children
};

/**
 * @class ram_filesystem
 * @brief Implements an in-memory filesystem based on the RAM filesystem structure.
 * 
 * Provides operations to manage files and directories entirely in RAM, making it
 * fast and lightweight for temporary storage.
 */
class ram_filesystem : public filesystem {
public:
    /**
     * @brief Creates the root node for the RAM filesystem.
     * @return A shared pointer to the root node.
     * 
     * Overrides the base `filesystem` implementation to initialize the root node.
     */
    kstl::shared_ptr<vfs_node> create_root_node() override;

    /**
     * @brief Unmounts the RAM filesystem.
     * 
     * Cleans up resources and detaches the filesystem from the virtual filesystem layer.
     */
    void unmount() override;

    /**
     * @brief Sets the operations for a node based on its path.
     * @param node Reference to the node to configure.
     * @param path Path to the node within the filesystem.
     * 
     * Configures the node with appropriate operations for interaction.
     */
    void set_ops(kstl::shared_ptr<vfs_node>& node, const kstl::string& path) override;

private:
    kstl::shared_ptr<vfs_node> m_root; /** Shared pointer to the root node */
    mutex m_fs_lock = mutex(); /** Mutex for synchronizing filesystem operations */

    // RAM Filesystem Operations

    /**
     * @brief Reads data from a RAM filesystem node.
     * @param node Pointer to the node to read from.
     * @param buffer Buffer to store the read data.
     * @param size Number of bytes to read.
     * @param offset Offset in the file to start reading from.
     * @return Number of bytes read on success, or a negative error code.
     */
    static ssize_t ramfs_read(vfs_node* node, void* buffer, size_t size, uint64_t offset);

    /**
     * @brief Writes data to a RAM filesystem node.
     * @param node Pointer to the node to write to.
     * @param buffer Buffer containing the data to write.
     * @param size Number of bytes to write.
     * @param offset Offset in the file to start writing at.
     * @return Number of bytes written on success, or a negative error code.
     */
    static ssize_t ramfs_write(vfs_node* node, const void* buffer, size_t size, uint64_t offset);

    /**
     * @brief Looks up a child node by name within a parent node.
     * @param parent Pointer to the parent node.
     * @param name Name of the child node to search for.
     * @return Shared pointer to the found node, or null if not found.
     */
    static kstl::shared_ptr<vfs_node> ramfs_lookup(vfs_node* parent, const char* name);

    /**
     * @brief Creates a new node in the RAM filesystem.
     * @param parent Pointer to the parent node.
     * @param name Name of the new node.
     * @param type Type of the node (file or directory).
     * @param perms Permission bits for the new node.
     * @return 0 on success, or a negative error code.
     */
    static int ramfs_create(vfs_node* parent, const char* name, vfs_node_type type, uint32_t perms);

    /**
     * @brief Removes a node from the RAM filesystem.
     * @param parent Pointer to the parent node.
     * @param node Pointer to the node to remove.
     * @return 0 on success, or a negative error code.
     */
    static int ramfs_remove(vfs_node* parent, vfs_node* node);

    /**
     * @brief Lists directory entries for a given node.
     * @param node Pointer to the directory node.
     * @param entries Vector to populate with directory entry names.
     * @return 0 on success, or a negative error code.
     */
    static int ramfs_listdir(vfs_node* node, kstl::vector<kstl::string>& entries);

    /**
     * @brief Deletes a file node from the RAM filesystem.
     * @param file_node Pointer to the file node to delete.
     * 
     * Frees resources associated with the file node.
     */
    static void _delete_ram_file(ramfs_node* file_node);

    /**
     * @brief Deletes a directory node from the RAM filesystem.
     * @param dir_node Pointer to the directory node to delete.
     * 
     * Recursively deletes child nodes before removing the directory.
     */
    static void _delete_ram_directory(ramfs_node* dir_node);
};
} // namespace fs

#endif // RAM_FILESYSTEM_H
