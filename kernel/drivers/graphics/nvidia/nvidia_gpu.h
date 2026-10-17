// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_GPU_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_GPU_H

#include "drivers/graphics/nvidia/gsp_boot_memory.h"
#include "drivers/pci_driver.h"

namespace drivers {

/**
 * The driver for NVIDIA GPUs, one object per card. The PCI framework creates
 * it on the unprivileged heap for every NVIDIA display controller, calls
 * `attach()` on the boot thread before userland starts, and frees it after any
 * result but `OK`. The driver owns its BAR0 mapping, which `detach()` unmaps,
 * and the GSP's boot memory, which `detach()` frees once each falcon it resets
 * reads as stopped.
 *
 * `attach()` declines the card when a driver display is already registered
 * with gfxfb, when the chip table does not list the card's chip, or when a
 * directory the chip's firmware files belong in is not installed, as in an
 * image without the nvidia-firmware package. Bring-up logs the layout of the
 * card's VBIOS, opens the chip's firmware files, checks the GSP image's
 * firmware version, logs where the GSP image places its firmware and the
 * chip's signature for it, checks the bootloader's header, logs where the
 * bootloader's descriptor places the parts of its program and the program's
 * app version. Booter load is the program SEC2, the GPU's security processor,
 * runs to boot the GSP. Bring-up then checks booter load's header, logs where
 * its descriptors place its code, data, signature slot and signatures, with
 * its ucode id and engine mask, chooses and logs which of booter load's
 * signatures its slot must hold, reading the version the chip's fuses, bits
 * burned into it for good, record for booter load, checks that the
 * bootloader's program holds at least one byte and that a radix3 page table,
 * NVIDIA's three-level table of page addresses, maps the GSP's firmware, and
 * finds FWSEC, the signed program in the VBIOS that fences off protected
 * memory for the GSP's firmware, through the VBIOS's falcon program table.
 * Once the card's boot firmware has finished setting up video memory, it
 * works out where in video memory FWSEC's FRTS region goes (the part of
 * protected memory FWSEC fences off first), then lays out and logs the rest
 * of protected memory and the heap the GSP's firmware keeps below it, as
 * `nvidia::protected_memory_layout` describes. It then chooses which of
 * FWSEC's signatures its signature slot must hold (the one for the version the
 * chip's fuses record for FWSEC), checks that the GSP has a RISC-V core for
 * the GSP's firmware to run on, and copies FWSEC into memory the card can
 * reach, patched to fence off the region and holding that signature. It then
 * resets the GSP's falcon, the microcontroller in the GSP that runs FWSEC, and
 * that reset is the first step that writes to the card's BAR0 registers. The
 * card's bus mastering is then turned on, and the falcon loads FWSEC from the
 * copy and is started on it. Once the falcon halts, the copy is freed, and
 * bring-up checks that FWSEC fenced off the region. It then copies the
 * bootloader's program, the GSP's firmware and the firmware's signature into
 * memory the card can reach, the firmware into pages listed by a radix3 page
 * table, copies booter load's program, with the chosen signature in its slot,
 * into memory of its own the card can reach, lays out the message queues (the
 * two rings through which the driver and the GSP's firmware exchange messages)
 * in memory of their own, writes protected memory's metadata into a page of
 * its own, and writes the log buffers and the arguments the firmware reads
 * when it starts, all as `nvidia::gsp_boot_memory` describes. The arguments
 * are those of the firmware's resource manager (its task that serves the
 * driver's requests) and of LibOS (the small operating system the firmware
 * runs on). The metadata tells booter load where the copies of the
 * bootloader's program, the firmware and its signature lie and where each part
 * of protected memory goes. Bring-up next queues in the command queue the two
 * messages NVIDIA's driver sends before the GSP starts (the system information
 * and the registry, here with no entry), as `nvidia::queue_boot_messages`
 * describes. The system information says how much video memory the firmware
 * must leave alone for a boot console (a boot framebuffer that may still be
 * drawn in), as `nvidia::boot_console_bytes` works it out from the card's
 * BARs, the boot framebuffer `g_boot_info` records and whether the chip's
 * fuses disable its display. Bring-up then resets the GSP into its RISC-V
 * core, tells the firmware once that both messages wait and writes the address
 * of LibOS's arguments into the GSP falcon's two mailbox registers. NVIDIA's
 * driver does these before booter load too, but resets the GSP before queueing
 * the messages and tells the firmware after each. Bring-up then resets SEC2's
 * falcon, runs booter load on it from its copy with the address of protected
 * memory's metadata in the SEC2 falcon's mailboxes, checks that booter load
 * left 0 in the first, writes the bootloader's app version into the register
 * of the GSP's falcon NVIDIA calls its OS register, and checks that the GSP's
 * RISC-V core, which booter load starts on the bootloader's program, reads as
 * running. Bring-up stops there, so when every step succeeds, `attach()`
 * declines as well.
 * `attach()` fails with a negative error when BAR0 cannot be mapped, the card
 * does not respond, its VBIOS's images are not laid out as
 * `nvidia::read_vbios_layout` requires, a firmware file of an installed
 * directory cannot be opened, the GSP image fails `nvidia::check_gsp_image`,
 * which requires a 64-bit little-endian ELF file whose one .fwversion
 * section holds exactly `FIRMWARE_VERSION` and its NUL, with one
 * .fwimage section and one section of the name the chip table gives for
 * the chip's signature, a name of at most 31 bytes, each section holding
 * at least one byte inside the file, the bootloader or booter load
 * is shorter than its header, cannot be read or holds a header that does not
 * describe it, the bootloader's descriptor is too short to hold every field
 * `nvidia::read_bootloader_layout` reads or places a part that ends past the
 * end of the bootloader's program, booter load's descriptors fail
 * `nvidia::read_booter_layout`, which requires them to follow NVIDIA's rules
 * for booter load and place its code, data, signature slot and signatures as
 * `nvidia::booter_layout` describes, booter load has more than one signature
 * and none for the fuse version read for it, the bootloader's program holds
 * no byte or the GSP's firmware has more pages than a radix3 page table maps,
 * as `nvidia::check_boot_copies` checks, or a firmware name built from the
 * chip table is too long or malformed. Once the firmware files check
 * out, it also fails when the VBIOS does not lead to a falcon program table
 * as `nvidia::find_falcon_program_table` requires, or the table does not
 * lead to FWSEC as `nvidia::find_fwsec` requires. Once FWSEC is found, it
 * fails when the card's boot firmware has not finished within
 * `nvidia::BOOT_FIRMWARE_TIMEOUT_NS`, or when `nvidia::place_frts_region`
 * refuses the card's state, as when protected memory is already set up. Once
 * the region is placed, it fails when `nvidia::lay_out_protected_memory`
 * finds that the layout does not fit in the part of video memory cleared
 * before the GSP's firmware runs, or when `nvidia::select_fwsec_signature`
 * refuses FWSEC's signatures (as when none is for the version the chip's fuses
 * record), when the GSP has no RISC-V core for the GSP's firmware to run on,
 * when the memory for FWSEC's copy cannot be allocated, or when FWSEC's data
 * does not lead to a place for the FRTS command as
 * `nvidia::prepare_fwsec_frts` requires. Once the copy is patched, it fails
 * when, during the reset of the GSP's falcon, the falcon does not finish
 * clearing its memories, or the GSP does not confirm its switch from its
 * RISC-V core to its falcon core, within `nvidia::FALCON_TIMEOUT_NS` of that
 * wait's start. Once the falcon is reset, it fails when the falcon does not
 * finish loading a block of FWSEC, or does not halt, within
 * `nvidia::FALCON_TIMEOUT_NS` of that wait's start. Once the falcon halts,
 * it fails when `nvidia::check_frts_result` finds an FRTS error code other
 * than 0, or protected memory not set up or starting elsewhere than at the
 * region. Once FWSEC has fenced off the region, it fails when the memory for
 * the copies, the message queues, protected memory's metadata or what the
 * firmware reads when it starts cannot be allocated, reading a firmware file
 * fails or the command queue lacks room for the system information or the
 * registry. Once the messages are queued, it fails when the falcon does not
 * finish clearing its memories within `nvidia::FALCON_TIMEOUT_NS` during the
 * GSP's reset into its RISC-V core. Once the GSP falcon's mailboxes are
 * written, it fails when SEC2's reset times out as the reset of the GSP's
 * falcon can, when SEC2's falcon does not finish loading a block of booter
 * load, or does not halt, within `nvidia::FALCON_TIMEOUT_NS` of that wait's
 * start, when its first mailbox then reads other than 0, or when the GSP's
 * RISC-V core does not read as running.
 * Whether `attach()` declines or fails, the screen is left as it was, since
 * bring-up itself never writes to the display engine, and the card keeps the
 * memory and I/O decoding that `attach()` turned on. A card that `attach()`
 * declines or fails before the first reset of the GSP's falcon keeps only that
 * decoding. A reset that times out leaves the falcon out of reset with its
 * memories still being cleared, or with its switch to the falcon core
 * requested but not confirmed. A card that gets as far as the load also keeps
 * its bus mastering on, and the load leaves its falcon's memory interface
 * letting the DMA engine read system memory by physical address. A load that
 * times out leaves the falcon not started, with its DMA engine possibly still
 * copying, and a run that times out leaves the falcon possibly still running
 * FWSEC, which may yet fence off the FRTS region. Either leaves FWSEC's copy
 * allocated until `detach()` confirms the GSP stopped, and for good when it
 * cannot, since the falcon may still read it. A falcon that halts, after FWSEC
 * ran or after the falcon's boot ROM refused it, is left halted when
 * `attach()` fails before the GSP's reset into its RISC-V core. A load of
 * booter load that times out leaves SEC2's falcon as FWSEC's leaves the GSP's,
 * and a run that times out may leave booter load running, which may later
 * start the GSP's RISC-V core. When every step succeeds, FWSEC ran and fenced
 * off the FRTS region, booter load ran on SEC2, and `attach()` returns with
 * the GSP's RISC-V core reading as running. Once FWSEC has set up protected
 * memory, it stays set up until the card is reset, and
 * `nvidia::place_frts_region` refuses the card in every `attach()` until then.
 * The decoding stays on because the boot framebuffer may be in this card's
 * memory, which the CPU reaches only while memory decoding is on.
 * From the first reset of the GSP's falcon on, whether `attach()` declines or
 * fails, `detach()` resets the GSP's falcon again with `nvidia::stop_falcon`
 * and frees all of the GSP's boot memory once the falcon reads as halted and
 * the RISC-V core as not running, leaving the falcon core selected. From the
 * load of booter load on, `detach()` stops SEC2's falcon the same way first,
 * since a booter load still running may start the GSP's RISC-V core again,
 * then stops the GSP whatever SEC2's result, freeing the memory only once both
 * read as stopped. `detach()` fails, keeping all of the boot memory allocated
 * for good, when a reset times out or a falcon then does not read as halted or
 * its RISC-V core reads as running.
 * NVIDIA's driver asks the GSP's firmware to unload and waits for the GSP to
 * suspend before the GSP's reset, then runs FWSEC again, to put back the
 * programs the VBIOS runs before an operating system loads, and booter unload,
 * to take protected memory down. `detach()` resets the running firmware
 * without these steps, so those programs are not put back and protected memory
 * stays set up.
 */
class nvidia_gpu_driver : public pci_driver {
public:
    nvidia_gpu_driver(pci::device* dev) : pci_driver("nvidia_gpu", dev) {}

