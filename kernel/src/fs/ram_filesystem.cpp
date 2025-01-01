#include <fs/ram_filesystem.h>
#include <time/time.h>
#include <serial/serial.h>

namespace fs {
kstl::shared_ptr<vfs_node> ram_filesystem::create_root_node() {
    mutex_guard guard(m_fs_lock);

    if (m_root) {
        return m_root;
    }

    // Create and return the root node
    m_root = kstl::make_shared<vfs_node>();
    m_root->stat.type = vfs_node_type::directory;

    // Create the root RAM node
    auto root_ram_node = new ramfs_node();
    root_ram_node->name = "/";
    root_ram_node->type = vfs_node_type::directory;

    // Link the VFS node with the RAM node
    m_root->_private = root_ram_node;

    return m_root;
}

void ram_filesystem::unmount() {
    mutex_guard guard(m_fs_lock);

    // Cleanup all resources
    auto root_ram_node = static_cast<ramfs_node*>(m_root->_private);
    _delete_ram_directory(root_ram_node);
}

void ram_filesystem::set_ops(kstl::shared_ptr<vfs_node>& node, const kstl::string& path) {
    __unused path;
    // Allow only directories to create
    node->ops = vfs_operations{
        .read = &ramfs_read,
        .write = &ramfs_write,
        .lookup = &ramfs_lookup,
        .create = nullptr,
        .remove = &ramfs_remove,
        .listdir = nullptr
    };

    // Directory-specific rules
    if (node->stat.type == vfs_node_type::directory) {
        node->ops.read = nullptr;
        node->ops.write = nullptr;
        node->ops.create = &ramfs_create;
        node->ops.listdir = &ramfs_listdir;
    }
}

ssize_t ram_filesystem::ramfs_read(vfs_node* node, void* buffer, size_t size, uint64_t offset) {
    auto fs = static_cast<ram_filesystem*>(node->fs);
    mutex_guard guard(fs->m_fs_lock);

    // Validate the input node and buffer
    if (!node || !buffer) {
        return make_error_code(fs_error::invalid_argument);
    }

    auto ram_node = static_cast<ramfs_node*>(node->_private);
    if (ram_node->type != vfs_node_type::file) {
        return make_error_code(fs_error::not_a_file);
    }

    // Calculate the readable size
    if (offset >= ram_node->data_size) {
        return 0; // Offset out of bounds
    }

    // Update the last access timestamp
    ram_node->access_ts = kernel_timer::get_system_time_in_milliseconds();

    size_t readable = kstl::min(size, ram_node->data_size - offset);
    memcpy(buffer, ram_node->data + offset, readable);
    return readable;
}

ssize_t ram_filesystem::ramfs_write(vfs_node* node, const void* buffer, size_t size, uint64_t offset) {
    auto fs = static_cast<ram_filesystem*>(node->fs);
    mutex_guard guard(fs->m_fs_lock);

    // Validate the input node and buffer
    if (!node || !buffer) {
        return make_error_code(fs_error::invalid_argument);
    }

    auto ram_node = static_cast<ramfs_node*>(node->_private);

    // Ensure the node is a file
    if (ram_node->type != vfs_node_type::file) {
        return make_error_code(fs_error::not_a_file);
    }

    // Resize the data buffer if necessary
    size_t required_size = offset + size;
    if (required_size > ram_node->data_size) {
        uint8_t* new_data = new uint8_t[required_size];
        if (ram_node->data) {
            memcpy(new_data, ram_node->data, ram_node->data_size); // Copy existing data
            delete[] ram_node->data; // Free old buffer
        }
        ram_node->data = new_data;
        ram_node->data_size = required_size;
    }

    // Update the last access and modification timestamps
    ram_node->access_ts = kernel_timer::get_system_time_in_milliseconds();
    ram_node->modification_ts = ram_node->access_ts;

    // Write the data into the file's data buffer
    memcpy(ram_node->data + offset, buffer, size);

    // Update the size in the VFS node to match the RAM node
    node->stat.size = ram_node->data_size;

    // Return the number of bytes written
    return size;
}

kstl::shared_ptr<vfs_node> ram_filesystem::ramfs_lookup(vfs_node* parent, const char* name) {
    auto fs = static_cast<ram_filesystem*>(parent->fs);
    mutex_guard guard(fs->m_fs_lock);

    auto ram_node = static_cast<ramfs_node*>(parent->_private);
    if (ram_node->type != vfs_node_type::directory) {
        return vfs_null_node; // Not a directory
    }

    for (ramfs_direntry* entry : ram_node->children) {
        if (entry->name == name) {
            // Create and return the virtual filesystem node
            auto vnode = kstl::make_shared<vfs_node>();
            vnode->stat.type = entry->node->type;
            vnode->stat.size = entry->node->data_size;
            vnode->stat.perms = entry->node->permissions;
            vnode->stat.creation_ts = entry->node->creation_ts;
            vnode->stat.modification_ts = entry->node->modification_ts;
            vnode->stat.access_ts = entry->node->access_ts;
            vnode->_private = entry->node;

#if 0
            serial::printf("ramfs> lookup for '%s' resolved to a ram node: 0x%llx\n", name, entry->node);
#endif
            return vnode;
        }
    }

    return vfs_null_node; // Not found
}

int ram_filesystem::ramfs_create(
    vfs_node* parent,
    const char* name,
    vfs_node_type type,
    uint32_t perms
) {
    auto fs = static_cast<ram_filesystem*>(parent->fs);
    mutex_guard guard(fs->m_fs_lock);

    auto ram_node = static_cast<ramfs_node*>(parent->_private);
    if (ram_node->type != vfs_node_type::directory) {
        return -1; // Not a directory
    }

    auto new_node = new ramfs_node();
    new_node->name = name;
    new_node->type = type;
    new_node->permissions = perms;
    new_node->creation_ts = kernel_timer::get_system_time_in_milliseconds();
    new_node->modification_ts = new_node->creation_ts;
    new_node->access_ts = new_node->creation_ts;
    new_node->data = nullptr;
    new_node->data_size = 0;

#if 0
    serial::printf("ramfs> created a new ram node '%s' (0x%llx)\n", name, new_node);
#endif

    // Create a new directory entry
    auto new_direntry = new ramfs_direntry();
    new_direntry->name = name;
    new_direntry->node = new_node;

    // Add the directory entry to the parent ram node
    ram_node->children.push_back(new_direntry);
    return 0; // Success
}

int ram_filesystem::ramfs_remove(vfs_node* parent, vfs_node* node) {
    // Validate the input parent and node
    if (!parent || !node) {
        return make_error_code(fs_error::invalid_argument);
    }

    auto fs = static_cast<ram_filesystem*>(node->fs);
    mutex_guard guard(fs->m_fs_lock);

    auto parent_ram_node = static_cast<ramfs_node*>(parent->_private);
    auto target_ram_node = static_cast<ramfs_node*>(node->_private);

    // Ensure the parent is a directory
    if (parent_ram_node->type != vfs_node_type::directory) {
        return make_error_code(fs_error::not_directory);
    }

    // Find the target node in the parent's children
    for (size_t i = 0; i < parent_ram_node->children.size(); ++i) {
        auto direntry = parent_ram_node->children[i];
        if (direntry->node != target_ram_node) {
            continue;
        }

#if 0
        serial::printf(
            "ramfs> deleting node '%s' from parent '%s'\n",
            target_ram_node->name.c_str(),
            parent_ram_node->name.c_str()
        );
#endif

        // Remove the directory entry from the parent's children
        parent_ram_node->children.erase(i);

        // Recursively delete the target node
        if (target_ram_node->type == vfs_node_type::directory) {
            _delete_ram_directory(target_ram_node);
        } else {
            _delete_ram_file(target_ram_node);
        }

        // Delete the directory entry itself
        delete direntry;

        return make_error_code(fs_error::success);
    }


    return make_error_code(fs_error::not_found); // Node not found in parent's children
}

int ram_filesystem::ramfs_listdir(vfs_node* node, kstl::vector<kstl::string>& entries) {
    if (!node) {
        return make_error_code(fs_error::invalid_argument);
    }

    auto fs = static_cast<ram_filesystem*>(node->fs);
    mutex_guard guard(fs->m_fs_lock);

    auto ram_node = static_cast<ramfs_node*>(node->_private);

    // Ensure the node is a directory
    if (ram_node->type != vfs_node_type::directory) {
        return make_error_code(fs_error::not_directory);
    }

    // Populate the entries with the names of the children
    for (ramfs_direntry* entry : ram_node->children) {
        entries.push_back(entry->name);
    }

    return make_error_code(fs_error::success);
}


void ram_filesystem::_delete_ram_file(ramfs_node* file_node) {
    if (!file_node || file_node->type != vfs_node_type::file) {
        return;
    }

#if 0
    serial::printf("ramfs> deleting file '%s'\n", file_node->name.c_str());
#endif

    // Free the file data and delete the node
    delete[] file_node->data;
    file_node->data = nullptr;
    file_node->data_size = 0;
    delete file_node;
}

void ram_filesystem::_delete_ram_directory(ramfs_node* dir_node) {
    if (!dir_node || dir_node->type != vfs_node_type::directory) {
        return; // Not a valid directory node
    }

#if 0
    serial::printf("ramfs> deleting directory '%s'\n", dir_node->name.c_str());
#endif

    // Recursively delete each child
    for (ramfs_direntry* direntry : dir_node->children) {
        if (direntry->node->type == vfs_node_type::directory) {
            _delete_ram_directory(direntry->node);
        } else {
            _delete_ram_file(direntry->node);
        }

        // Delete the directory entry
        delete direntry;
    }

    // Clear the children vector and delete the directory itself
    dir_node->children.clear();
    delete dir_node;
}
} // namespace fs
