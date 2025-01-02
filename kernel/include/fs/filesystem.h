#ifndef FILESYSTEM_H
#define FILESYSTEM_H
#include "vfs_node.h"
#include <sync.h>

namespace fs {
/**
 * @enum fs_error
 * @brief Represents error codes for filesystem operations.
 * 
 * This enumeration defines a set of error codes used to
 * identify various filesystem-related issues.
 */
enum class fs_error : ssize_t {
    success = 0,                 // No error
    not_found = -1,              // File or directory not found
    already_exists = -2,         // File or directory already exists
    permission_denied = -3,      // Insufficient permissions
    invalid_path = -4,           // Malformed or invalid path
    no_space_left = -5,          // No space left on device
    io_error = -6,               // Input/output error
    filesystem_full = -7,        // Filesystem is full
    not_directory = -8,          // Expected a directory but got a file
    is_directory = -9,           // Expected a file but got a directory
    unsupported_operation = -10, // Operation not supported
    bad_filesystem = -11,        // Filesystem related error
    invalid_argument = -12,      // Invalid argument to an operation
    not_a_file = -13,            // Not a file error
    unknown_error = -14          // Generic error
};

/**
 * @brief Converts an `fs_error` enumeration value to its corresponding error message string.
 * @param err The `fs_error` value to convert.
 * @return A string to the corresponding error message.
 * 
 * This function provides human-readable descriptions of filesystem error codes.
 */
inline const char* error_to_string(fs_error err) {
    switch (err) {
    case fs_error::success:              return "Success";
    case fs_error::not_found:            return "Not found";
    case fs_error::already_exists:       return "Already exists";
    case fs_error::permission_denied:    return "Permission denied";
    case fs_error::invalid_path:         return "Invalid path";
    case fs_error::no_space_left:        return "No space left";
    case fs_error::io_error:             return "I/O error";
    case fs_error::filesystem_full:      return "Filesystem full";
    case fs_error::not_directory:        return "Not a directory";
    case fs_error::is_directory:         return "Is a directory";
    case fs_error::unsupported_operation:return "Unsupported operation";
    case fs_error::bad_filesystem:       return "Bad filesystem";
    case fs_error::invalid_argument:     return "Invalid argument";
    case fs_error::not_a_file:           return "Not a file";
    default:                             return "Unknown error";
    }
}

/**
 * @brief Converts an error code of type `ssize_t` to its corresponding error message string.
 * @param err The error code to convert.
 * @return A string to the corresponding error message.
 * 
 * Internally casts the error code to `fs_error` and retrieves its message.
 */
inline const char* error_to_string(ssize_t err) {
    return error_to_string(static_cast<fs_error>(err));
}

/**
 * @brief Converts an `fs_error` enumeration value to its corresponding numeric error code.
 * @param err The `fs_error` value to convert.
 * @return The numeric representation of the error code as `ssize_t`.
 * 
 * Provides a way to retrieve the numeric value of an `fs_error` for use in functions or APIs
 * expecting standard error codes.
 */
constexpr ssize_t make_error_code(fs_error err) {
    return static_cast<ssize_t>(err);
}

class filesystem {
public:
    /**
     * @brief Virtual destructor for the filesystem.
     *
     * Ensures proper cleanup when a derived filesystem is destroyed.
     */
    virtual ~filesystem() = default;

    /**
     * @brief Creates the root `vfs_node` for the filesystem.
     *
     * This function is called by the VFS during the mount process. The derived
     * class is responsible for creating and initializing the root node of the
     * filesystem. VFS will then issue a `set_ops` call on the created node.
     *
     * @return kstl::shared_ptr<vfs_node> A shared pointer to the root node of the filesystem.
     */
    virtual kstl::shared_ptr<vfs_node> create_root_node() = 0;

    /**
     * @brief Hook to be called on filesystem unmount operation.
     */
    virtual void unmount() = 0;

    /**
     * @brief Sets the `vfs_operations` for a given `vfs_node`.
     *
     * This function is used to assign the appropriate operations to a node.
     * The `path` parameter provides the node's location within the filesystem,
     * allowing custom behaviors to be applied to specific nodes.
     *
     * Derived filesystems should implement this function to customize the node's
     * operations as needed.
     *
     * @param node A reference to the `vfs_node` whose operations are being set.
     * @param path The path of the node relative to the filesystem root.
     */
    virtual void set_ops(kstl::shared_ptr<vfs_node>& node, const kstl::string& path) = 0;
};
} // namespace fs

#endif // FILESYSTEM_H
