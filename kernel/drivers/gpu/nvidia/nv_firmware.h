#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_FIRMWARE_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_FIRMWARE_H

#include "common/types.h"

// GSP firmware handling for the RTX 3080 (GA102) bring-up.
//
// We reuse NVIDIA's OpenRM GSP firmware verbatim. gsp_ga10x.bin is a RISC-V
// ELF whose .fwimage section is the ~38 MB GSP-RM image that gets loaded into
// VRAM, .fwversion is the version string, and .fwsignature[...] are the HS
// signature blobs (one per fuse version). The blob is staged into the initrd
// at GSP_FW_PATH by the build.
namespace nvidia {

constexpr const char* GSP_FW_PATH    = "/firmware/nvidia/535.183.01/gsp_ga10x.bin";
constexpr const char* GSP_FW_VERSION = "535.183.01";

constexpr int32_t FW_OK            = 0;
constexpr int32_t ERR_FW_NOENT     = -10; // file missing (not staged in initrd?)
constexpr int32_t ERR_FW_IO        = -11; // read/seek failure
constexpr int32_t ERR_FW_FORMAT    = -12; // not the expected ELF
constexpr int32_t ERR_FW_VERSION   = -13; // .fwversion != GSP_FW_VERSION
constexpr int32_t ERR_FW_NOSECTION = -14; // required section missing

// Located file offsets/sizes of the GSP firmware ELF sections we care about.
struct gsp_fw_image {
    uint64_t fwimage_off;     // .fwimage   - the GSP-RM image payload
    uint64_t fwimage_size;
    uint64_t fwversion_off;   // .fwversion - version string
    uint64_t fwversion_size;
    uint64_t fwsig_off[8];    // .fwsignature[...] - HS signatures (per fuse ver)
    uint64_t fwsig_size[8];
    uint32_t fwsig_count;
    uint64_t sig_ga10x_off;   // .fwsignature_ga10x - GSP-RM signature (Booter verify)
    uint64_t sig_ga10x_size;
    char     version[32];     // decoded .fwversion text
};

/**
 * @brief Open the staged GSP firmware ELF, validate it is the expected
 * 535.183.01 RISC-V image, and record the .fwimage / .fwsignature section
 * locations. Reads only ELF metadata (header + section table + .fwversion),
 * not the full 38 MB payload. Heavily logged.
 * @return FW_OK on success; negative ERR_FW_* otherwise.
 */
int32_t fw_probe_gsp(gsp_fw_image& out);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_FIRMWARE_H
