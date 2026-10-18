// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_FALCON_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_FALCON_H

#include "common/types.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

// How long a reset waits for a falcon to report it is ready for one before going on without the report, 150 us as
// in NVIDIA's driver
constexpr uint64_t FALCON_READY_TIMEOUT_NS = 150000;

// How long each wait for a falcon's memories, its core selection, a block of a program's load or a falcon's halt may
// take outside the CPU sequencer's steps, NVIDIA's default timeout
constexpr uint64_t FALCON_TIMEOUT_NS = DEFAULT_TIMEOUT_NS;

/**
 * A program for a falcon, which the falcon's boot ROM runs only once the
 * program's signature checks out. The program lies in system memory from
 * `dma_address`, its code and its data each at an offset of its own. The
 * falcon runs code from IMEM, its code memory, and keeps data in DMEM, its
 * data memory. A physical base says where in IMEM or DMEM a part goes. The
 * code's blocks in IMEM are also tagged with the virtual addresses the code
 * runs at, starting from its virtual base.
 */
struct falcon_program {
    uint64_t dma_address;         // the program's first byte, as the card addresses memory
    bool     is_cached;           // whether the CPU maps the program's memory cached, so its writes may sit in caches
    bool     reads_cached_memory; // whether the program, once started, may read other memory the CPU maps cached
    uint32_t code_offset;         // bytes from the program's first byte to its code
    uint32_t code_size;           // bytes
    uint32_t imem_physical_base;  // bytes from the start of IMEM, of which the DMA engine keeps the low 24 bits
    uint32_t imem_virtual_base;   // the address the code starts running at
    uint32_t data_offset;         // bytes from the program's first byte to its data
    uint32_t data_size;           // bytes
    uint32_t dmem_physical_base;  // bytes from the start of DMEM, of which the DMA engine keeps the low 24 bits
    uint32_t signature_offset;    // bytes from the start of the data to the signature the boot ROM checks
    uint32_t engine_mask;         // the engines the program is signed to run on
    uint8_t  ucode_id;            // NVIDIA's number for the program among the falcon's programs
};

// The values a program finds in its falcon's two mailbox registers as it starts, and those it leaves in them
struct falcon_mailboxes {
    uint32_t mailbox0;
    uint32_t mailbox1;
};

// Identifies one of the GPU's falcons, the microcontrollers in its engines: the GSP's, which runs FWSEC, or that of
// SEC2, the GPU's security processor, which runs the programs that boot the GSP and take protected memory down
enum class falcon_id : uint8_t {
    gsp,
    sec2,
};

/**
 * @brief Reset `falcon` into the state NVIDIA's driver loads a signed program
 * into. A falcon has a falcon core and, when its hardware configuration says
 * so, a RISC-V core, in which case it runs whichever of the two its RISC-V
 * boot control register selects. The programs `run_falcon_program` loads run
 * on the falcon core.
 * NVIDIA's driver resets a falcon by disabling and then enabling it, and
 * neither the GSP's falcon nor SEC2's has a bit in the chip's engine enable
 * register, so the disable and the enable are each a full reset. Each full
 * reset gives the falcon up to `ready_timeout_ns` to report it is ready for a
 * reset, going on without the report when it does not come, sets and then
 * releases the falcon's engine reset, waits for the falcon to finish clearing
 * its code and data memories, IMEM and DMEM, and selects the falcon core if
 * the RISC-V core is selected. After both full resets, as NVIDIA's reset
 * ends, it selects the falcon core again if the RISC-V core is selected,
 * waits for the memories once more, and writes `boot0` to the falcon's RM
 * register, which NVIDIA's driver sets to the chip's `PMC_BOOT_0` value as it
 * finishes enabling a falcon.
 * The waits spin on the calling CPU instead of sleeping, so a slow falcon can
 * hold the CPU for up to twice `ready_timeout_ns` and six times `timeout_ns`.
 * @param regs A view of BAR0. Before a reset of the GSP's falcon, the card's
 * boot firmware must have finished, as `wait_for_boot_firmware` waits for,
 * since the GSP runs it and the reset would stop it.
 * @param falcon The falcon to reset.
 * @param boot0 The chip's `PMC_BOOT_0` value.
 * @param ready_timeout_ns How long each wait for the ready report may take,
 * `FALCON_READY_TIMEOUT_NS` at bring-up.
 * @param timeout_ns How long each wait for the memories or the core selection
 * may take, `FALCON_TIMEOUT_NS` at bring-up, or the time left until
 * `cpu_sequencer_context::deadline_ns` for the CPU sequencer's reset step.
 * Each wait reads its register at least once, even for a timeout of 0.
 * NVIDIA's driver counts its default timeout once for a whole call while its
 * thread-state timeout is on, and for each wait otherwise.
 * @return `OK`, or `ERR_TIMEOUT` when the falcon has not finished clearing
 * its memories, or has not confirmed its switch to the falcon core, within
 * `timeout_ns` of that wait's start. NVIDIA's driver goes on after either
 * wait runs out, so both failures are this driver's, to stop bring-up before
 * a program is loaded into a falcon that has not finished its reset. The
 * cause is logged, and the RM register is left unwritten.
 */
