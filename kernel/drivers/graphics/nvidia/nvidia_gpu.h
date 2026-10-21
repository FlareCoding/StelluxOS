// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_GPU_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_GPU_H

#include "drivers/graphics/nvidia/chip.h"
#include "drivers/graphics/nvidia/fwsec.h"
#include "drivers/graphics/nvidia/gsp.h"
#include "drivers/graphics/nvidia/gsp_boot_memory.h"
#include "drivers/graphics/nvidia/gsp_info.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/pci_driver.h"

namespace drivers {

/**
 * The driver for NVIDIA GPUs, one object per card. The PCI framework creates
 * it on the unprivileged heap for every NVIDIA display controller, calls
 * `attach()` on the boot thread before userland starts, and frees it after any
 * result but `OK`. The driver owns its BAR0 mapping, which `detach()` unmaps,
 * the GSP's boot memory, which `detach()` frees once each falcon it resets
 * reads as stopped, a copy of booter unload, which `detach()` frees once SEC2
 * can no longer read it, and the payload buffer of its `nvidia::gsp`, which
 * `detach()` frees with `nvidia::gsp::stop`.
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
 * runs to boot the GSP, and booter unload is SEC2's program to take protected
 * memory down. Bring-up then checks booter load's header, logs where its
 * descriptors place its code, data, signature slot and signatures, with its
 * ucode id and engine mask, chooses and logs which of booter load's signatures
 * its slot must hold, reading the version the chip's fuses, bits burned into
 * it for good, record for booter load, does the same for booter unload, checks
 * that the bootloader's program holds at least one byte and that a radix3 page
 * table, NVIDIA's three-level table of page addresses, maps the GSP's
 * firmware, and finds FWSEC, the signed program in the VBIOS that fences off
 * protected memory for the GSP's firmware, through the VBIOS's falcon program
 * table. Once the card's boot firmware has finished setting up video memory,
 * it works out where in video memory FWSEC's FRTS region goes (the part of
 * protected memory FWSEC fences off first), then lays out and logs the rest
 * of protected memory and the heap the GSP's firmware keeps below it, as
 * `nvidia::protected_memory_layout` describes. It then chooses which of
 * FWSEC's signatures its signature slot must hold (the one for the version the
 * chip's fuses record for FWSEC), checks that the GSP has a RISC-V core for
 * the GSP's firmware to run on, copies booter unload's program, with its
 * chosen signature in its slot, into memory of its own the card can reach, and
 * copies FWSEC into memory the card can reach, patched to fence off the region
 * and holding FWSEC's chosen signature. It then resets the GSP's falcon, the
 * microcontroller in the GSP that runs FWSEC, and that reset is the first step
 * that writes to the card's BAR0 registers. The card's bus mastering is then
 * turned on, and the falcon loads FWSEC from FWSEC's copy and is started on
 * it. Once the falcon halts, FWSEC's copy is freed, and bring-up checks that
 * FWSEC fenced off the region. It then copies the bootloader's program, the
 * GSP's firmware and the firmware's signature into memory the card can reach,
 * the firmware into pages listed by a radix3 page table, copies booter load's
 * program, with the chosen signature in its slot, into memory of its own the
 * card can reach, allocates memory for the message queues (the two rings
 * through which the driver and the GSP's firmware exchange messages), writes
 * protected memory's metadata into a page of its own, and writes the log
 * buffers and the arguments the firmware reads when it starts, all as
 * `nvidia::gsp_boot_memory` describes. The arguments are those of the
 * firmware's resource manager (its task that keeps the GPU's objects and
 * carries out the commands to them, called controls) and of LibOS (the small
 * operating system the firmware runs on). The metadata tells booter load where
 * the copies of the bootloader's program, the firmware and its signature lie
 * and where each part of protected memory goes. Bring-up next calls
 * `nvidia::gsp::start`, which allocates the buffer each received message's
 * payload is copied into and lays the queues out in their memory, after which
 * the `nvidia::gsp` carries every message the driver sends or receives.
 * Through it, bring-up queues in the command queue the two messages NVIDIA's
 * driver sends before the GSP starts (the system information and the registry,
 * here with no entry), as `nvidia::queue_boot_messages` describes. The system
 * information says how much video memory the firmware must leave alone for a
 * boot console (a boot framebuffer that may still be drawn in), as
 * `nvidia::boot_console_bytes` works it out from the card's BARs, the boot
 * framebuffer `g_boot_info` records and whether the chip's fuses disable its
 * display. Bring-up then resets the GSP into its RISC-V core, tells the
 * firmware once that both messages wait and writes the address of LibOS's
 * arguments into the GSP falcon's two mailbox registers. NVIDIA's driver does
 * these before booter load too, but resets the GSP before queueing the
 * messages and tells the firmware after each. Bring-up then resets SEC2's
 * falcon, runs booter load on it from its copy with the address of protected
 * memory's metadata in the SEC2 falcon's mailboxes, checks that booter load
 * left 0 in the first, writes the bootloader's app version into the register
 * of the GSP's falcon NVIDIA calls its OS register, and checks that the GSP's
 * RISC-V core, which booter load starts on the bootloader's program, reads as
 * running. Bring-up then links the status queue (the ring the firmware writes
 * its messages into) once the firmware has written the queue's header, as
 * `nvidia::message_queues::link_status_queue` describes, retrying for up to
 * `nvidia::STATUS_QUEUE_LINK_TIMEOUT_NS`. It then waits for the firmware's
 * ready message (NVIDIA's GSP_INIT_DONE) with `nvidia::gsp::await`, taking the
 * messages before it as `nvidia::receive_awaited` describes: the CPU sequencer
 * (the register, delay and falcon steps the firmware sends for the driver to
 * run) runs as soon as its message is read, as `nvidia::run_cpu_sequencer`
 * requires and describes, and the other messages are logged once the wait
 * ends. As NVIDIA's driver waits for the ready message in one poll, every wait
 * for a message until the ready message arrives gets what is left of one
 * `nvidia::RECEIVE_TIMEOUT_NS` deadline set before the first read, the
 * sequencer's runs counting against it, and a wait with none left still reads
 * the queue once. The sequencer is given a deadline
 * `nvidia::DEFAULT_TIMEOUT_NS` after the same start, which its waits count
 * against as `nvidia::run_cpu_sequencer` describes. A sequencer that fails is
 * logged and the wait goes on, as in NVIDIA's driver. Once the ready message
 * arrives with a result of 0, bring-up sends the two requests NVIDIA's driver
 * sends first: `nvidia::confirm_message_version` checks that the firmware
 * accepts the driver's VGX version (the version of NVIDIA's messages the
 * driver builds), and `nvidia::read_static_configuration` reads the video
 * memory regions the firmware reports, where BAR1's root page directory lies,
 * and the handles (the numbers naming the firmware's objects) of the internal
 * client and subdevice, the objects the firmware keeps for the controls that
 * NVIDIA's driver sends on its own behalf. BAR1 is the CPU's window into the
 * card's video memory, and its root page directory is the top level of the
 * page tables the firmware binds BAR1 to. Bring-up logs each region, as
 * reserved when it holds reserved bytes and as usable otherwise, as NVIDIA's
 * driver counts regions. It then reads the root page directory's first entry,
 * the one covering the start of BAR1, with `nvidia::read_video_memory_u64`
 * through BAR0's window (the 1 MiB of BAR0 that reaches video memory without
 * page tables), and logs the entry with BAR1's block register (the register
 * that binds BAR1 to video memory). It then reads the GSP's GPU interrupt
 * vector (NVIDIA's number for one of the GPU's interrupts, not an MSI vector
 * `on_interrupt` receives) from the firmware's kernel interrupt table (for
 * each GPU unit whose interrupts NVIDIA's driver services, the vectors on
 * which they arrive) with `nvidia::read_gsp_interrupt_vector` and logs it with
 * both handles. The firmware signals the driver on that vector, which must
 * name a bit of the CPU interrupt tree (the GPU's registers reporting its
 * interrupts to the CPU) and come from a row holding no bits in the GPU's PMC
 * interrupt register, as `nvidia::read_gsp_interrupt_vector` checks. When
 * NVIDIA's driver keeps the boot console mapped at the start of BAR1, as
 * `nvidia::is_console_kept_at_bar1_start` decides, bring-up maps it there too.
 * The console's memory is then the firmware's video memory region 0 whole, as
 * NVIDIA's driver takes it, and the console's tables are the page tables that
 * map that many bytes from the start of BAR1 onto the same offsets of video
 * memory. Bring-up places the tables with `nvidia::place_console_page_tables`,
 * builds a copy of them on the unprivileged heap with
 * `nvidia::write_console_page_tables`, writes the copy through BAR0's window
 * with `nvidia::write_video_memory` and frees the copy. It then links the
 * tables with `nvidia::link_console_page_tables`, which names them in the root
 * page directory's first entry and then invalidates BAR1's cached
 * translations, as NVIDIA's driver links its own tables. In between, the call
 * reads the entry back, which NVIDIA's driver does not. Bring-up then maps
 * BAR1 into the kernel's address space, checks that BAR1 reads region 0's last
 * 8 bytes as BAR0's window does and that a value written there through BAR1
 * reads back through both, writes the bytes back, checks that BAR1 reads 0
 * just past region 0 and unmaps BAR1. Whether bring-up maps the console or
 * not, it then stops, so `attach()` declines as well.
 * `attach()` fails with a negative error when BAR0 cannot be mapped, the card
 * does not respond, its VBIOS's images are not laid out as
 * `nvidia::read_vbios_layout` requires, a firmware file of an installed
 * directory cannot be opened, the GSP image fails `nvidia::check_gsp_image`,
 * which requires a 64-bit little-endian ELF file whose one .fwversion
 * section holds exactly `FIRMWARE_VERSION` and its NUL, with one
 * .fwimage section and one section of the name the chip table gives for
 * the chip's signature, a name of at most 31 bytes, each section holding
 * at least one byte inside the file, the bootloader, booter load or booter
 * unload is shorter than its header, cannot be read or holds a header that
 * does not describe it, the bootloader's descriptor is too short to hold every
 * field `nvidia::read_bootloader_layout` reads or places a part that ends past
 * the end of the bootloader's program, a booter's descriptors fail
 * `nvidia::read_booter_layout`, which requires them to follow NVIDIA's rules
 * for booters and place the booter's code, data, signature slot and signatures
 * as `nvidia::booter_layout` describes, a booter has more than one signature
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
 * when the memory for booter unload's copy or FWSEC's copy cannot be
 * allocated, when reading booter unload's program or signature fails, or when
 * FWSEC's data does not lead to a place for the FRTS command as
 * `nvidia::prepare_fwsec_frts` requires. Once FWSEC's copy is patched, it
 * fails when, during the reset of the GSP's falcon, the falcon does not finish
 * clearing its memories, or the GSP does not confirm its switch from its
 * RISC-V core to its falcon core, within `nvidia::FALCON_TIMEOUT_NS` of that
 * wait's start. Once the falcon is reset, it fails when the falcon does not
 * finish loading a block of FWSEC, or does not halt, within
 * `nvidia::FALCON_TIMEOUT_NS` of that wait's start. Once the falcon halts,
 * it fails when `nvidia::check_frts_result` finds an FRTS error code other
 * than 0, or protected memory not set up or starting elsewhere than at the
 * region. Once FWSEC has fenced off the region, it fails when the memory for
 * the copies, the message queues, protected memory's metadata, what the
 * firmware reads when it starts or the payloads of the firmware's messages
 * cannot be allocated, reading a firmware file fails or the command queue
 * lacks room for the system information or the registry. Once the messages are
 * queued, it fails when the falcon does not finish clearing its memories
 * within `nvidia::FALCON_TIMEOUT_NS` during the GSP's reset into its RISC-V
 * core. Once the GSP falcon's mailboxes are written, it fails when SEC2's
 * reset times out as the reset of the GSP's falcon can, when SEC2's falcon
 * does not finish loading a block of booter load, or does not halt, within
 * `nvidia::FALCON_TIMEOUT_NS` of that wait's start, when its first mailbox
 * then reads other than 0, or when the GSP's RISC-V core does not read as
 * running. Once the core runs, it fails when the firmware writes no status
 * queue header that `nvidia::message_queues::link_status_queue` accepts within
 * `nvidia::STATUS_QUEUE_LINK_TIMEOUT_NS`, when the ready message has not
 * arrived by a read begun after the waits' shared `nvidia::RECEIVE_TIMEOUT_NS`
 * deadline, or when a message fails a check of
 * `nvidia::message_queues::receive` on each of `nvidia::RECEIVE_TRIES` reads
 * in a row. It fails with the `nvidia::ERR_FIRMWARE` that
 * `nvidia::gsp::to_error` gives when the ready message's result is not 0, the
 * firmware's report that its initialization failed, which NVIDIA's driver also
 * treats as a failed boot. Once that result is 0, `attach()` fails as
 * `nvidia::confirm_message_version` describes, such as when the firmware
 * refuses the VGX version, then as `nvidia::read_static_configuration`
 * describes, and then as `nvidia::read_gsp_interrupt_vector` describes, as
 * when the kernel interrupt table has no row for the GSP. While mapping the
 * console, it fails as `nvidia::place_console_page_tables` describes, as when
 * region 0 leaves no byte of BAR1 past it, with `nvidia::ERR_NO_MEMORY` when
 * the copy of the tables cannot be allocated, with `nvidia::ERR_BAD_STATE`
 * when `nvidia::write_video_memory` refuses the copy or the first entry reads
 * back as another value, with `nvidia::ERR_TIMEOUT` when BAR1's invalidate
 * does not finish within `nvidia::DEFAULT_TIMEOUT_NS`, as
 * `nvidia::link_console_page_tables` describes, with the error
 * `pci_driver::map_bar` returns when BAR1 cannot be mapped, with the error
 * `pci_driver::unmap_bar` returns when BAR1 cannot be unmapped after the check
 * passed, or with `nvidia::ERR_BAD_STATE` when the check through BAR1 reads
 * another value. Bring-up itself never writes to the display engine, and the
 * CPU sequencer's steps write only the registers the firmware names. Once the
 * sequencer has resumed the GSP's firmware, though, the firmware puts all of
 * BAR1 (the CPU's window into the card's video memory) into virtual mode,
 * translated through the firmware's page tables, as NVIDIA's driver expects,
 * so BAR1 no longer reaches what it reached before, such as a boot
 * framebuffer, until `detach()` writes back BAR1's block register, which binds
 * BAR1 to the video memory. When bring-up maps the console, as NVIDIA's driver
 * does for the console's driver to draw through, BAR1 reaches region 0 again
 * and reads 0 just past its end. The card keeps the memory and I/O decoding
 * that `attach()` turned on. A card that `attach()` declines or fails before
 * the first reset of the GSP's falcon keeps only that decoding. A reset that
 * times out leaves the falcon out of reset with its memories still being
 * cleared, or with its switch to the falcon core requested but not confirmed.
 * A card that gets as far as the load also keeps its bus mastering on, and the
 * load leaves its falcon's memory interface letting the DMA engine read system
 * memory by physical address. A load that times out leaves the falcon not
 * started, with its DMA engine possibly still copying, and a run that times
 * out leaves the falcon possibly still running FWSEC, which may yet fence off
 * the FRTS region. Either leaves FWSEC's copy allocated until `detach()`
 * confirms the GSP stopped, and for good when it cannot, since the falcon may
 * still read it. A falcon that halts, after FWSEC ran or after the falcon's
 * boot ROM refused it, is left halted when `attach()` fails before the GSP's
 * reset into its RISC-V core. A load of booter load that times out leaves
 * SEC2's falcon as FWSEC's leaves the GSP's, and a run that times out may
 * leave booter load running, which may later start the GSP's RISC-V core. A
 * failure after the RISC-V core reads as running leaves the core as booter
 * load or the CPU sequencer's steps left it. When `attach()` declines at the
 * end of bring-up, FWSEC ran and fenced off the FRTS region, booter load ran
 * on SEC2, each CPU sequencer the firmware sent ran, or was refused or stopped
 * at a step as it logged, the firmware reported its initialization done,
 * accepted the VGX version, reported its video memory regions, where BAR1's
 * root page directory lies and the handles of the internal client and
 * subdevice, and its kernel interrupt table held a row for the GSP with a
 * vector inside the CPU interrupt tree and no bits in the GPU's PMC interrupt
 * register, and the register of BAR0's window was written back after each use
 * with the value it held before. When bring-up maps the console, the console's
 * tables also lie in video memory and the root page directory's first entry
 * names them. Nothing frees the tables' video memory, which BAR1 translates
 * through until `detach()` writes BAR1's block register back, and for good
 * when it is not written back. A failure after the first entry is written
 * leaves the entry and the tables as they are, and BAR1, once mapped into the
 * kernel's address space for the check, is unmapped again whatever the check
 * found, unless `pci_driver::unmap_bar` fails. Once FWSEC has set up protected
 * memory, it stays set up until `detach()` takes it down with booter unload,
 * as below, or the card is reset, and `nvidia::place_frts_region` refuses the
 * card in every `attach()` until then. The decoding stays on because the boot
 * framebuffer may be in this card's memory, which the CPU reaches only while
 * memory decoding is on.
 * Once the ready message has arrived, whatever its result, `detach()` first
 * asks the GSP's firmware to unload, through the `nvidia::gsp` that `attach()`
 * started, and waits for the GSP's processor to be suspended, as
 * `nvidia::unload_gsp_firmware` describes. `detach()` then frees the payload
 * buffer with `nvidia::gsp::stop` and stops the falcons as below whatever the
 * unload's result, as NVIDIA's driver also resets the GSP after a failed
 * unload.
 * From the first reset of the GSP's falcon on, whether `attach()` declines or
 * fails, `detach()` resets the GSP's falcon again with `nvidia::stop_falcon`
 * and frees all of the GSP's boot memory once the falcon reads as halted and
 * the RISC-V core as not running, leaving the falcon core selected. From the
 * load of booter load on, `detach()` stops SEC2's falcon the same way first,
 * since a booter load still running may start the GSP's RISC-V core again,
 * then stops the GSP whatever SEC2's result, freeing the memory only once both
 * read as stopped. `detach()` fails, keeping all of the boot memory allocated
 * for good, when a reset times out or a falcon then does not read as halted or
 * its RISC-V core reads as running. Once the CPU sequencer's resume has
 * started SEC2's falcon again, that falcon may not read as halted after
 * `detach()` resets it, and `detach()` then fails the same way. From the load
 * of booter load on, once the GSP reads as stopped, whatever SEC2's result,
 * `detach()` writes back BAR1's block register when the register no longer
 * reads as it did before booter load, as `nvidia::restore_bar1_block`
 * describes, and logs the register's values. When the register has changed but
 * the GSP does not read as stopped, `detach()` leaves it unwritten and logs
 * both values. When SEC2 does not read as halted, a booter load still running
 * may start the GSP's RISC-V core again once `detach()` has written the
 * register, and the GSP's firmware may then rewrite it once more, with nothing
 * logged.
 * Once bring-up's run of FWSEC has halted, whether `attach()` then declines or
 * fails, `detach()` runs FWSEC's SB command on the GSP's falcon once each
 * falcon it resets reads as stopped and the boot memory is freed, to put back
 * the programs the VBIOS runs before an operating system loads.
 * `nvidia::run_fwsec_sb` copies FWSEC from the VBIOS again, patches the copy
 * for the SB command, runs it on the GSP's falcon as bring-up runs FWSEC for
 * the FRTS command, and checks that the registers show no failure. A failure
 * is logged without failing `detach()`. A load or run that times out leaves
 * the copy allocated for good, since the falcon may still read it. No second
 * reset frees it, since NVIDIA's driver never resets the GSP after the SB
 * command at unload, and after the run's timeout a reset could cut FWSEC
 * short.
 * From the first reset of the GSP's falcon on, once each falcon `detach()`
 * resets reads as stopped and the boot memory is freed, `detach()` takes
 * protected memory down as `nvidia::take_down_protected_memory` describes,
 * after the SB command whatever its result, or without it when the SB command
 * does not run: while protected memory reads as set up, `detach()` resets
 * SEC2's falcon and runs booter unload on it from its copy, and protected
 * memory must then no longer read as set up. A failure is logged and fails
 * `detach()`. A load or run of booter unload that times out leaves the copy
 * allocated for good, since SEC2 may still read it, and otherwise `detach()`
 * frees the copy, as it does whenever booter unload does not run. When a
 * falcon is not confirmed stopped, booter unload does not run, and protected
 * memory stays as it was.
 * NVIDIA's driver asks the firmware to unload as it destroys the GPU's state,
 * with no check that the ready message arrived, while `detach()` asks only
 * once it has, since a firmware that never reported itself ready is not known
 * to answer and the resets follow either way. NVIDIA's driver then resets the
 * GSP without checking the reset, runs the SB command, logging and asserting
 * on a failure without failing the unload, and, while protected memory is set
 * up, resets SEC2 and runs booter unload on it to take protected memory down,
 * whatever the SB command and the SEC2 reset return. While its thread-state
 * timeout is on, it starts its thread-state budget again before the GSP's
 * reset, so one default timeout bounds every wait of the reset, the SB command
 * and booter unload other than the falcons' ready waits, where each wait here
 * has a timeout of its own. It runs the SB command from the copy of FWSEC it
 * made at bring-up and frees only as the GPU is destroyed, with the signature
 * chosen again for the version the chip's fuses record, and with the FRTS
 * command's region fields left in the input buffer past the SB command's 24
 * bytes. `detach()` resets SEC2 before booter unload, and from the load of
 * booter load on also before the GSP. `detach()` runs the SB command from a
 * fresh copy with the signature `attach()` chose. The command runs only once
 * bring-up's run of FWSEC has halted, since a FWSEC that did not halt for the
 * FRTS command is not expected to halt for this one, and only once each falcon
 * `detach()` resets reads as stopped, since FWSEC needs the GSP's falcon reset
 * and a booter load still running may start the GSP's RISC-V core again.
 * Booter unload also runs only once each falcon `detach()` resets reads as
 * stopped, since a falcon not confirmed stopped is in a state nothing here can
 * account for, and not after a SEC2 reset that times out, since no program is
 * loaded into a falcon that has not finished its reset. `detach()` returns the
 * unload's error ahead of the take-down's, as NVIDIA's driver returns the
 * request's ahead of booter unload's, and the unload's error can be the
 * suspend wait's timeout, which NVIDIA's driver drops, as
 * `nvidia::unload_gsp_firmware` describes.
 * Writing BAR1's block register back and the check through BAR1 are this
 * driver's own steps, and so is failing `attach()` when BAR1's invalidate
 * times out, which NVIDIA's driver ignores. So is failing `attach()` when the
 * root page directory's first entry reads back as another value, since
 * NVIDIA's driver reads back none of the entries it writes into the firmware's
 * root page directory. Bring-up writes only the root page directory's first
 * entry, which covers all of BAR1, where NVIDIA's driver writes every entry of
 * its own root page directory into the firmware's and makes any entries past
 * its own invalid.
 */
class nvidia_gpu_driver : public pci_driver {
public:
    nvidia_gpu_driver(pci::device* dev) : pci_driver("nvidia_gpu", dev) {}

