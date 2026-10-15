// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_FWSEC_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_FWSEC_H

#include "common/types.h"
#include "drivers/graphics/nvidia/falcon.h"
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

/**
 * @brief Copy FWSEC from the VBIOS into `image` and patch the copy to fence
 * off the FRTS region at `frts_offset` when the falcon runs it. While FWSEC
 * runs, the falcon holds its data in DMEM, the falcon's data memory, and
 * FWSEC takes its command through a table in that data, which NVIDIA calls
 * its application interface:
 *
 *   data + interface_offset   header: 4 bytes, the entry count at byte 3
 *                             entries: 8 bytes each, an id and an offset in the data
 *   data + mapper's offset    the DMEM mapper, 64 bytes: the id of the command FWSEC
 *                             runs when it starts, and its input buffer's offset and size
 *   data + buffer's offset    the input buffer, which holds the command's arguments
 *
 * The patch writes the FRTS command, 48 bytes of arguments, at the start of
 * the input buffer of the mapper that the table's last DMEM mapper entry
 * leads to, sets the FRTS command's id as the one FWSEC runs when it starts,
 * and then copies the chosen signature into the signature slot.
 * @param rom A view whose offset 0 is the first byte of the ROM window, and
 * which must cover all `ROM_WINDOW_BYTES` of it.
 * @param fwsec FWSEC, which must be as `find_fwsec` returns it.
 * @param signature The signature, which must be the one `select_fwsec_signature`
 * chose for `fwsec`.
 * @param frts_offset Where the FRTS region starts in video memory, as
 * `place_frts_region` returns it. The command carries it in 4 KiB units kept
 * to 32 bits, as NVIDIA's driver does, so it must lie below 16 TiB.
 * @param image Receives the copy, and must hold `fwsec.size` bytes. When the
 * result is `OK`, the copy differs from FWSEC as the VBIOS stores it only in
 * the command's arguments, the mapper's command id and the signature slot.
 * @return `OK`, or `ERR_BAD_VBIOS` when the table's header or one of its
 * entries ends past FWSEC's data, the table has fewer than 2 entries or no
 * DMEM mapper entry, a DMEM mapper ends past the data, or the last one's
 * input buffer is smaller than the command or, at the buffer's offset, the
 * command ends past the data. The cause is logged, and `image` then holds
 * FWSEC as the VBIOS stores it.
 */
int32_t prepare_fwsec_frts(
    const registers& rom,
    const fwsec_layout& fwsec,
    const fwsec_signature& signature,
    uint64_t frts_offset,
    uint8_t* image
);

/**
 * @brief Describe a copy of FWSEC as the program the GSP's falcon runs, with
 * the sizes, bases, engine mask and ucode id FWSEC's descriptor gives, and
 * the signature slot as the signature's offset in the data. The descriptor's
 * code size and IMEM virtual base are unchecked, as in NVIDIA's driver, so a
 * copy on a 256-byte boundary, as a page allocation is, meets
 * `run_gsp_falcon_program`'s alignment only when both are multiples of 256.
 * FWSEC's size, rounded up to 256 bytes, then holds each part's whole blocks.
 * @param fwsec FWSEC as `find_fwsec` returns it.
 * @param dma_address Where the copy starts, as the card addresses memory.
 * The copy must hold FWSEC as `prepare_fwsec_frts` fills it.
 * @param is_cached Whether the CPU maps the copy cached.
 * @return The program `run_gsp_falcon_program` loads from the copy.
 */
falcon_program describe_fwsec_program(const fwsec_layout& fwsec, uint64_t dma_address, bool is_cached);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_FWSEC_H
