// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_FALCON_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_FALCON_H

#include "common/types.h"
#include "drivers/graphics/nvidia/registers.h"

namespace drivers::nvidia {

// How long a reset waits for a falcon to report it is ready for one before going on without the report, 150 us as
// in NVIDIA's driver
constexpr uint64_t FALCON_READY_TIMEOUT_NS = 150000;

// How long each wait for a falcon's memories, its core selection, a block of a program's load or a program's halt
// may take, NVIDIA's default timeout of 4 s for a GPU in graphics mode rather than compute mode
constexpr uint64_t FALCON_TIMEOUT_NS = 4000000000;

/**
 * A program for a falcon, which the falcon's boot ROM runs only once the
 * program's signature checks out. The program lies in system memory, its code
 * at `dma_address` and its data right after the code. The falcon runs code
 * from IMEM, its code memory, and keeps data in DMEM, its data memory. A
 * physical base says where in IMEM or DMEM a part goes. The code's blocks in
 * IMEM are also tagged with the virtual addresses the code runs at, starting
 * from its virtual base.
 */
struct falcon_program {
    uint64_t dma_address;        // the program's first byte, as the card addresses memory
    bool     is_cached;          // whether the CPU maps the program's memory cached, so its writes may sit in caches
    uint32_t code_size;          // bytes
    uint32_t imem_physical_base; // bytes from the start of IMEM, of which the DMA engine keeps the low 24 bits
    uint32_t imem_virtual_base;  // the address the code starts running at
    uint32_t data_size;          // bytes
    uint32_t dmem_physical_base; // bytes from the start of DMEM, of which the DMA engine keeps the low 24 bits
    uint32_t signature_offset;   // bytes from the start of the data to the signature the boot ROM checks
    uint16_t engine_mask;        // the engines the program is signed to run on
    uint8_t  ucode_id;           // NVIDIA's number for the program among the falcon's programs
};

// Identifies one of the GPU's falcons, the microcontrollers in its engines: the GSP's, which runs FWSEC, or that of
// SEC2, the GPU's security processor, which runs the program that boots the GSP's firmware
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
 * may take, `FALCON_TIMEOUT_NS` at bring-up. Each wait reads its register at
 * least once, even for a timeout of 0. NVIDIA's driver counts its default
 * timeout once for a whole call while its thread-state timeout is on, and for
 * each wait otherwise.
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
 * `FALCON_TIMEOUT_NS` at bring-up. The wait reads its register at least once,
 * even for a timeout of 0. NVIDIA's driver counts its default timeout once
 * for a whole call while its thread-state timeout is on, and for each wait
 * otherwise.
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
 * reads look in the CPU's caches only when the program is cached, since
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
 * registers hold 41 bits of them, so `dma_address` less `imem_virtual_base`
 * and `dma_address` plus `code_size` must also be multiples of 256. The DMA
 * engine reads whole blocks, so the memory must hold each part rounded up to
 * 256 bytes, and must stay allocated while the falcon may read it: until this
 * returns `OK`, and after any other result until the falcon is reset and
 * confirmed halted, as `stop_falcon` does.
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