int32_t reset_falcon(
    registers& regs,
    falcon_id falcon,
    uint32_t boot0,
    uint64_t ready_timeout_ns,
    uint64_t timeout_ns
);

/**
 * @brief Whether the GSP has a RISC-V core, the core its firmware runs on, as
 * its falcon's hardware configuration register says. NVIDIA's driver boots
 * the GSP's firmware only on a GSP that has one, which it checks before FWSEC
 * runs.
 * @param regs A view of BAR0.
 * @return Whether the RISC-V field of the falcon's hardware configuration
 * register is set.
 */
bool has_gsp_riscv_core(const registers& regs);

/**
 * @brief Reset the GSP into its RISC-V core, as NVIDIA's driver does once
 * FWSEC has run and before it queues its first message for the GSP's
 * firmware. The reset gives the falcon up to `ready_timeout_ns` to report it
 * is ready for a reset, going on without the report when it does not come,
 * sets and then releases the GSP's engine reset, waits for the falcon to
 * finish clearing IMEM and DMEM, and then selects the RISC-V core in its boot
 * control register, also setting the register's field NVIDIA's driver calls
 * BRFETCH. The core is left unstarted.
 * The waits spin on the calling CPU instead of sleeping, so a slow falcon can
 * hold the CPU for up to `ready_timeout_ns` and `timeout_ns`.
 * @param regs A view of BAR0. The card's boot firmware must have finished, as
 * `wait_for_boot_firmware` waits for, since the GSP runs it and the reset
 * would stop it. The GSP must have a RISC-V core, as `has_gsp_riscv_core`
 * reports, since the reset selects that core.
 * @param ready_timeout_ns How long the wait for the ready report may take,
 * `FALCON_READY_TIMEOUT_NS` at bring-up.
 * @param timeout_ns How long the wait for the memories may take,
 * `FALCON_TIMEOUT_NS` at bring-up, or the time left until
 * `cpu_sequencer_context::deadline_ns` when `resume_gsp_firmware` resets the
 * GSP for the CPU sequencer's resume step. The wait reads its register at
 * least once, even for a timeout of 0. NVIDIA's driver counts its default
 * timeout once for a whole call while its thread-state timeout is on, and for
 * each wait otherwise.
 * @return `OK`, or `ERR_TIMEOUT` when the falcon has not finished clearing
 * its memories within `timeout_ns`, where NVIDIA's driver stops too. The
 * cause is logged, and the boot control register is left unwritten.
 */
int32_t reset_gsp_into_riscv(registers& regs, uint64_t ready_timeout_ns, uint64_t timeout_ns);

