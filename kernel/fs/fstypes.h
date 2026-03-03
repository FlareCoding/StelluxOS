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

constexpr uint32_t ACCESS_MODE_MASK = 0x3;

constexpr int32_t SEEK_SET = 0;
constexpr int32_t SEEK_CUR = 1;
constexpr int32_t SEEK_END = 2;

struct vattr {
    node_type type;
    size_t size;
};

struct dirent {
    char name[NAME_MAX + 1];
    node_type type;
};

} // namespace fs

#endif // STELLUX_FS_FSTYPES_H
