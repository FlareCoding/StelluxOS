#ifndef STELLUX_EXEC_ELF_H
#define STELLUX_EXEC_ELF_H

#include "common/types.h"

namespace exec {

struct elf_segment {
    uint64_t vaddr;
    uint64_t offset;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
    uint32_t flags;
};

constexpr uint32_t MAX_ELF_SEGMENTS = 16;

struct elf_image {
    uint64_t    entry_point;
    uint32_t    segment_count;
    elf_segment segments[MAX_ELF_SEGMENTS];
};

constexpr int32_t OK                    =  0;
constexpr int32_t ERR_INVALID_MAGIC     = -1;
constexpr int32_t ERR_INVALID_CLASS     = -2;
constexpr int32_t ERR_INVALID_DATA      = -3;
constexpr int32_t ERR_INVALID_VERSION   = -4;
constexpr int32_t ERR_INVALID_TYPE      = -5;
constexpr int32_t ERR_INVALID_ARCH      = -6;
constexpr int32_t ERR_INVALID_PHDR      = -7;
constexpr int32_t ERR_TOO_MANY_SEGMENTS = -8;
constexpr int32_t ERR_NO_LOADABLE       = -9;
constexpr int32_t ERR_FILE_OPEN         = -10;
constexpr int32_t ERR_FILE_READ         = -11;
constexpr int32_t ERR_NO_MEM            = -12;

/**
 * Parse an ELF64 binary from a memory buffer.
 * Validates headers and extracts PT_LOAD segments into out.
 */
int32_t parse_elf(const void* buffer, size_t size, elf_image* out);

/**
 * Parse an ELF64 binary from a file path.
 * Opens the file, reads it into a temporary buffer, parses, and cleans up.
 */
int32_t parse_elf(const char* path, elf_image* out);

} // namespace exec

#endif // STELLUX_EXEC_ELF_H
