#ifndef ELF64_LOADER_H
#define ELF64_LOADER_H
#include "elf_types.h"
#include <process/process.h>
#include <process/vma.h>

namespace elf {
class elf64_loader {
public:
    /**
     * @brief Loads an ELF file from a memory buffer into a process core.
     * @param file_buffer Pointer to the ELF file data in memory.
     * @param buffer_size Size of the file buffer in bytes.
     * @return Pointer to the created process core, or nullptr if loading failed.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static process_core* load_elf(
        const uint8_t* file_buffer,
        size_t buffer_size
    );

    /**
     * @brief Loads an ELF file from disk into a process core.
     * @param filepath Path to the ELF file on disk.
     * @return Pointer to the created process core, or nullptr if loading failed.
     * 
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE static process_core* load_from_file(
        const char* filepath
    );

private:
    static bool _validate_elf_header(const elf64_ehdr& header);

    __PRIVILEGED_CODE static bool _load_segments(
        const uint8_t* file_buffer,
        const elf64_ehdr& header,
        paging::page_table* page_table,
        mm_context* mm_ctx
    );

    __PRIVILEGED_CODE static void _initialize_process_heap(
        const uint8_t* file_buffer,
        const elf64_ehdr& header,
        mm_context* mm_ctx
    );

    __PRIVILEGED_CODE static void* _allocate_and_map_segment(
        uint64_t vaddr,
        uint64_t offset,
        uint64_t filesz,
        uint64_t memsz,
        uint32_t flags,
        const uint8_t* file_buffer,
        paging::page_table* page_table
    );

    static uint8_t* _read_file(const char* file_path, size_t& file_size);

    static void _log_error(const char* message);
    static void _log_info(const char* message);
};
} // namespace elf

#endif // ELF64_LOADER_H
