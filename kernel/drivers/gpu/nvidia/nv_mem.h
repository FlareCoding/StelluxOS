#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_MEM_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_MEM_H

#include "common/types.h"
#include "mm/pmm_types.h"

// DMA memory helpers for the RTX 3080 (GA102) bring-up.
//
// The GSP / Falcon path needs physically-contiguous sysmem buffers whose
// device (DMA) physical address we hand to the GPU. GA102's sysmem DMA window
// is 47-bit (NV_GSP_GPU_MIN_SUPPORTED_DMA_ADDR_WIDTH = 47), so we validate every
// allocation fits. On x86 with no IOMMU translation, dma_addr == phys_addr.
namespace nvidia {

constexpr int32_t MEM_OK            = 0;
constexpr int32_t ERR_MEM_ALLOC     = -40; // allocation failed
constexpr int32_t ERR_MEM_DMA_RANGE = -41; // phys outside the 47-bit DMA window

// GA102 sysmem DMA address width (bits). Anything the GPU DMAs must fit here.
constexpr uint32_t GSP_DMA_ADDR_WIDTH = 47;

// A physically-contiguous DMA buffer: CPU virtual address + device physical addr.
struct dma_buffer {
    uintptr_t        cpu_va; // CPU access
    pmm::phys_addr_t phys;   // value handed to the GPU (== dma addr on x86)
    size_t           size;   // requested byte size
};

/**
 * @brief Allocate a physically-contiguous, zeroed DMA buffer.
 * @param bytes    Requested size (rounded up to whole pages by the allocator).
 * @param uncached If true, map non-cacheable (PAGE_DMA) - required for buffers
 *                 the GPU DMAs while the CPU also writes them (matches the open
 *                 driver's NV_MEMORY_UNCACHED ucode buffers).
 * @param out      On success, the allocated buffer.
 * @return MEM_OK on success; negative ERR_MEM_* otherwise.
 */
int32_t dma_alloc(size_t bytes, bool uncached, dma_buffer& out);

/** @brief Free a buffer from dma_alloc and clear the struct. */
void dma_free(dma_buffer& buf);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_MEM_H
