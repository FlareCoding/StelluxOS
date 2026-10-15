// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_FWSEC_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_FWSEC_H

#include "common/types.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/vbios.h"

namespace drivers::nvidia {

/**
 * Which of FWSEC's signatures goes in its signature slot. FWSEC carries a
 * signature for each version it supports, and the slot must hold the one for
 * the version that the chip's fuses record for FWSEC.
 */
struct fwsec_signature {
    uint8_t fuse_version; // the version the chip's fuses record for FWSEC
    uint8_t index;        // the signature's place among FWSEC's signatures, counting from 0
};

/**
 * @brief Choose FWSEC's signature for the version the chip's fuses record.
 * FWSEC's ucode id, NVIDIA's number for it among the programs the GSP's
 * falcon runs, names one of 16 fuse registers, from 1 for the first. The
 * version is the bit number of that register's highest set bit plus 1, or 0
 * when no bit is set or the ucode id names none of them.
 * @param regs A view of BAR0.
 * @param fwsec FWSEC as `find_fwsec` returns it.
 * @param signature_out Receives the choice when the result is `OK`. Its
 * `fuse_version` is then below 16 and its `index` below
 * `fwsec.signature_count`.
 * @return `OK`, or `ERR_BAD_VBIOS` when FWSEC's signature versions do not
 * include the fuse version, or when the signature for it would come after
 * all `fwsec.signature_count` of FWSEC's signatures. The cause is logged.
 */
int32_t select_fwsec_signature(const registers& regs, const fwsec_layout& fwsec, fwsec_signature& signature_out);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_FWSEC_H
