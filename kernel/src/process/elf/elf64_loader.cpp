#include <process/elf/elf64_loader.h>
#include <memory/memory.h>
#include <memory/vmm.h>
#include <memory/paging.h>
#include <fs/vfs.h>
#include <serial/serial.h>

// Uncomment to see verbose loader logs
// #define ELF64_LOADER_ENABLE_LOGS 1

namespace elf {
__PRIVILEGED_CODE
task_control_block* elf64_loader::load_elf(const uint8_t* file_buffer, size_t buffer_size) {
    if (!file_buffer || buffer_size < sizeof(elf64_ehdr)) {
        _log_error("Invalid file buffer or buffer size.");
        return nullptr;
    }

    // Parse ELF Header
    const auto& elf_header = *reinterpret_cast<const elf64_ehdr*>(file_buffer);

    // Validate ELF Header
    if (!_validate_elf_header(elf_header)) {
        _log_error("Invalid ELF header.");
        return nullptr;
    }

    // Create a new page table for the process
    paging::page_table* page_table = paging::create_higher_class_userland_page_table();
    if (!page_table) {
        _log_error("Failed to create userland page table.");
        return nullptr;
    }

    // Load ELF Segments
    if (!_load_segments(file_buffer, elf_header, page_table)) {
        _log_error("Failed to load ELF segments.");
        return nullptr;
    }

    // Create a userland task with the entry point and page table
    task_control_block* task = sched::create_upper_class_userland_task(elf_header.e_entry, page_table);
    if (!task) {
        _log_error("Failed to create userland task.");
        return nullptr;
    }

    _log_info("ELF loaded successfully, task created.");
    return task;
}

__PRIVILEGED_CODE
task_control_block* elf64_loader::load_from_file(const char* filepath) {
    if (!filepath) {
        _log_error("Invalid file path.");
        return nullptr;
    }

    // Read file into memory
    size_t file_size = 0;
    uint8_t* file_buffer = _read_file(filepath, file_size);
    if (!file_buffer) {
        _log_error("Failed to read file.");
        return nullptr;
    }

    // Create ELF loader instance and load ELF file
    auto loader = elf::elf64_loader();
    task_control_block* task = loader.load_elf(file_buffer, file_size);
    if (!task) {
        _log_error("Failed to load ELF file.");
        return nullptr;
    }

    // Set the task's name
    auto name = fs::virtual_filesystem::get_filename_from_path(filepath);
    memcpy(task->name, name.c_str(), kstl::max(name.length(), sizeof(task->name) - 1));

    return task;
}

bool elf64_loader::_validate_elf_header(const elf64_ehdr& header) {
    // Check magic numbers
    if (header.e_ident[EI_MAG0] != ELF_MAGIC0 ||
        header.e_ident[EI_MAG1] != ELF_MAGIC1 ||
        header.e_ident[EI_MAG2] != ELF_MAGIC2 ||
        header.e_ident[EI_MAG3] != ELF_MAGIC3) {
        _log_info("Invalid ELF magic number.");
        return false;
    }

    // Check class (ELF64)
    if (header.e_ident[EI_CLASS] != ELFCLASS64) {
        _log_info("Unsupported ELF class (not ELF64).");
        return false;
    }

    // Check data encoding (little-endian)
    if (header.e_ident[EI_DATA] != ELFDATA2LSB) {
        _log_info("Unsupported ELF data encoding (not little-endian).");
        return false;
    }

    // Check type (Executable)
    if (header.e_type != ET_EXEC) {
        _log_info("Unsupported ELF type (not executable).");
        return false;
    }

    // Check machine type (x86-64)
    if (header.e_machine != EM_X86_64) {
        _log_info("Unsupported machine type (not x86-64).");
        return false;
    }

    // Check version
    if (header.e_version != 1) {
        _log_info("Unsupported ELF version.");
        return false;
    }

    _log_info("ELF header validation passed.");
    return true;
}

__PRIVILEGED_CODE
bool elf64_loader::_load_segments(
    const uint8_t* file_buffer,
    const elf64_ehdr& header,
    paging::page_table* page_table
) {
    const auto* program_headers = reinterpret_cast<const elf64_phdr*>(file_buffer + header.e_phoff);

    for (uint16_t i = 0; i < header.e_phnum; ++i) {
        const auto& phdr = program_headers[i];

        // Skip non-loadable segments
        if (phdr.p_type != PT_LOAD) {
            continue;
        }

        _log_info("Loading segment...");
#ifdef ELF64_LOADER_ENABLE_LOGS
        serial::printf("  Virtual Address: 0x%llx\n", phdr.p_vaddr);
        serial::printf("  File Size: 0x%llx\n", phdr.p_filesz);
        serial::printf("  Memory Size: 0x%llx\n", phdr.p_memsz);
#endif

        // Allocate and map the segment
        if (!_allocate_and_map_segment(
                phdr.p_vaddr,
                phdr.p_offset,
                phdr.p_filesz,
                phdr.p_memsz,
                phdr.p_flags,
                file_buffer,
                page_table
        )) {
            _log_error("Failed to allocate and map segment.");
            return false;
        }
    }

    _log_info("All segments loaded successfully.");
    return true;
}

__PRIVILEGED_CODE
void* elf64_loader::_allocate_and_map_segment(
    uint64_t vaddr,
    uint64_t offset,
    uint64_t filesz,
    uint64_t memsz,
    uint32_t flags,
    const uint8_t* file_buffer,
    paging::page_table* page_table
) 
{
    // Align the virtual address and memory size to page boundaries
    uint64_t aligned_vaddr_start = PAGE_ALIGN_DOWN(vaddr);
    uint64_t aligned_vaddr_end = PAGE_ALIGN_UP(vaddr + memsz);
    uint64_t aligned_size = aligned_vaddr_end - aligned_vaddr_start;

    // Calculate the number of pages needed
    size_t num_pages = aligned_size / PAGE_SIZE;

    // Allocate physical memory for the segment
    auto& physalloc = allocators::page_bitmap_allocator::get_physical_allocator();
    void* physical_memory = physalloc.alloc_pages(num_pages);
    if (!physical_memory) {
        _log_error("Failed to allocate physical pages for segment.");
        return nullptr;
    }
    
    // Determine segment memory flags
    uint64_t page_flags = PTE_PRESENT | PTE_US;
    if (!(flags & PF_X)) {
        page_flags |= PTE_NX;
    }
    if (flags & PF_W) {
        page_flags |= PTE_RW;
    }

    // Map physical pages to virtual address space
    paging::map_pages(
        aligned_vaddr_start,
        reinterpret_cast<uintptr_t>(physical_memory),
        num_pages,
        page_flags,
        page_table
    );

    // Copy data from the ELF file to the allocated memory
    void* phys_memory_mapped_vaddr = paging::phys_to_virt_linear(physical_memory);
    void* dest_memory = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(phys_memory_mapped_vaddr) + (vaddr % PAGE_SIZE)
    );
    const void* src_memory = file_buffer + offset;
    memcpy(dest_memory, src_memory, filesz);

