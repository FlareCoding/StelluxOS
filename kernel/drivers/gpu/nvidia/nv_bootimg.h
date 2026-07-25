#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_BOOTIMG_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_BOOTIMG_H

#include "common/types.h"
#include "drivers/gpu/nvidia/nv_mem.h"

// GSP-RM boot ucode (the RISC-V "SK+BL" separation-kernel + bootloader image and
// its descriptor). Unlike gsp_ga10x.bin, this is not shipped as a firmware file -
// in the open driver it is compressed bindata (kgspGetGspRmBootUcodeStorage_GA102,
// storages "ucode_image_prod"/"ucode_desc_prod"). We decompress it offline with
// scripts/extract-nv-bindata.py and stage the raw bytes in the initrd; here we
// just load them. The SEC2 Booter authenticates this image into WPR2 and the GSP
// RISC-V boots from it. (open driver: kernel_gsp.c:3140-3225.)
namespace nvidia {

constexpr const char* GSP_BOOT_IMAGE_PATH =
    "/firmware/nvidia/535.183.01/gsp_ga10x_boot_image.bin";
constexpr const char* GSP_BOOT_DESC_PATH =
    "/firmware/nvidia/535.183.01/gsp_ga10x_boot_desc.bin";

constexpr int32_t BOOTIMG_OK       = 0;
constexpr int32_t ERR_BOOTIMG_IO   = -80; // missing/short file (staged in initrd?)
constexpr int32_t ERR_BOOTIMG_SIZE = -81; // unexpected size

// RM_RISCV_UCODE_DESC - exact mirror of open-gpu-kernel-modules
// arch/nvalloc/common/inc/rmRiscvUcode.h (21 x NvU32 = 84 bytes). Describes the
// layout of the SK+BL image; monitor{Code,Data}Offset + manifestOffset feed the
// GspFwWprMeta (kgspCalculateFbLayout_TU102:616-618).
struct rm_riscv_ucode_desc {
    uint32_t version;
    uint32_t bootloader_offset;
    uint32_t bootloader_size;
    uint32_t bootloader_param_offset;
    uint32_t bootloader_param_size;
    uint32_t riscv_elf_offset;
    uint32_t riscv_elf_size;
    uint32_t app_version;
    uint32_t manifest_offset;
    uint32_t manifest_size;
    uint32_t monitor_data_offset;
    uint32_t monitor_data_size;
    uint32_t monitor_code_offset;
    uint32_t monitor_code_size;
    uint32_t b_is_monitor_enabled;
    uint32_t swbrom_code_offset;
    uint32_t swbrom_code_size;
    uint32_t swbrom_data_offset;
    uint32_t swbrom_data_size;
    uint32_t fb_reserved_size;
    uint32_t b_signed_as_code;
};
static_assert(sizeof(rm_riscv_ucode_desc) == 84, "RM_RISCV_UCODE_DESC must be 84 bytes");

struct gsp_boot_ucode {
    dma_buffer          image;      // SK+BL image (CACHED contiguous), gspRmBootUcodeSize bytes
    uint32_t            image_size; // == bindata actualSize (24576 on GA102)
    rm_riscv_ucode_desc desc;
};

/**
 * @brief Load the staged GSP-RM boot ucode descriptor and SK+BL image from the
 * initrd. The image goes into a CACHED contiguous DMA buffer. Heavily logged.
 * @return BOOTIMG_OK on success; negative ERR_BOOTIMG_* otherwise.
 */
int32_t gsp_load_boot_ucode(gsp_boot_ucode& out);

/** @brief Release buffers from gsp_load_boot_ucode. */
void gsp_free_boot_ucode(gsp_boot_ucode& out);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_BOOTIMG_H
