#ifndef ELF64_LOADER_H
#define ELF64_LOADER_H
#include "elf_types.h"
#include <process/process.h>

namespace elf {
class elf64_loader {
public:
    __PRIVILEGED_CODE static task_control_block* load_elf(
        const uint8_t* file_buffer,
        size_t buffer_size
    );

    __PRIVILEGED_CODE static task_control_block* load_from_file(
        const char* filepath
    );

private:
    static bool _validate_elf_header(const elf64_ehdr& header);

    __PRIVILEGED_CODE static bool _load_segments(
        const uint8_t* file_buffer,
        const elf64_ehdr& header,
        paging::page_table* page_table
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
