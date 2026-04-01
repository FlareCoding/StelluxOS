#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_FWSEC_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_FWSEC_H

#include "common/types.h"
#include "drivers/graphics/nvidia/nv_types.h"
#include "drivers/graphics/nvidia/nv_firmware.h"
#include "drivers/graphics/nvidia/nv_falcon.h"

namespace nv {

class nv_gpu;

// FWSEC commands
constexpr uint32_t FWSEC_CMD_FRTS = 0x15; // Create WPR2 / FRTS region
constexpr uint32_t FWSEC_CMD_SB   = 0x19; // Secure Boot

// FRTS region type
constexpr uint32_t FRTS_REGION_TYPE_FB = 0x02;

// FRTS region size (1MB)
constexpr uint32_t FRTS_REGION_SIZE = 0x100000;

// Application interface IDs
constexpr uint32_t APPIF_ID_DMEMMAPPER = 0x04;

// DMEMMAPPER signature
constexpr uint32_t DMEMMAPPER_SIG = 0x50414D44; // "DMAP"

// ============================================================================
// FWSEC DMEM Structures (for command patching)
// ============================================================================

struct __attribute__((packed)) falcon_appif_hdr_v1 {
    uint8_t version;     // Must be 1
    uint8_t header_size; // Typically 4
    uint8_t entry_size;  // Typically 8
    uint8_t entry_count;
};
static_assert(sizeof(falcon_appif_hdr_v1) == 4);

struct __attribute__((packed)) falcon_appif_v1 {
    uint32_t id;
    uint32_t dmem_base;
};
static_assert(sizeof(falcon_appif_v1) == 8);

struct __attribute__((packed)) falcon_dmemmapper_v3 {
    uint32_t signature;              // "DMAP" = 0x50414D44
    uint16_t version;                // 3
    uint16_t size;                   // 64
    uint32_t cmd_in_buffer_offset;
    uint32_t cmd_in_buffer_size;
    uint32_t cmd_out_buffer_offset;
    uint32_t cmd_out_buffer_size;
    uint32_t nvf_img_data_buffer_offset;
    uint32_t nvf_img_data_buffer_size;
    uint32_t printf_buffer_hdr;
    uint32_t ucode_build_time_stamp;
    uint32_t ucode_signature;
    uint32_t init_cmd;               // Patched with FWSEC_CMD_FRTS or FWSEC_CMD_SB
    uint32_t ucode_feature;
    uint32_t ucode_cmd_mask0;
    uint32_t ucode_cmd_mask1;
    uint32_t multi_tgt_tbl;
};
static_assert(sizeof(falcon_dmemmapper_v3) == 64);

// FRTS command input buffer (ReadVbios + FrtsRegion)
struct __attribute__((packed)) frts_read_vbios {
    uint32_t ver;    // 1
    uint32_t hdr;    // sizeof(frts_read_vbios) = 24
    uint64_t addr;   // 0 (GPU reads VBIOS from ROM itself)
    uint32_t size;   // 0
    uint32_t flags;  // 2
};
static_assert(sizeof(frts_read_vbios) == 24);

struct __attribute__((packed)) frts_region {
    uint32_t ver;    // 1
    uint32_t hdr;    // sizeof(frts_region) = 20
    uint32_t addr;   // FRTS FB address >> 12
    uint32_t size;   // FRTS size >> 12
    uint32_t ftype;  // FRTS_REGION_TYPE_FB = 2
};
static_assert(sizeof(frts_region) == 20);

struct __attribute__((packed)) frts_cmd {
    frts_read_vbios read_vbios;
    frts_region     region;
};
static_assert(sizeof(frts_cmd) == 44);

// ============================================================================
// FWSEC-FRTS API
// ============================================================================

/**
 * Execute FWSEC-FRTS on the GSP falcon to establish the WPR2 region.
 *
 * This function:
 * 1. Determines the FRTS region address in VRAM
 * 2. Selects the correct RSA-3K signature via fuse registers
 * 3. Patches the FWSEC DMEM with the FRTS command and parameters
 * 4. Copies the selected signature into the ucode
 * 5. Allocates a DMA buffer and copies the patched ucode
 * 6. Resets the GSP falcon
 * 7. DMA-loads IMEM+DMEM onto the falcon
 * 8. Programs BROM registers for HS verification
 * 9. Boots the falcon and waits for completion
 * 10. Verifies WPR2 was successfully established
 *
 * @param gpu    GPU device
 * @param fwsec  Parsed FWSEC firmware data (will be modified: DMEM patched)
 * @param gsp    GSP falcon interface
 * @return OK on success (WPR2 established), error code on failure
 */
int32_t fwsec_run_frts(nv_gpu* gpu, fwsec_fw& fwsec, falcon& gsp);

/**
 * Determine the FRTS region address in VRAM.
 * Computes the layout based on VRAM size and VGA workspace.
 *
 * @param gpu       GPU device
 * @param out_addr  Output: FRTS region start address in VRAM
 * @param out_size  Output: FRTS region size in bytes
 * @return OK on success
 */
int32_t fwsec_compute_frts_region(nv_gpu* gpu, uint64_t& out_addr, uint64_t& out_size);

/**
 * Select the correct RSA-3K signature for FWSEC based on fuse registers.
 *
 * @param gpu          GPU device
 * @param fwsec        FWSEC firmware data
 * @param out_sig_idx  Output: index of the selected signature
 * @return OK on success
 */
int32_t fwsec_select_signature(nv_gpu* gpu, const fwsec_fw& fwsec, uint32_t& out_sig_idx);

/**
 * Patch FWSEC DMEM with FRTS command and parameters.
 *
 * @param fwsec      FWSEC firmware data (ucode_data will be modified)
 * @param frts_addr  FRTS region address in VRAM
 * @param frts_size  FRTS region size in bytes
 * @param sig_idx    Signature index (selected by fwsec_select_signature)
 * @return OK on success
 */
int32_t fwsec_patch_dmem(fwsec_fw& fwsec, uint64_t frts_addr, uint64_t frts_size,
                          uint32_t sig_idx);

} // namespace nv

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_FWSEC_H
