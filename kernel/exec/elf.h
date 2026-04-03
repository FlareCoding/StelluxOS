#ifndef STELLUX_EXEC_ELF_H
#define STELLUX_EXEC_ELF_H

#include "common/types.h"

namespace mm { struct mm_context; }
namespace fs { class node; }

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
    uint64_t    e_phoff;
    uint16_t    phentsize;
    uint16_t    phnum;
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
constexpr int32_t ERR_PT_CREATE         = -13;
constexpr int32_t ERR_PAGE_ALLOC        = -14;
constexpr int32_t ERR_PAGE_MAP          = -15;

struct loaded_image {
    uint64_t entry_point;
    uint64_t pt_root; // convenience mirror of mm_ctx->pt_root
    mm::mm_context* mm_ctx; // owning reference, transferred to task on success
    uint32_t segment_count;
    uint64_t phdr_vaddr;
    uint16_t phentsize;
    uint16_t phnum;
};

/**
 * Parse an ELF64 binary from a memory buffer.
 * Validates headers and extracts PT_LOAD segments into out.
 */
int32_t parse_elf(const void* buffer, size_t size, elf_image* out);

/**
 * Parse an ELF64 binary from a file path.
 * Opens the file, reads it into a temporary buffer, parses, and cleans up.
 * If base_dir is non-null, path is resolved relative to it.
 */
int32_t parse_elf(const char* path, elf_image* out, fs::node* base_dir = nullptr);

/**
 * Load an ELF64 binary from a memory buffer into a new user address space.
 * Creates a user page table and maps all PT_LOAD segments with correct permissions.
 * Elevates internally for privileged operations (PMM, paging).
 */
int32_t load_elf(const void* buffer, size_t size, loaded_image* out);

/**
 * Load an ELF64 binary from a file path into a new user address space.
 * Elevates internally for privileged operations (PMM, paging).
 * If base_dir is non-null, path is resolved relative to it.
 */
int32_t load_elf(const char* path, loaded_image* out, fs::node* base_dir = nullptr);

/**
 * Unload a previously loaded ELF image.
 * Unmaps and frees all user-space pages, then destroys the page table.
 * Elevates internally for privileged operations.
 */
void unload_elf(loaded_image* img);

} // namespace exec

#endif // STELLUX_EXEC_ELF_H
