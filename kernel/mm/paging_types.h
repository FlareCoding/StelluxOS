#ifndef STELLUX_MM_PAGING_TYPES_H
#define STELLUX_MM_PAGING_TYPES_H

#include "types.h"
#include "mm/pmm_types.h"

namespace paging {

using virt_addr_t = uintptr_t;

// Sentinel value for unmap operations
constexpr pmm::phys_addr_t PHYS_UNMAP = ~0ULL;

// Page size constants
constexpr size_t PAGE_SIZE_4KB = 4096;
constexpr size_t PAGE_SIZE_2MB = 2 * 1024 * 1024;
constexpr size_t PAGE_SIZE_1GB = 1024 * 1024 * 1024;

// Permission flags (combinable)
constexpr uint32_t PAGE_READ      = (1 << 0);
constexpr uint32_t PAGE_WRITE     = (1 << 1);
constexpr uint32_t PAGE_EXEC      = (1 << 2);
constexpr uint32_t PAGE_USER      = (1 << 3);
constexpr uint32_t PAGE_GLOBAL    = (1 << 4);
constexpr uint32_t PAGE_LARGE_2MB = (1 << 5);
constexpr uint32_t PAGE_HUGE_1GB  = (1 << 6);

// Memory type flags (bits 8-9, mutually exclusive)
constexpr uint32_t PAGE_NORMAL    = (0 << 8);  // Write-back cacheable (regular RAM)
constexpr uint32_t PAGE_DEVICE    = (1 << 8);  // Uncached, strongly ordered (MMIO)
constexpr uint32_t PAGE_WC        = (2 << 8);  // Write-combining (framebuffer)
constexpr uint32_t PAGE_DMA       = (3 << 8);  // Non-cacheable (DMA buffers)

constexpr uint32_t PAGE_TYPE_MASK = (3 << 8);

using page_flags_t = uint32_t;

// Convenience flag combinations
constexpr uint32_t PAGE_KERNEL_RO  = PAGE_READ;
constexpr uint32_t PAGE_KERNEL_RW  = PAGE_READ | PAGE_WRITE;
constexpr uint32_t PAGE_KERNEL_RX  = PAGE_READ | PAGE_EXEC;
constexpr uint32_t PAGE_KERNEL_RWX = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
constexpr uint32_t PAGE_USER_RO    = PAGE_READ | PAGE_USER;
constexpr uint32_t PAGE_USER_RW    = PAGE_READ | PAGE_WRITE | PAGE_USER;
constexpr uint32_t PAGE_USER_RX    = PAGE_READ | PAGE_EXEC | PAGE_USER;
constexpr uint32_t PAGE_USER_RWX   = PAGE_READ | PAGE_WRITE | PAGE_EXEC | PAGE_USER;

// Memory attribute enum
enum class mem_attr : uint8_t {
    NORMAL = 0,        // Write-back cacheable (regular RAM)
    DEVICE = 1,        // Uncached, strongly ordered (MMIO)
    WRITE_COMBINE = 2, // Write-combining (framebuffer)
    DMA = 3,           // Non-cacheable (DMA buffers)
};

// Result codes
constexpr int32_t OK                 = 0;
constexpr int32_t ERR_INVALID_ADDR   = -1;
constexpr int32_t ERR_ALREADY_MAPPED = -2;
constexpr int32_t ERR_NOT_MAPPED     = -3;
constexpr int32_t ERR_ALIGNMENT      = -4;
constexpr int32_t ERR_INVALID_FLAGS  = -5;

} // namespace paging

#endif // STELLUX_MM_PAGING_TYPES_H
