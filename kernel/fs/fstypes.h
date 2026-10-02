#ifndef STELLUX_FS_FSTYPES_H
#define STELLUX_FS_FSTYPES_H

#include "common/types.h"

namespace fs {

enum class node_type : uint32_t {
    regular,
    directory,
    symlink,
    char_device,
    block_device,
    socket
};

constexpr size_t NAME_MAX = 255;
constexpr size_t PATH_MAX = 4096;
constexpr uint32_t SYMLOOP_MAX = 40;

constexpr uint32_t O_RDONLY = 0;
constexpr uint32_t O_WRONLY = 1;
constexpr uint32_t O_RDWR   = 2;
constexpr uint32_t O_CREAT  = 0x40;
constexpr uint32_t O_EXCL   = 0x80;
constexpr uint32_t O_TRUNC  = 0x200;
constexpr uint32_t O_APPEND   = 0x400;
constexpr uint32_t O_NONBLOCK = 0x800;
constexpr uint32_t O_CLOEXEC  = 0x80000;

constexpr uint32_t ACCESS_MODE_MASK = 0x3;

// The file status flags, which F_SETFL may change after open
constexpr uint32_t STATUS_FLAG_MASK = O_APPEND | O_NONBLOCK;

constexpr int32_t SEEK_SET = 0;
constexpr int32_t SEEK_CUR = 1;
constexpr int32_t SEEK_END = 2;

// The largest offset a file position can hold, the maximum of the signed 64-bit off_t
constexpr int64_t MAX_FILE_OFFSET = 0x7FFFFFFFFFFFFFFF;

struct vattr {
    node_type type;
    uint32_t mode;      // Permission bits, within MODE_PERMISSION_BITS
    size_t size;
    uint64_t ino;       // Unique among all nodes for the node's lifetime, never 0
    uint64_t dev;       // Identifies the mounted filesystem instance, 0 if unmounted
    uint64_t atime_ns;  // Last access, Unix epoch nanoseconds, reads do not update it
    uint64_t mtime_ns;  // Last content change
    uint64_t ctime_ns;  // Last content or attribute change, never set by callers
};

// setattr mask bits naming the vattr fields to apply
constexpr uint32_t VATTR_ATIME = 1u << 0;
constexpr uint32_t VATTR_MTIME = 1u << 1;
constexpr uint32_t VATTR_MODE  = 1u << 2;

// The permission, set-id, and sticky bits of a mode
constexpr uint32_t MODE_PERMISSION_BITS = 07777;

// Modes of the files and directories the kernel creates without a request
constexpr uint32_t DEFAULT_FILE_MODE      = 0644;
constexpr uint32_t DEFAULT_DIRECTORY_MODE = 0755;

struct dirent {
    char name[NAME_MAX + 1];
    node_type type;
    uint64_t ino;
};

} // namespace fs

#endif // STELLUX_FS_FSTYPES_H
