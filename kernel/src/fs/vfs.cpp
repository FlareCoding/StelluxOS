#include <fs/vfs.h>
#include <serial/serial.h>

namespace fs {
virtual_filesystem g_global_vfs = virtual_filesystem();

virtual_filesystem& virtual_filesystem::get() {
    return g_global_vfs;
}

fs_error virtual_filesystem::mount(
    const kstl::string& path,
    const kstl::shared_ptr<filesystem>& fs
) {
    mutex_guard guard(m_vfs_lock);

    if (!fs) {
        serial::printf("[*] Attempting to mount a nullptr filesystem at '%s'\n", path.c_str());
        return fs_error::bad_filesystem;
    }

    auto root_node = fs->create_root_node();

    if (!root_node) {
        serial::printf("[*] Failed to create root node for filesystem at '%s'\n", path.c_str());
        return fs_error::io_error;
    }

    // Update the ops struct and owner filesystem reference
    root_node->fs = fs.get();
    fs->set_ops(root_node, path);

    m_mount_points.push_back(mount_point {
        .path = path,
        .root_node = root_node,
        .owner_fs = fs
    });

    return fs_error::success;
}

fs_error virtual_filesystem::unmount(
    const kstl::string& path
) {
    mutex_guard guard(m_vfs_lock);

    if (path.empty()) {
        return fs_error::invalid_argument;
    }

    for (size_t i = 0; i < m_mount_points.size(); ++i) {
        auto& mnt = m_mount_points[i];
        if (mnt.path == path) {
            // Call the filesystem's specific unmount hook
            mnt.owner_fs->unmount();

            // Remove the mount point
            m_mount_points.erase(i);

            return fs_error::success;
        }
    }

    // Mount point was not found
    return fs_error::invalid_path;
}

fs_error virtual_filesystem::create(
    const kstl::string& path,
    fs::vfs_node_type type,
    uint32_t perms
) {
    // Validate the input path
    if (path.empty()) {
        return fs_error::invalid_path;
    }

    // Split the path into components
    kstl::vector<kstl::string> components;
    _split_path(path, components);

    if (components.empty() || components.back() == "/") {
        return fs_error::invalid_path;
    }

    // Extract the name of the file or directory to create
    kstl::string name = components.back();
    components.pop_back();

    // Resolve the parent directory
    kstl::shared_ptr<vfs_node> parent_node;
    kstl::string parent_path = components.front() == "/" ? "/" : "";

    // Build out a parent directory path
    for (size_t i = (components.front() == "/" ? 1 : 0); i < components.size(); ++i) {
        if (parent_path != "/") {
            parent_path += "/";
        }
        parent_path += components[i];
    }

    fs_error result = _resolve_path(parent_path, parent_node);
    if (result != fs_error::success) {
        return result;
    }

    // Ensure the parent node is a directory
    if (parent_node->stat.type != fs::vfs_node_type::directory) {
        return fs_error::not_directory;
    }

    // Ensure the parent node has a valid create operation
    if (!parent_node->ops.create) {
        return fs_error::unsupported_operation;
    }

    // Check if the path already exists
    if (!parent_node->ops.lookup) {
        return fs_error::unsupported_operation;
    }

    kstl::shared_ptr<vfs_node> target_node = parent_node->ops.lookup(parent_node.get(), name.c_str());
    if (target_node) {
        return fs_error::already_exists;
    }

    // Call the create operation
    int status = parent_node->ops.create(parent_node.get(), name.c_str(), type, perms);
    
    return (status == 0) ? fs_error::success : fs_error::io_error;
}

fs_error virtual_filesystem::remove(
    const kstl::string& path
) {
    // Validate the input path
    if (path.empty()) {
        return fs_error::invalid_path;
    }

    // Split the path into components
    kstl::vector<kstl::string> components;
    _split_path(path, components);

    if (components.empty() || components.back() == "/") {
        return fs_error::invalid_path;
    }

    // Extract the name of the target node to remove
    kstl::string name = components.back();
    components.pop_back();

    // Resolve the parent directory
    kstl::shared_ptr<vfs_node> parent_node;
    kstl::string parent_path = components.front() == "/" ? "/" : "";

    // Build the parent directory path
    for (size_t i = (components.front() == "/" ? 1 : 0); i < components.size(); ++i) {
        if (parent_path != "/") {
            parent_path += "/";
        }
        parent_path += components[i];
    }

    fs_error result = _resolve_path(parent_path, parent_node);
    if (result != fs_error::success) {
        return result;
    }

    // Ensure the parent node is a directory
    if (parent_node->stat.type != fs::vfs_node_type::directory) {
        return fs_error::not_directory;
    }

    // Ensure the parent node has a valid remove operation
    if (!parent_node->ops.remove) {
        return fs_error::unsupported_operation;
    }

    // Resolve the target node
    kstl::shared_ptr<vfs_node> target_node;
    result = _resolve_path(path, target_node);
    if (result != fs_error::success) {
        return result;
    }

    // Call the remove operation
    int status = parent_node->ops.remove(parent_node.get(), target_node.get());
    return (status == 0) ? fs_error::success : fs_error::io_error;
}

ssize_t virtual_filesystem::read(
    const kstl::string& path,
    void* buffer,
    size_t size,
    uint64_t offset
) {
    // Validate the input path
    if (path.empty() || !buffer) {
        return make_error_code(fs_error::invalid_argument);
    }

    // Resolve the node for the provided path
    kstl::shared_ptr<fs::vfs_node> resolved_node;
    fs_error result = _resolve_path(path, resolved_node);

    if (result != fs_error::success) {
        return make_error_code(result);
    }

    // Ensure the resolved node is a file
    if (resolved_node->stat.type != fs::vfs_node_type::file) {
        return make_error_code(fs_error::not_a_file);
    }

    // Ensure the node has a valid read operation
    if (!resolved_node->ops.read) {
        return make_error_code(fs_error::unsupported_operation);
    }

    // Call the read operation
    ssize_t bytes_read = resolved_node->ops.read(resolved_node.get(), buffer, size, offset);

    // Return the number of bytes read or an error code
    return bytes_read;
}

ssize_t virtual_filesystem::write(
    const kstl::string& path,
    const void* buffer,
    size_t size,
    uint64_t offset
) {
    // Validate input arguments
    if (path.empty() || !buffer) {
        return make_error_code(fs_error::invalid_argument);
    }

    // Resolve the node for the provided path
    kstl::shared_ptr<fs::vfs_node> resolved_node;
    fs_error result = _resolve_path(path, resolved_node);

    if (result != fs_error::success) {
        return make_error_code(result);
    }

    // Ensure the resolved node is a file
    if (resolved_node->stat.type != fs::vfs_node_type::file) {
        return make_error_code(fs_error::not_a_file);
    }

    // Ensure the node has a valid write operation
    if (!resolved_node->ops.write) {
        return make_error_code(fs_error::unsupported_operation);
    }

    // Call the write operation
    ssize_t bytes_written = resolved_node->ops.write(resolved_node.get(), buffer, size, offset);

    // Return the number of bytes written or an error code
    return bytes_written;
}

fs_error virtual_filesystem::listdir(const kstl::string& path, kstl::vector<kstl::string>& entries) {
    // Validate the input path
    if (path.empty()) {
        return fs_error::invalid_path;
    }

    // Resolve the node for the provided path
    kstl::shared_ptr<vfs_node> resolved_node;
    fs_error result = _resolve_path(path, resolved_node);

    if (result != fs_error::success) {
        return result; // Path could not be resolved
    }

    // Ensure the resolved node is a directory
    if (resolved_node->stat.type != vfs_node_type::directory) {
        return fs_error::not_directory;
    }

    // Ensure the node has a valid listdir operation
    if (!resolved_node->ops.listdir) {
        return fs_error::unsupported_operation;
    }

    // Call the listdir operation
    int status = resolved_node->ops.listdir(resolved_node.get(), entries);
    
    return static_cast<fs_error>(status);
}

fs_error virtual_filesystem::stat(const kstl::string& path, vfs_stat_struct& info) {
    // Validate the input path
    if (path.empty()) {
        return fs_error::invalid_path;
    }

    // Resolve the node for the provided path
    kstl::shared_ptr<vfs_node> resolved_node;
    fs_error result = _resolve_path(path, resolved_node);

    if (result != fs_error::success) {
        return result; // Path could not be resolved
    }

    // Copy the stat information from the resolved
    // node to the info struct provided by reference.
    info = resolved_node->stat;
    return fs_error::success;
}

bool virtual_filesystem::path_exists(const kstl::string& path) {
    kstl::shared_ptr<fs::vfs_node> resolved_node;
    return _resolve_path(path, resolved_node) == fs_error::success;
}

kstl::string virtual_filesystem::get_filename_from_path(const kstl::string& path) {
    if (path.empty()) {
        return ""; // Return an empty string if the path is empty.
    }

    // Vector to hold the path components
    kstl::vector<kstl::string> components;

    // Split the path into components
    _split_path(path, components);

    // Return the last component, which represents the filename, or empty if no components
    return components.empty() ? "" : components.back();
}

fs_error virtual_filesystem::is_mount_point(
    const kstl::string& path,
    kstl::shared_ptr<vfs_node>& out_root_node
) {
    for (auto& mnt : m_mount_points) {
        if (mnt.path == path) {
            out_root_node = mnt.root_node;
            return fs_error::success;
        }
    }

    return fs_error::not_found;
}

fs_error virtual_filesystem::_resolve_path(
    const kstl::string& path,
    kstl::shared_ptr<vfs_node>& out_node
) {
    mutex_guard guard(m_vfs_lock);

    // Validate the input path
    if (path.empty()) {
        return fs_error::invalid_path;
    }

    // Split the path into components
    kstl::vector<kstl::string> components;
    _split_path(path, components);

    if (components.empty()) {
        return fs_error::invalid_path;
    }

    // Determine the starting root node
    kstl::shared_ptr<vfs_node> current_node;
    if ((is_mount_point(components.front(), current_node)) != fs_error::success) {
        return fs_error::invalid_path;
    }

    // Keep track of the current filesystem
    filesystem* current_fs = current_node->fs;

    // Build the current path dynamically as we iterate
    kstl::string current_path = components.front() == "/" ? "/" : "";

    // Iterate through the components
    for (size_t i = (components.front() == "/" ? 1 : 0); i < components.size(); ++i) {
        const kstl::string& component = components[i];

        // Update the current path
        if (current_path != "/") {
            current_path += "/";
        }
        current_path += component;

        // Check if current path matches a mount point
        kstl::shared_ptr<vfs_node> mount_node;
        if (is_mount_point(current_path, mount_node) == fs_error::success) {
            current_node = mount_node;
#if 0
            serial::printf("switching to a new filesystem's root node: %llx\n", current_node.get());
#endif
            continue; // Switch to the new filesystem's root node
        }

        // Ensure the current node has a valid lookup operation
        if (!current_node->ops.lookup) {
            return fs_error::unsupported_operation;
        }

        // Perform the lookup for the next component
        current_node = current_node->ops.lookup(current_node.get(), component.c_str());
        
        // If lookup fails, return an error
        if (!current_node) {
            return fs_error::not_found;
        }

        // Since the node was most likely newly created, we need
        // to update its ops struct as well as the fs reference.
        current_node->fs = current_fs;
        current_fs->set_ops(current_node, current_path);
    }

    // If we resolved all components successfully, set the output node
    out_node = current_node;
    return fs_error::success;
}

void virtual_filesystem::_split_path(
    const kstl::string& path,
    kstl::vector<kstl::string>& components
) {
    size_t start = 0;
    size_t end = 0;

    // Clear the components vector to ensure it's empty before processing
    components.clear();

    // If the path starts with '/', add "/" as the first component
    if (!path.empty() && path[0] == '/') {
        components.push_back("/");
        start = 1; // Skip the leading '/'
    }

    // Iterate through the path string
    while (end < path.length()) {
        // Find the next slash
        end = path.find('/', start);

        // If no more slashes, set end to the end of the string
        if (end == kstl::string::npos) {
            end = path.length();
        }

        // Extract the substring between start and end
        kstl::string component = path.substring(start, end - start);

        // If the component is non-empty, add it to the components vector
        if (!component.empty()) {
            components.push_back(component);
        }

        // Move start to the character after the current slash
        start = end + 1;
    }
}
} // namespace fs
