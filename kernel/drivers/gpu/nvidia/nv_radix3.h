#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_RADIX3_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_RADIX3_H

#include "common/types.h"
#include "drivers/gpu/nvidia/nv_mem.h"
#include "drivers/gpu/nvidia/nv_firmware.h"

// radix3: the 3-level sysmem page table the GSP MMU uses to gather the (large)
// GSP-RM image during boot. Faithful port of _kgspCreateRadix3 (kernel_gsp.c),
// LIBOS_MEMORY_REGION_RADIX_PAGE_SIZE=4096. The open driver backs the image with
// non-contiguous pages; StelluxOS's buddy allocator gives large contiguous
// blocks, so we back it contiguously and the PDE/PTE entries become simple
// arithmetic (raw byte phys, exactly what the GSP expects). Host-side only.
namespace nvidia {

constexpr int32_t RADIX3_OK        = 0;
constexpr int32_t ERR_RADIX3_MEM   = -70; // allocation failed
constexpr int32_t ERR_RADIX3_IO    = -71; // .fwimage read failed
constexpr int32_t ERR_RADIX3_SHAPE = -72; // unexpected table geometry

struct gsp_radix3 {
    dma_buffer image;      // contiguous .fwimage copy (CACHED)
    dma_buffer table;      // contiguous 3-level page table (CACHED)
    uint64_t   root_phys;  // radix3 root PDE phys (handed to GSP-RM boot)
    uint32_t   data_pages; // number of 4K data pages
};

/**
 * @brief Load the GSP-RM image (.fwimage) into a contiguous buffer and build the
 * radix3 page table over it.
 * @param fw_path Path to gsp_ga10x.bin in the initrd.
 * @param fi      Parsed firmware image (provides .fwimage offset/size).
 * @param out     On success, the image + table buffers and root PDE phys.
 * @return RADIX3_OK on success; negative ERR_RADIX3_* otherwise. Heavily logged.
 */
int32_t gsp_build_radix3(const char* fw_path, const gsp_fw_image& fi, gsp_radix3& out);

/** @brief Release buffers from gsp_build_radix3. */
void gsp_free_radix3(gsp_radix3& out);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_RADIX3_H