/**
 * @brief Tell the GSP's firmware that messages wait in the command queue, by
 * writing 0 to the queue head register of its resource manager (the
 * firmware's task that serves the driver's requests), as NVIDIA's driver does
 * after each message it queues.
 * @param regs A view of BAR0. The GSP must have been reset into its RISC-V
 * core with `reset_gsp_into_riscv`, which NVIDIA's driver does before it
 * queues the first message.
 */
void notify_gsp_of_queued_messages(registers& regs);

/**
 * @brief Write the address of the arguments of LibOS (the small operating
 * system the GSP's firmware runs on) into the GSP falcon's two mailbox
 * registers, the low 32 bits into the first and the high 32 bits into the
 * second. NVIDIA's driver writes them before SEC2 runs booter load, the
 * program that boots the GSP's firmware.
 * @param regs A view of BAR0. The GSP must have been reset into its RISC-V
 * core with `reset_gsp_into_riscv`, after which NVIDIA's driver writes them
 * too, since a later reset of the GSP clears both registers.
 * @param address The arguments' first byte, as the card addresses memory.
 */
void write_libos_arguments_address(registers& regs, uint64_t address);

/**
 * @brief Whether `falcon` is halted, as the GSP's falcon is once the card's
 * boot firmware has finished with it, and as a falcon is once a program run
 * on it stops.
 * @param regs A view of BAR0.
 * @param falcon The falcon to read.
 * @return Whether the halted bit of the falcon's control register is set.
 */
bool is_falcon_halted(const registers& regs, falcon_id falcon);

/**
 * @brief Wait for the GSP's processor (its RISC-V core, which runs the
 * firmware) to be suspended, as NVIDIA's driver waits once it has asked the
 * GSP's firmware to unload: until the GSP falcon's first mailbox reads
 * exactly 0x80000000, which NVIDIA's driver reads as the report of LibOS (the
 * small operating system the firmware runs on) that the processor is
 * suspended, or until `timeout_ns` passes. The mailbox is read once more
 * after the deadline, as NVIDIA's waits for a condition read it. The call
 * spins on the calling CPU without sleeping.
 * @param regs A view of BAR0.
 * @param timeout_ns How long to wait, `DEFAULT_TIMEOUT_NS` at unload, the
 * default timeout NVIDIA's wait takes.
 * @return `OK` once the mailbox reads 0x80000000, or `ERR_TIMEOUT`, logged
 * with what the mailbox then reads, when it did not by the last read.
 */
int32_t wait_for_gsp_suspend(const registers& regs, uint64_t timeout_ns);

/**
 * @brief Stop `falcon`: reset it as `reset_falcon` does, then confirm that
 * the falcon reads as halted and, when the falcon has a RISC-V core, that the
 * core does not read as running, as the active field of the core's control
 * register shows. NVIDIA's driver gives the GSP's falcon this reset as it
 * unloads the firmware, after waiting for the firmware to suspend the GSP's
 * processor, whether or not the wait succeeds. This driver stops a falcon
 * with it before freeing memory the falcon may still read.
 * The waits spin on the calling CPU instead of sleeping, so a slow falcon can
 * hold the CPU for up to twice `ready_timeout_ns` and six times `timeout_ns`.
 * @param regs A view of BAR0, as for `reset_falcon`.
 * @param falcon The falcon to stop.
 * @param boot0 The chip's `PMC_BOOT_0` value, for the reset.
 * @param ready_timeout_ns How long each wait for the ready report may take,
 * as for `reset_falcon`.
 * @param timeout_ns How long each wait for the memories or the core selection
 * may take, as for `reset_falcon`.
 * @return `OK` once the falcon reads as stopped, `ERR_TIMEOUT` when the reset
 * times out as `reset_falcon` describes, or `ERR_BAD_STATE` when, after the
 * reset, the falcon does not read as halted or its RISC-V core reads as
 * running. The cause is logged.
 */
int32_t stop_falcon(registers& regs, falcon_id falcon, uint32_t boot0, uint64_t ready_timeout_ns, uint64_t timeout_ns);

