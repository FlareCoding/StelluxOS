#ifndef VMA_H
#define VMA_H
#include <types.h>
#include <memory/paging.h>
#include <process/mm.h>

/**
 * StelluxOS Virtual Address Space Layout (x86-64)
 * 
 * This follows a simplified version of the Linux x86-64 memory layout:
 * 
 * 0x0000000000000000 - 0x0000000000000fff   : Null page (unmapped)
 * 0x0000000000001000 - 0x00000000003fffff   : Low memory (reserved)
 * 0x0000000000400000 - 0x0000000040000000   : ELF executable region
 * 0x0000000040000000 - 0x00007f0000000000   : Heap region (brk grows up starting after the loaded ELF segments)
 * 0x00007f0000000000 - 0x00007fffffffffff   : mmap region (grows up)
 * 0x00007fff00000000 - 0x00007fffffffffff   : Stack region (grows down from top)
 * 
 * Notes:
 * - ELF executables in Stellux are loaded at 0x400000
 * - Heap starts after ELF segments and grows upward with brk()
 * - mmap region starts at 0x7f0000000000 and grows upward
 * - Stack is at the very top and grows downward
 * - This leaves a large gap between heap and mmap for safety
 */

// Address space layout constants
constexpr uintptr_t USERSPACE_START         = 0x0000000000001000;   // Skip null page
constexpr uintptr_t USERSPACE_END           = 0x00007fffffffffff;   // Top of user space
constexpr uintptr_t ELF_REGION_START        = 0x0000000000400000;   // Typical ELF load address
constexpr uintptr_t ELF_REGION_END          = 0x0000000040000000;   // End of ELF region
constexpr uintptr_t HEAP_REGION_START       = 0x0000000040000000;   // Start of heap region
constexpr uintptr_t MMAP_REGION_START       = 0x00007f0000000000;   // Start of mmap region
constexpr uintptr_t STACK_REGION_START      = 0x00007fff00000000;   // Bottom/start of stack region, the actual stack beginning address at the top is 0x00007fffffffffff

// Protection flags for mmap() - standard Linux values
#define PROT_NONE    0x0  // Page may not be accessed
#define PROT_READ    0x1  // Page may be read
#define PROT_WRITE   0x2  // Page may be written
#define PROT_EXEC    0x4  // Page may be executed

// Mapping flags for mmap() - standard Linux values
#define MAP_SHARED      0x01   // Share changes with other processes
#define MAP_PRIVATE     0x02   // Changes are copy-on-write, private
#define MAP_FIXED       0x10   // Interpret addr exactly
#define MAP_ANONYMOUS   0x20   // Not backed by file (fd ignored)
#define MAP_LOCKED      0x2000 // Lock pages in memory
#define MAP_POPULATE    0x8000 // Populate (prefault) page tables

// Protection flags for VMA regions (same values as mmap PROT_* flags)
#define VMA_PROT_READ    PROT_READ
#define VMA_PROT_WRITE   PROT_WRITE
#define VMA_PROT_EXEC    PROT_EXEC

// VMA type flags
#define VMA_TYPE_PRIVATE    0x1  // Private mapping
#define VMA_TYPE_SHARED     0x2  // Shared mapping
#define VMA_TYPE_ANONYMOUS  0x4  // Not backed by a file
#define VMA_TYPE_FILE       0x8  // Backed by a file

/**
 * @brief Represents a Virtual Memory Area (VMA) in a process's address space.
 */
struct vma_area {
    uintptr_t start;          // Start address of the region
    uintptr_t end;            // End address of the region
    uint64_t flags;           // Protection flags (read, write, execute)
    uint64_t type;            // Type (private, shared, anonymous, file-backed)
    uint64_t file_offset;     // Offset in file for file-backed mappings
    void* file_backing;       // Pointer to file backing (if any)
    vma_area* next;           // Next VMA in the list
    vma_area* prev;           // Previous VMA in the list
};

/**
 * @brief Initializes VMA management for a process.
 * 
 * @param mm_ctx The process's memory management context
 * @return true if initialization was successful, false otherwise
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool init_process_vma(mm_context* mm_ctx);

/**
 * @brief Finds a suitable address range for a new mapping.
 * 
 * @param mm_ctx The process's memory management context
 * @param size Size of the requested mapping
 * @param flags Protection flags for the mapping
 * @param preferred_addr Preferred address (if any)
 * @return uintptr_t The start address of the suitable range, or 0 if none found
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uintptr_t find_free_vma_range(
    mm_context* mm_ctx,
    size_t size,
    uint64_t flags,
    uintptr_t preferred_addr = 0
);

/**
 * @brief Creates a new VMA in the process's address space.
 * 
 * @param mm_ctx The process's memory management context
 * @param start Start address of the region
 * @param size Size of the region
 * @param flags Protection flags
 * @param type VMA type flags
 * @param file_backing Optional file backing
 * @param file_offset Offset in file for file-backed mappings
 * @return vma_area* Pointer to the created VMA, or nullptr if creation failed
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE vma_area* create_vma(
    mm_context* mm_ctx,
    uintptr_t start,
    size_t size,
    uint64_t flags,
    uint64_t type,
    void* file_backing = nullptr,
    uint64_t file_offset = 0
);

/**
 * @brief Removes a VMA from the process's address space.
 * 
 * @param mm_ctx The process's memory management context
 * @param vma Pointer to the VMA to remove
 * @return true if removal was successful, false otherwise
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool remove_vma(mm_context* mm_ctx, vma_area* vma);

/**
 * @brief Finds a VMA containing the specified address.
 * 
 * @param mm_ctx The process's memory management context
 * @param addr Address to look up
 * @return vma_area* Pointer to the containing VMA, or nullptr if not found
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE vma_area* find_vma(mm_context* mm_ctx, uintptr_t addr);

/**
 * @brief Checks if a VMA with the given flags exists at the specified address.
 * 
 * @param mm_ctx The process's memory management context
 * @param addr Address to check
 * @param flags Required protection flags
 * @return true if a VMA with the required flags exists at the address
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool check_vma_flags(mm_context* mm_ctx, uintptr_t addr, uint64_t flags);

/**
 * @brief Merges adjacent VMAs if they have the same flags and type.
 * 
 * @param mm_ctx The process's memory management context
 * @param vma Pointer to the VMA to merge with its neighbors
 * @return true if merge was successful, false otherwise
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool merge_vmas(mm_context* mm_ctx, vma_area* vma);

/**
 * @brief Splits a VMA at the specified address.
 * 
 * @param mm_ctx The process's memory management context
 * @param vma Pointer to the VMA to split
 * @param split_addr Address at which to split the VMA
 * @return vma_area* Pointer to the new VMA, or nullptr if split failed
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE vma_area* split_vma(mm_context* mm_ctx, vma_area* vma, uintptr_t split_addr);

/**
 * @brief Prints a human-readable representation of all VMAs in a process's address space.
 * 
 * @param mm_ctx The process's memory management context
 * @param process_name Optional name of the process for better identification in logs
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void dbg_print_vma_regions(const mm_context* mm_ctx, const char* process_name = nullptr);

#endif // VMA_H 