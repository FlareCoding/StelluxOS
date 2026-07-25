#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_FWSEC_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_FWSEC_H

#include "common/types.h"
#include "drivers/gpu/nvidia/nv_vbios.h"
#include "drivers/gpu/nvidia/nv_mem.h"

// FWSEC ucode descriptor parsing for the RTX 3080 (GA102) bring-up.
//
// Faithful port of the open driver's s_vbiosParseFwsecUcodeDescFromBit
// (open-gpu-kernel-modules @535.183.01, kernel_gsp_fwsec.c): locate the BIT
// header, walk BIT tokens to the FALCON_DATA (0x70) token, follow it to the
// Falcon ucode table, and find the FWSEC entry's descriptor. The Falcon ucode
// table, entries, and descriptors are addressed at (expansionRomOffset + ptr);
// the BIT header and token DataPtrs are absolute - this offset distinction is
// the exact thing the prior attempt got wrong.
namespace nvidia {

constexpr int32_t FWSEC_OK         = 0;
constexpr int32_t ERR_FWSEC_NOBIT  = -30; // BIT header not found
constexpr int32_t ERR_FWSEC_NOFLCN = -31; // no FALCON_DATA token
constexpr int32_t ERR_FWSEC_TABLE  = -32; // bad Falcon ucode table
constexpr int32_t ERR_FWSEC_ENTRY  = -33; // no FWSEC entry / bad descriptor

struct fwsec_info {
    uint64_t vbios_version;    // (Version<<8)|OemVersion (40-bit) from BIOSDATA
    uint32_t bit_addr;         // BIT header offset within the image
    uint32_t ucode_table_ptr;  // FalconUcodeTablePtr (pre-adjustment)
    uint8_t  app_id;           // matched FWSEC application id (0x85 prod / 0x45 dbg / 0x05)
    uint32_t desc_offset;      // absolute image offset of the FWSEC desc (= ext_rom_off + DescPtr)
    uint8_t  desc_version;     // FALCON_UCODE_DESC version (2 or 3)
    uint32_t desc_size;        // descriptor size in bytes (44 + SignatureCount*384 for V3)

    // FALCON_UCODE_DESC_V3 body (valid when desc_version == 3). These are the
    // direct inputs to FWSEC-FRTS: where to DMA the code/data into the Falcon,
    // where the FRTS command interface lives, and the PKC signature selection.
    uint32_t stored_size;
    uint32_t pkc_data_offset;    // offset of the RSA-3K signature region
    uint32_t interface_offset;   // FRTS command interface offset (in DMEM)
    uint32_t imem_phys_base;
    uint32_t imem_load_size;
    uint32_t imem_virt_base;
    uint32_t dmem_phys_base;
    uint32_t dmem_load_size;
    uint16_t engine_id_mask;
    uint8_t  ucode_id;
    uint8_t  signature_count;
    uint16_t signature_versions;
};

/**
 * @brief Locate the FWSEC ucode descriptor inside an extracted VBIOS image.
 * @param vb        Extracted VBIOS (from vbios_extract_from_rom).
 * @param use_debug Select debug FWSEC (0x45) instead of production (0x85).
 * @param out       On success, the located FWSEC descriptor info.
 * @return FWSEC_OK on success; negative ERR_FWSEC_* otherwise. Heavily logged.
 */
int32_t fwsec_parse(const vbios_image& vb, bool use_debug, fwsec_info& out);

// Assembled FWSEC ucode, ready to DMA into the Falcon (code then data, one buffer).
struct fwsec_ucode {
    dma_buffer image;       // contiguous UNCACHED DMA buffer
    uint32_t   total_size;  // ALIGN256(stored_size) bytes
    uint32_t   imem_size;   // code = [0, imem_size)
    uint32_t   dmem_size;   // data = [imem_size, imem_size + dmem_size)
};

/**
 * @brief Copy the FWSEC code+data out of the VBIOS image into a contiguous,
 * uncached DMA buffer (the layout the Falcon DMA-load-and-go consumes). The
 * ucode bytes live at (desc_offset + desc_size) in the VBIOS, length
 * ALIGN256(stored_size). Host-side only; no GPU access. Free with
 * fwsec_free_ucode.
 */
int32_t fwsec_load_ucode(const vbios_image& vb, const fwsec_info& fi, fwsec_ucode& out);
void    fwsec_free_ucode(fwsec_ucode& out);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_FWSEC_H
