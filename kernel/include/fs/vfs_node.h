#ifndef VFS_NODE_H
#define VFS_NODE_H
#include <memory/memory.h>
#include <kstl/vector.h>
#include <core/string.h>

namespace fs {
class filesystem;
struct vfs_node;

/**
 * @enum vfs_node_type
 * @brief Represents the type of a Virtual File System (VFS) node.
 * 
 * Used to classify nodes in the VFS, such as files, directories, and special nodes.
 */
enum class vfs_node_type {
    invalid = 0,
    file,
    directory,
    mount_point,
    special
};

/**
 * @typedef vfs_read_t
 * @brief Function pointer type for read operations.
 * 
 * Defines the signature for reading data from a VFS node.
 * @param node Pointer to the node to read from.
 * @param buffer Buffer to store the read data.
 * @param size Number of bytes to read.
 * @param offset Offset in the file to start reading.
 * @return Number of bytes read on success, or a negative error code.
 */
typedef ssize_t (*vfs_read_t)(vfs_node* node, void* buffer, size_t size, uint64_t offset);

/**
 * @typedef vfs_write_t
 * @brief Function pointer type for write operations.
 * 
 * Defines the signature for writing data to a VFS node.
 * @param node Pointer to the node to write to.
 * @param buffer Buffer containing the data to write.
 * @param size Number of bytes to write.
 * @param offset Offset in the file to start writing.
 * @return Number of bytes written on success, or a negative error code.
 */
typedef ssize_t (*vfs_write_t)(vfs_node* node, const void* buffer, size_t size, uint64_t offset);

/**
 * @typedef vfs_lookup_t
 * @brief Function pointer type for lookup operations.
 * 
 * Defines the signature for finding a child node by name within a parent node.
 * @param parent Pointer to the parent node.
 * @param name Name of the child node to locate.
 * @return Shared pointer to the found node, or `vfs_null_node` if not found.
 */
typedef kstl::shared_ptr<vfs_node> (*vfs_lookup_t)(vfs_node* parent, const char* name);

/**
 * @typedef vfs_create_t
 * @brief Function pointer type for node creation operations.
 * 
 * Defines the signature for creating a new node in the VFS.
 * @param parent Pointer to the parent node.
 * @param name Name of the new node.
 * @param type Type of the node (file, directory, etc.).
 * @param perms Permissions for the new node.
 * @return 0 on success, or a negative error code.
 */
typedef int (*vfs_create_t)(vfs_node* parent, const char* name, vfs_node_type type, uint32_t perms);

/**
 * @typedef vfs_delete_t
 * @brief Function pointer type for node deletion operations.
 * 
 * Defines the signature for removing a node from the VFS.
 * @param parent Pointer to the parent node.
 * @param node Pointer to the node to delete.
 * @return 0 on success, or a negative error code.
 */
typedef int (*vfs_delete_t)(vfs_node* parent, vfs_node* node);

/**
 * @typedef vfs_listdir_t
 * @brief Function pointer type for listing directory entries.
 * 
 * Defines the signature for populating a list of entries in a directory node.
 * @param node Pointer to the directory node.
 * @param entries Vector to store the names of directory entries.
 * @return 0 on success, or a negative error code.
 */
typedef int (*vfs_listdir_t)(vfs_node* node, kstl::vector<kstl::string>& entries);

/**
 * @struct vfs_operations
 * @brief Defines the set of operations that can be performed on a VFS node.
 * 
 * This structure encapsulates function pointers for filesystem-specific operations.
 */
struct vfs_operations {
    vfs_read_t       read;
    vfs_write_t      write;
    vfs_lookup_t     lookup;
    vfs_create_t     create;
    vfs_delete_t     remove;
    vfs_listdir_t    listdir;
};

/**
 * @struct vfs_stat_struct
 * @brief Stores metadata about a VFS node.
 * 
 * Contains information such as type, size, permissions, and timestamps.
 */
struct vfs_stat_struct {
    vfs_node_type   type;               // Type of node (file, directory, etc.)
    uint64_t        size;               // Size of the file
    uint32_t        perms;              // Permissions (e.g., rwx flags)
    uint64_t        creation_ts;        // Creation timestamp
    uint64_t        modification_ts;    // Last modification timestamp
    uint64_t        access_ts;          // Last access timestamp
};


/**
 * @struct vfs_node
 * @brief Represents a node in the Virtual File System (VFS).
 * 
 * A VFS node encapsulates metadata, operations, and pointers to filesystem-specific data.
 */
struct vfs_node {
    // Node information structure
    vfs_stat_struct stat;

    // Filesystem-specific operations
    vfs_operations  ops;

    // Pointer to filesystem-specific metadata
    void*           _private;

    // Pointer to the owning filesystem
    filesystem*     fs;
};

/**
 * @brief Represents a null VFS node.
 * 
 * A shared pointer initialized to `nullptr` to signify an invalid or non-existent node.
 */
static kstl::shared_ptr<vfs_node> vfs_null_node = kstl::shared_ptr<vfs_node>(nullptr);
} // namespace fs

#endif // VFS_NODE_H