    int32_t attach() override;

    /**
     * @brief Stop SEC2 once bring-up has begun loading booter load, then the
     * GSP once bring-up has reset its falcon, freeing the GSP's boot memory
     * only once each reads as stopped, then unmap BAR0 as
     * `pci_driver::detach()` does. Each stop spins on the calling CPU, for up
     * to twice `nvidia::FALCON_READY_TIMEOUT_NS` and six times
     * `nvidia::FALCON_TIMEOUT_NS` when its falcon is slow to reset.
     * @return What `pci_driver::detach()` returns once the falcons are stopped
     * or need no stop, `nvidia::ERR_TIMEOUT` when a reset times out, or
     * `nvidia::ERR_BAD_STATE` when a falcon then does not read as halted or
     * its RISC-V core reads as running, each error keeping the boot memory
     * allocated for good. When SEC2's stop fails, the GSP's stop still runs
     * and SEC2's error is returned. The cause is logged. A later call stops no
     * falcon again and returns what `pci_driver::detach()` returns.
     */
    int32_t detach() override;

    void run() override;

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void on_interrupt(uint32_t vector) override;

private:
    int32_t stop_falcons_and_free_boot_memory();

    nvidia::gsp_boot_memory m_boot_memory;
    uint32_t                m_boot0 = 0;
    bool                    m_must_stop_gsp = false;
    bool                    m_must_stop_sec2 = false;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_GPU_H