/**
 * @brief Load `program` into `falcon` and run it until the falcon halts. The
 * falcon's DMA engine copies the code into IMEM and then the data into DMEM,
 * 256 bytes at a time, reading system memory by its physical addresses. The
 * reads look in the CPU's caches only when the program is cached or may, once
 * started, read other cached memory through the same DMA settings, since
 * memory the CPU writes uncached holds every write already. The falcon's boot
 * ROM is then given the signature's offset in the data, the engine mask, the
 * ucode id and the signature's algorithm, RSA-3072, the falcon's boot vector
 * is set to the code's virtual base, and the falcon is started. The boot ROM
 * checks the signature and runs the program, and the falcon halts when the
 * program ends or when the boot ROM refuses it, so a halt says nothing of the
 * program's result.
 * The waits spin on the calling CPU instead of sleeping, so a slow falcon can
 * hold the CPU for up to `timeout_ns` for each block and for the run.
 * @param regs A view of BAR0. The card's bus mastering must be on, since the
 * DMA engine reads system memory, and `falcon` must have been reset with
 * `reset_falcon`, since the load needs its memories cleared and its falcon
 * core selected.
 * @param falcon The falcon to run the program on.
 * @param program The program. It must lie in system memory below 512 TiB,
 * since the DMA engine takes its addresses in 256-byte units and its two base
 * registers hold 41 bits of them, so `dma_address` plus `code_offset` less
 * `imem_virtual_base`, and `dma_address` plus `data_offset`, must also be
 * multiples of 256. The DMA engine reads whole blocks, so the memory must
 * hold each part rounded up to 256 bytes, and must stay allocated while the
 * falcon may read it: until this returns `OK`, and after any other result
 * until the falcon is reset and confirmed halted, as `stop_falcon` does.
 * @param timeout_ns How long each block's load and the run may take,
 * `FALCON_TIMEOUT_NS` at bring-up. Each wait reads its register at least
 * once, even for a timeout of 0. NVIDIA's driver counts its default timeout
 * once for a whole call while its thread-state timeout is on, and for each
 * wait otherwise.
 * @return `OK` once the falcon halts, or `ERR_TIMEOUT` when a block's load
 * does not finish, or the falcon does not halt, within `timeout_ns` of that
 * wait's start. The cause is logged. The falcon is started only after every
 * block is loaded.
 */
int32_t run_falcon_program(registers& regs, falcon_id falcon, const falcon_program& program, uint64_t timeout_ns);

/**
 * @brief Run `program` on `falcon` as the other `run_falcon_program` does,
 * also writing `mailboxes` into the falcon's two mailbox registers after the
 * boot vector and before the start, and reading both registers back into
 * `mailboxes` after the wait for the halt, whether or not it timed out, as
 * NVIDIA's driver does for a program it hands arguments to.
 * @param regs A view of BAR0, as for the other `run_falcon_program`.
 * @param falcon The falcon to run the program on.
 * @param program The program, as for the other `run_falcon_program`.
 * @param mailboxes The values to write, which receive what the registers hold
 * after the halt wait, and keep the values to write when a block's load times
 * out, since the falcon is then not started.
 * @param timeout_ns How long each block's load and the run may take, as for
 * the other `run_falcon_program`.
 * @return As the other `run_falcon_program` returns. When the falcon does not
 * halt, what its mailboxes then read is logged as well.
 */
int32_t run_falcon_program(
    registers& regs,
    falcon_id falcon,
    const falcon_program& program,
    falcon_mailboxes& mailboxes,
    uint64_t timeout_ns
);

