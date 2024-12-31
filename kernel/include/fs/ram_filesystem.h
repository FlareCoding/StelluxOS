#ifndef RAM_FILESYSTEM_H
#define RAM_FILESYSTEM_H
#include "filesystem.h"
#include <kstl/vector.h>

namespace fs {
struct ramfs_node;

struct ramfs_direntry {
    kstl::string name;
    ramfs_node* node;
};

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

class ram_filesystem : public filesystem {
public:
    kstl::shared_ptr<vfs_node> create_root_node() override;

    void set_ops(kstl::shared_ptr<vfs_node>& node, const kstl::string& path) override;

private:
    // RAM Filesystem Operations
    static ssize_t ramfs_read(vfs_node* node, void* buffer, size_t size, uint64_t offset);
    static ssize_t ramfs_write(vfs_node* node, const void* buffer, size_t size, uint64_t offset);

    static kstl::shared_ptr<vfs_node> ramfs_lookup(vfs_node* parent, const char* name);
    static int ramfs_create(vfs_node* parent, const char* name, vfs_node_type type, uint32_t perms);
    static int ramfs_remove(vfs_node* parent, vfs_node* node);
    static int ramfs_listdir(vfs_node* node, kstl::vector<kstl::string>& entries);

    static void _delete_ram_file(ramfs_node* file_node);
    static void _delete_ram_directory(ramfs_node* dir_node);
};
} // namespace fs

#endif // RAM_FILESYSTEM_H
