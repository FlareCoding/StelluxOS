#ifndef VFS_NODE_H
#define VFS_NODE_H
#include <memory/memory.h>
#include <kstl/vector.h>
#include <core/string.h>

namespace fs {
class filesystem;
struct vfs_node;

enum class vfs_node_type {
    invalid = 0,
    file,
    directory,
    mount_point,
    special
};

// Define function pointer types for operations
typedef ssize_t (*vfs_read_t)(vfs_node* node, void* buffer, size_t size, uint64_t offset);
typedef ssize_t (*vfs_write_t)(vfs_node* node, const void* buffer, size_t size, uint64_t offset);
typedef kstl::shared_ptr<vfs_node> (*vfs_lookup_t)(vfs_node* parent, const char* name);
typedef int (*vfs_create_t)(vfs_node* parent, const char* name, vfs_node_type type);
typedef int (*vfs_delete_t)(vfs_node* parent, vfs_node* node);
typedef int (*vfs_listdir_t)(vfs_node* node, kstl::vector<kstl::string>& entries);

// Operations struct
struct vfs_operations {
    vfs_read_t       read;
    vfs_write_t      write;
    vfs_lookup_t     lookup;
    vfs_create_t     create;
    vfs_delete_t     remove;
    vfs_listdir_t    listdir;
};

struct vfs_node {
    uint64_t        id;                 // Unique ID for this node
    vfs_node_type   type;               // Type of node (file, directory, etc.)
    uint64_t        size;               // Size of the file
    uint32_t        perms;              // Permissions (e.g., rwx flags)
    uint64_t        creation_ts;        // Creation timestamp
    uint64_t        modification_ts;    // Last modification timestamp
    uint64_t        access_ts;          // Last access timestamp

    // Filesystem-specific operations
    vfs_operations  ops;

    // Pointer to filesystem-specific metadata
    void*           _private;

    // Pointer to the owning filesystem
    filesystem*     fs;
};

static kstl::shared_ptr<vfs_node> vfs_null_node = kstl::shared_ptr<vfs_node>(nullptr);
} // namespace fs

#endif // VFS_NODE_H