/**
 * @brief Run booter load, the program that starts the GSP's firmware, on
 * SEC2's falcon with `run_falcon_program`, handing it `metadata_address` in
 * the SEC2 falcon's two mailboxes, its low 32 bits in the first and its high
 * 32 bits in the second, and check that booter load leaves 0 in the first, as
 * NVIDIA's driver does.
 * @param regs A view of BAR0, as for `run_falcon_program` on SEC2's falcon:
 * the card's bus mastering must be on and SEC2's falcon must have been reset
 * with `reset_falcon`.
 * @param booter_load Booter load's program, as `describe_booter_program` gives
 * it, whose memory must stay allocated as `run_falcon_program` requires.
 * @param metadata_address Where protected memory's metadata lies, as the card
 * addresses memory.
 * @param timeout_ns How long each block's load and the run may take, as for
 * `run_falcon_program`.
 * @return `OK` once booter load halts with 0 in the first mailbox, the
 * `ERR_TIMEOUT` that `run_falcon_program` returns, or `ERR_BAD_STATE` when the
 * first mailbox reads other than 0 after the halt. The cause is logged.
 */
int32_t run_booter_load(
    registers& regs,
    const falcon_program& booter_load,
    uint64_t metadata_address,
    uint64_t timeout_ns
);

/**
 * @brief Run booter unload, the program that takes protected memory down, on
 * SEC2's falcon with `run_falcon_program`, handing it 0xFF in both of the SEC2
 * falcon's mailboxes, and check that booter unload leaves 0 in the first, as
 * NVIDIA's driver does at an unload outside a power transition.
 * @param regs A view of BAR0, as for `run_falcon_program` on SEC2's falcon:
 * the card's bus mastering must be on and SEC2's falcon must have been reset
 * with `reset_falcon`.
 * @param booter_unload Booter unload's program, as `describe_booter_program`
 * gives it, whose memory must stay allocated as `run_falcon_program` requires.
 * @param timeout_ns How long each block's load and the run may take, as for
 * `run_falcon_program`.
 * @return `OK` once booter unload halts with 0 in the first mailbox, the
 * `ERR_TIMEOUT` that `run_falcon_program` returns, or `ERR_BAD_STATE` when the
 * first mailbox reads other than 0 after the halt. The cause is logged.
 */
int32_t run_booter_unload(registers& regs, const falcon_program& booter_unload, uint64_t timeout_ns);

/**
 * @brief Write `app_version` into the register of the GSP's falcon NVIDIA
 * calls its OS register, as NVIDIA's driver does once booter load succeeds.
 * @param regs A view of BAR0.
 * @param app_version The app version of the bootloader's program, as
 * `read_bootloader_layout` returns it.
 */
void write_gsp_app_version(registers& regs, uint32_t app_version);

/**
 * @brief Whether the GSP's RISC-V core is running, as the active field of the
 * core's control register shows. NVIDIA's driver reads it once booter load
 * succeeds and fails the GSP's boot when the core is not running.
 * @param regs A view of BAR0.
 * @return Whether the active field is set.
 */
bool is_gsp_riscv_running(const registers& regs);

/**
 * @brief Let `falcon`'s DMA engine use physical addresses with no address
 * space bound to the falcon, by setting the field of its memory interface's
 * control register that allows it and clearing its DMA control register.
 * `run_falcon_program` does this before loading a program. NVIDIA's driver
 * also does it after each reset of the GSP's falcon that the CPU sequencer
 * asks for. The CPU sequencer is a list of register, delay and falcon steps
 * the GSP's firmware sends for the driver to run.
 * @param regs A view of BAR0.
 * @param falcon The falcon whose DMA engine is set up.
 */
void allow_falcon_dma_without_context(registers& regs, falcon_id falcon);

/**
 * @brief Start `falcon` at the boot vector it holds, through its control
 * register's alias while the register's alias enable bit is set and through
 * the register itself otherwise, as NVIDIA's driver starts a falcon.
 * `run_falcon_program` starts its falcon this way, and the CPU sequencer asks
 * for it on the GSP's falcon.
 * @param regs A view of BAR0.
 * @param falcon The falcon to start.
 */
void start_falcon(registers& regs, falcon_id falcon);