    // Zero out the rest of the memory (if memsz > filesz)
    if (memsz > filesz) {
        void* bss_start = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(dest_memory) + filesz);
        memset(bss_start, 0, memsz - filesz);
    }

    _log_info("Segment allocated and mapped successfully.");
    return physical_memory;
}

uint8_t* elf64_loader::_read_file(const char* file_path, size_t& file_size) {
    auto& vfs = fs::virtual_filesystem::get();
    fs::vfs_stat_struct stat;

    // Get file stats
    if (vfs.stat(file_path, stat) != fs::fs_error::success) {
        _log_error("Failed to stat file.");
        return nullptr;
    }

    file_size = stat.size;
    if (file_size == 0) {
        _log_error("File is empty.");
        return nullptr;
    }

    // Allocate memory for file buffer
    uint8_t* buffer = reinterpret_cast<uint8_t*>(zmalloc(file_size));
    if (!buffer) {
        _log_error("Failed to allocate memory for file.");
        return nullptr;
    }

    // Read file into buffer
    if (!vfs.read(file_path, buffer, file_size, 0)) {
        _log_error("Failed to read file into buffer.");
        return nullptr;
    }

#ifdef ELF64_LOADER_ENABLE_LOGS
    serial::printf("File read successfully: %s (%llu bytes)\n", file_path, file_size);
#endif
    return buffer;
}

void elf64_loader::_log_error(const char* message) {
    // Unconditionally print the error message
    serial::printf("[ELF64 Loader] ERROR: %s\n", message);
}

void elf64_loader::_log_info(const char* message) {
#ifdef ELF64_LOADER_ENABLE_LOGS
    serial::printf("[ELF64 Loader] INFO: %s\n", message);
#else
    __unused message;
#endif
}
} // namespace elf