    int32_t attach() override;

    /**
     * @brief Ask the GSP's firmware to unload once its ready message has
     * arrived, then free the payload buffer with `nvidia::gsp::stop`, then
     * stop SEC2 once bring-up has begun loading booter load, then the GSP once
     * bring-up has reset its falcon, freeing the GSP's boot memory only once
     * each reads as stopped and, from the load of booter load on, writing back
     * BAR1's block register once the GSP reads as stopped, then run FWSEC's SB
     * command on the GSP's falcon once bring-up's run of FWSEC has halted and
     * the boot memory is freed, then take protected memory down with booter
     * unload on SEC2 once the boot memory is freed, then free booter unload's
     * copy unless SEC2 may still read it, then unmap BAR0 as
     * `pci_driver::detach()` does. The unload spins on the calling CPU,
     * waiting for the reply as `nvidia::gsp::request` waits, with a deadline
     * `nvidia::RECEIVE_TIMEOUT_NS` after the request, then for up to
     * `nvidia::DEFAULT_TIMEOUT_NS` for the suspend. Each falcon's stop spins
     * for up to twice `nvidia::FALCON_READY_TIMEOUT_NS` and six times
     * `nvidia::FALCON_TIMEOUT_NS` when the falcon is slow to reset. Running
     * FWSEC's SB command spins for up to `nvidia::FALCON_TIMEOUT_NS` for each
     * block's load and for the run, as `nvidia::run_falcon_program` describes.
     * Taking protected memory down spins for SEC2's reset as long as a stop
     * can, then for booter unload's loads and run as long as the SB command's.
     * @return What `pci_driver::detach()` returns once the falcons are stopped
     * or need no stop, `nvidia::ERR_TIMEOUT` when a stop's reset times out, or
     * `nvidia::ERR_BAD_STATE` when a falcon then does not read as halted or
     * its RISC-V core reads as running, each error keeping the boot memory
     * allocated for good. When SEC2's stop fails, `detach()` still stops the
     * GSP's falcon and returns SEC2's error. When the stops succeed after an
     * unload that failed, the unload's error is returned, what
     * `nvidia::unload_gsp_firmware` returns. When the stops and the unload
     * succeed and taking protected memory down fails, the
     * `nvidia::ERR_TIMEOUT` or `nvidia::ERR_BAD_STATE` that
     * `nvidia::take_down_protected_memory` returns is returned. Whatever is
     * returned, booter unload's copy stays allocated for good only after a
     * load or run of booter unload that timed out. The cause is logged. A
     * failure of the SB command is logged and changes no result. A later call
     * sends no request, stops no falcon, runs no FWSEC or booter unload and
     * writes no register again, and returns what `pci_driver::detach()`
     * returns.
     */
    int32_t detach() override;

    void run() override;

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void on_interrupt(uint32_t vector) override;

private:
    int32_t map_boot_console(
        nvidia::registers& regs,
        const nvidia::static_configuration& configuration,
        uint64_t reserved_console_bytes,
        uint64_t bar1_bytes
    );
    int32_t tear_down_gsp();
    int32_t unload_firmware(nvidia::registers& regs);
    void run_fwsec_sb(nvidia::registers& regs);
    int32_t take_down_protected_memory(nvidia::registers& regs);

    nvidia::gsp_boot_memory       m_boot_memory;
    nvidia::gsp                   m_gsp;
    nvidia::fwsec_layout          m_fwsec = {};
    nvidia::fwsec_signature       m_fwsec_signature = {};
    dma::buffer                   m_booter_unload_copy = {};
    nvidia::booter_layout         m_booter_unload_parts = {};
    const nvidia::chip_info*      m_chip = nullptr;
    uint32_t                      m_boot0 = 0;
    uint32_t                      m_bar1_block = 0;
    bool                          m_must_stop_gsp = false;
    bool                          m_must_stop_sec2 = false;
    bool                          m_must_unload = false;
    bool                          m_must_run_fwsec_sb = false;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NVIDIA_GPU_H
