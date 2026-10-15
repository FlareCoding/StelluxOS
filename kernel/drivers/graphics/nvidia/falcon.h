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
 * A program for the GSP's falcon, which the falcon's boot ROM runs only once
 * the program's signature checks out. The program lies in system memory, its
 * code at `dma_address` and its data right after the code. The falcon runs
 * code from IMEM, its code memory, and keeps data in DMEM, its data memory. A
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

/**
 * @brief Reset the GSP's falcon, the microcontroller in the GSP that runs
 * FWSEC, into the state NVIDIA's driver loads FWSEC into. The falcon has a
 * falcon core and, when its hardware configuration says so, a RISC-V core,
 * and it runs whichever core its RISC-V boot control register selects.
 * FWSEC runs on the falcon core.
 * NVIDIA's driver resets a falcon by disabling and then enabling it, and the
 * GSP's falcon has no bit in the chip's engine enable register, so each of
 * the two is a full reset. Each full reset gives the falcon up to
 * `ready_timeout_ns` to report it is ready for a reset, going on without the
 * report when it does not come, sets and then releases the GSP's engine
 * reset, waits for the falcon to finish clearing its code and data memories,
 * IMEM and DMEM, and selects the falcon core if the RISC-V core is
 * selected. After both full resets, as NVIDIA's reset ends, it selects the
 * falcon core again if the RISC-V core is selected, waits for the memories
 * once more, and writes `boot0` to the falcon's RM register, which NVIDIA's
 * driver sets to the chip's `PMC_BOOT_0` value as it finishes enabling a
 * falcon.
 * The waits spin on the calling CPU instead of sleeping, so a slow falcon can
 * hold the CPU for up to twice `ready_timeout_ns` and six times `timeout_ns`.
 * @param regs A view of BAR0. The card's boot firmware must have finished, as
 * `wait_for_boot_firmware` waits for, since the GSP runs it and the reset
 * would stop it.
 * @param boot0 The chip's `PMC_BOOT_0` value.
 * @param ready_timeout_ns How long each wait for the ready report may take,
 * `FALCON_READY_TIMEOUT_NS` at bring-up.
 * @param timeout_ns How long each wait for the memories or the core selection
 * may take, `FALCON_TIMEOUT_NS` at bring-up. Each wait reads its register at
 * least once, even for a timeout of 0. NVIDIA's driver counts its default
 * timeout once for a whole call while its thread-state timeout is on, and for
 * each wait otherwise.
 * @return `OK`, or `ERR_TIMEOUT` when the falcon has not finished clearing
 * its memories, or the GSP has not confirmed its switch to the falcon core,
 * within `timeout_ns` of that wait's start. NVIDIA's driver goes on after
 * either wait runs out, so both failures are this driver's, to stop bring-up
 * before FWSEC is loaded into a falcon that has not finished its reset. The
 * cause is logged, and the RM register is left unwritten.
 */
int32_t reset_gsp_falcon(registers& regs, uint32_t boot0, uint64_t ready_timeout_ns, uint64_t timeout_ns);

/**
 * @brief Whether the GSP's falcon is halted, as it is once the card's boot
 * firmware has finished with it and once a program run on it stops.
 * @param regs A view of BAR0.
 * @return Whether the halted bit of the falcon's control register is set.
 */
bool is_gsp_falcon_halted(const registers& regs);

/**
 * @brief Load `program` into the GSP's falcon and run it until the falcon
 * halts. The falcon's DMA engine copies the code into IMEM and then the data
 * into DMEM, 256 bytes at a time, reading system memory by its physical
 * addresses. The reads look in the CPU's caches only when the program is
 * cached, since memory the CPU writes uncached holds every write already. The
 * falcon's boot ROM is then given the signature's offset in the data, the
 * engine mask, the ucode id and the signature's algorithm, RSA-3072, the
 * falcon's boot vector is set to the code's virtual base, and the falcon is
 * started. The boot ROM checks the signature and runs the program, and the
 * falcon halts when the program ends or when the boot ROM refuses it, so a
 * halt says nothing of the program's result.
 * The waits spin on the calling CPU instead of sleeping, so a slow falcon can
 * hold the CPU for up to `timeout_ns` for each block and for the run.
 * @param regs A view of BAR0. The card's bus mastering must be on, since the
 * DMA engine reads system memory, and the GSP's falcon must have been reset
 * with `reset_gsp_falcon`, since the load needs its memories cleared and its
 * falcon core selected.
 * @param program The program. It must lie in system memory below 512 TiB,
 * since the DMA engine takes its addresses in 256-byte units and its two base
 * registers hold 41 bits of them, so `dma_address` less `imem_virtual_base`
 * and `dma_address` plus `code_size` must also be multiples of 256. The DMA
 * engine reads whole blocks, so the memory must hold each part rounded up to
 * 256 bytes, and must stay allocated while the falcon may read it: until this
 * returns `OK`, and for good after any other result.
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
int32_t run_gsp_falcon_program(registers& regs, const falcon_program& program, uint64_t timeout_ns);

} // namespace drivers::nvidia

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_FALCON_H
