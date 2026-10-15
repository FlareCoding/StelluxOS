// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/fwsec.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "common/logging.h"

namespace drivers::nvidia {

// The fuse registers recording a version for each program the GSP's falcon runs, the first for ucode id 1
constexpr uint32_t UCODE_VERSION_FUSES       = 0x8241C0;
constexpr uint32_t UCODE_VERSION_FUSE_STRIDE = 4; // bytes from one fuse register to the next
constexpr uint32_t UCODE_VERSION_FUSE_COUNT  = 16;

// The versions FWSEC's signature versions can list, one bit each from version 0
constexpr uint32_t SIGNATURE_VERSION_COUNT = 8 * sizeof(fwsec_layout::signature_versions);

// The index of the fuse register's highest set bit plus 1, or 0 when no bit is set or the ucode id names none
static uint32_t read_fuse_version(const registers& regs, uint8_t ucode_id) {
    uint32_t fuse_index = static_cast<uint32_t>(ucode_id) - 1;
    if (fuse_index >= UCODE_VERSION_FUSE_COUNT) {
        return 0;
    }

    uint32_t fuses = regs.read(UCODE_VERSION_FUSES + fuse_index * UCODE_VERSION_FUSE_STRIDE);
    uint32_t version = 0;
    while (fuses != 0) {
        version++;
        fuses >>= 1;
    }

    return version;
}

int32_t select_fwsec_signature(const registers& regs, const fwsec_layout& fwsec, fwsec_signature& signature_out) {
    uint32_t fuse_version = read_fuse_version(regs, fwsec.ucode_id);

    // Versions from 16 have no bit, and the bound keeps the shift below 32 bits, where it is defined
    bool has_version_bit = fuse_version < SIGNATURE_VERSION_COUNT;
    bool has_signature = has_version_bit && ((fwsec.signature_versions >> fuse_version) & 1) != 0;
    if (!has_signature) {
        log::error(
            "nvidia: FWSEC has no signature for fuse version %u, only for the versions whose bits are set in 0x%04X",
            fuse_version,
            fwsec.signature_versions
        );
        return ERR_BAD_VBIOS;
    }

    uint32_t index = 0;
    for (uint32_t version = 0; version < fuse_version; version++) {
        index += (fwsec.signature_versions >> version) & 1;
    }

    if (index >= fwsec.signature_count) {
        log::error(
            "nvidia: FWSEC's signature for fuse version %u comes after %u others, past its %u signatures",
            fuse_version,
            index,
            fwsec.signature_count
        );
        return ERR_BAD_VBIOS;
    }

    signature_out.fuse_version = static_cast<uint8_t>(fuse_version);
    signature_out.index = static_cast<uint8_t>(index);

    return OK;
}

} // namespace drivers::nvidia