/**
 * @brief Wait for `falcon` to halt, a step the CPU sequencer asks for on the
 * GSP's falcon. The wait spins on the calling CPU without sleeping, holding it
 * for up to `timeout_ns` and then until its last read returns.
 * @param regs A view of BAR0.
 * @param falcon The falcon to wait for.
 * @param timeout_ns How long the wait may take, the time left until
 * `cpu_sequencer_context::deadline_ns` for the CPU sequencer's halt step,
 * which NVIDIA's driver gives its default timeout. The wait reads its register
 * at least once, even for a timeout of 0. NVIDIA's driver counts its default
 * timeout once for a whole call while its thread-state timeout is on, and for
 * each wait otherwise.
 * @return `OK` once the falcon's halted bit is set, or `ERR_TIMEOUT`, which is
 * logged, when it is not set within `timeout_ns`.
 */
int32_t wait_for_falcon_halt(const registers& regs, falcon_id falcon, uint64_t timeout_ns);

/**
 * @brief Have SEC2 resume the GSP's firmware, the CPU sequencer's resume step.
 * As in NVIDIA's driver, the GSP is reset into its RISC-V core as
 * `reset_gsp_into_riscv` does, the address of LibOS's arguments is written
 * into its falcon's mailboxes, and SEC2's falcon is started as booter load
 * left it, with no reset, for SEC2 to resume the firmware. The call then waits
 * for the field of a secure scratch register that NVIDIA's driver reads as the
 * firmware resumed, checks that the SEC2 falcon's first mailbox reads 0, gives
 * the GSP falcon's OS register `app_version` and checks that the RISC-V core
 * runs. It spins on the calling CPU without sleeping, holding it for up to
 * `ready_timeout_ns` and twice `timeout_ns`, and then until its last read
 * returns.
 * @param regs A view of BAR0, as for `reset_gsp_into_riscv`. Booter load must
 * also have run on SEC2's falcon, as `run_booter_load` runs it, with nothing
 * resetting SEC2 since.
 * @param libos_arguments_address As for `write_libos_arguments_address`.
 * @param app_version As for `write_gsp_app_version`.
 * @param ready_timeout_ns As for `reset_gsp_into_riscv`.
 * @param timeout_ns How long the reset's wait for the memories and the wait
 * for the scratch register's field may each take, the time left until
 * `cpu_sequencer_context::deadline_ns` for the CPU sequencer's resume step,
 * where NVIDIA's driver gives both waits its default timeout. Each wait reads
 * its register at least once, even for a timeout of 0, and the field is read
 * once more after its deadline passes, as NVIDIA's driver reads it. NVIDIA's
 * wait for the memories also reads its register once more after its deadline,
 * which the wait in `reset_gsp_into_riscv` does not. NVIDIA's driver counts
 * its default timeout once for a whole call while its thread-state timeout is
 * on, and for each wait otherwise.
 * @return `OK`, `ERR_TIMEOUT` when the reset times out as
 * `reset_gsp_into_riscv` describes, leaving the mailboxes unwritten and SEC2
 * not started, or when the field is not set within `timeout_ns`, SEC2 left
 * started, or `ERR_BAD_STATE` when the SEC2 falcon's first mailbox then reads
 * other than 0 or the RISC-V core does not read as running. The cause is
 * logged.
 */
int32_t resume_gsp_firmware(
    registers& regs,
    uint64_t libos_arguments_address,
    uint32_t app_version,
    uint64_t ready_timeout_ns,
    uint64_t timeout_ns
);

/**
 * @brief Read the version the chip's fuses, bits burned into the chip for
 * good, record for a program that `falcon` runs. The program's ucode id,
 * NVIDIA's number for it among that falcon's programs, names one of the
 * falcon's 16 fuse registers, from 1 for the first. The version is the bit
 * number of that register's highest set bit plus 1, or 0 when no bit is set
 * or the ucode id names none of the registers.
 * @param regs A view of BAR0.
 * @param falcon The falcon whose fuse registers record the program's version.
 * @param ucode_id The program's ucode id, in all 32 bits NVIDIA's driver
 * takes it in, so an id past 16 names no register.
 * @return The version, from 0 to 32.
 */
uint32_t read_ucode_fuse_version(const registers& regs, falcon_id falcon, uint32_t ucode_id);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_FALCON_H
