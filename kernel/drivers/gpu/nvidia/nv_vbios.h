#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_VBIOS_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_VBIOS_H

#include "common/types.h"

// VBIOS extraction for the RTX 3080 (GA102) bring-up.
//
// Faithful port of the open driver's kgspExtractVbiosFromRom_TU102
// (open-gpu-kernel-modules @535.183.01,
//  src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_vbios_tu102.c):
// read the GPU expansion ROM through the NV_PROM aperture in BAR0, skip any
// leading IFR region, walk the PCI expansion-ROM (PCIR + NPDE) chain to size
// it, then copy the whole image into system memory. FWSEC is parsed from this
// image later (the next stage).
namespace nvidia {

constexpr int32_t VB_OK        = 0;
constexpr int32_t ERR_VB_NOSIG = -20; // no valid PCI expansion-ROM signature
constexpr int32_t ERR_VB_IFR   = -21; // malformed IFR / ROM directory
constexpr int32_t ERR_VB_ROMS  = -22; // malformed PCIR / expansion-ROM chain
constexpr int32_t ERR_VB_SIZE  = -23; // computed size out of range
constexpr int32_t ERR_VB_MEM   = -24; // buffer allocation failed

// A CPU-side copy of the GPU's VBIOS image.
struct vbios_image {
    uint8_t* image;       // image buffer (vmm::alloc VA); release via vbios_free
    uint32_t size;        // total VBIOS size in bytes
    uint32_t ext_rom_off; // expansion-ROM offset (ext - base), 0 if none
};

/**
 * @brief Read + size the VBIOS from the GPU expansion ROM (NV_PROM in BAR0).
 * @param bar0_va Mapped BAR0 base virtual address.
 * @param out     On success, holds the extracted image (free with vbios_free).
 * @return VB_OK on success; negative ERR_VB_* otherwise. Heavily logged.
 */
int32_t vbios_extract_from_rom(uintptr_t bar0_va, vbios_image& out);

/** @brief Release an image returned by vbios_extract_from_rom. */
void vbios_free(vbios_image& img);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_VBIOS_H
