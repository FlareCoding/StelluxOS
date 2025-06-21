#ifndef FILE_OBJECT_H
#define FILE_OBJECT_H
#include "vfs.h"

// POSIX file open flags
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001  
#define O_RDWR      0x0002
#define O_CREAT     0x0040   // Create if doesn't exist
#define O_EXCL      0x0080   // Fail if exists (with O_CREAT)
#define O_TRUNC     0x0200   // Truncate to 0 length
#define O_APPEND    0x0400   // All writes go to end
#define O_NONBLOCK  0x0800   // Non-blocking I/O

// POSIX lseek whence values
#define SEEK_SET    0        // Seek from beginning of file
#define SEEK_CUR    1        // Seek from current position
#define SEEK_END    2        // Seek from end of file

namespace fs {
struct file_object {
    kstl::shared_ptr<vfs_node> vnode; // VFS node reference with lifetime management
    uint64_t position;                // Current file position
    uint32_t open_flags;              // O_RDONLY, O_WRONLY, O_RDWR, etc.
    uint32_t status_flags;            // O_APPEND, O_NONBLOCK, etc.

    // Access control
    bool can_read() const { return !(open_flags & O_WRONLY); }
    bool can_write() const { return (open_flags & O_WRONLY) || (open_flags & O_RDWR); }
};
} // namespace fs

#endif // FILE_OBJECT_H
