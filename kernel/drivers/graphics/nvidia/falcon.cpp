// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "hw/cpu.h"

namespace drivers::nvidia {

// The base of the GSP's falcon registers, and of its second register block, which holds its RISC-V core's registers
constexpr uint32_t GSP_FALCON_BASE  = 0x110000;
constexpr uint32_t GSP_FALCON2_BASE = 0x111000;

// The GSP's engine register, whose bit 0 holds the GSP in reset while set, and the reads of it NVIDIA's driver makes
// after each write so the reset signal propagates
constexpr uint32_t GSP_FALCON_ENGINE       = 0x1103C0;
constexpr uint32_t ENGINE_RESET            = 0x1;
constexpr uint32_t RESET_PROPAGATION_READS = 10;

// The falcon's hardware configuration register: bit 10 set when the GSP has a RISC-V core, bit 12 set while the
// falcon clears its memories after a reset, bit 31 set once the falcon is ready for a reset
constexpr uint32_t GSP_FALCON_HWCFG2 = GSP_FALCON_BASE + 0x0F4;
constexpr uint32_t HAS_RISCV_CORE    = 0x400;
constexpr uint32_t CLEARING_MEMORIES = 0x1000;
constexpr uint32_t READY_FOR_RESET   = 0x80000000;

// The falcon register NVIDIA's driver writes the chip's `PMC_BOOT_0` value to as a reset ends
constexpr uint32_t GSP_FALCON_RM = GSP_FALCON_BASE + 0x084;

// The RISC-V core's boot control register: bit 4 set while the RISC-V core is selected over the falcon core, bit 0
// set once a selection has taken effect, and the value NVIDIA's driver writes to select the falcon core
constexpr uint32_t GSP_RISCV_BCR_CTRL  = GSP_FALCON2_BASE + 0x668;
constexpr uint32_t RISCV_CORE_SELECTED = 0x10;
constexpr uint32_t CORE_SELECT_VALID   = 0x1;
constexpr uint32_t SELECT_FALCON_CORE  = 0x0;

// The falcon's control register: writing bit 1 starts the falcon, bit 4 is set while it is halted, and while bit 6
// is set the falcon is started through the register's alias instead
constexpr uint32_t GSP_FALCON_CPUCTL       = GSP_FALCON_BASE + 0x100;
constexpr uint32_t GSP_FALCON_CPUCTL_ALIAS = GSP_FALCON_BASE + 0x130;
constexpr uint32_t START_FALCON            = 0x2;
constexpr uint32_t FALCON_HALTED           = 0x10;
constexpr uint32_t STARTS_THROUGH_ALIAS    = 0x40;

// Where the falcon starts running, as a virtual address of its code
constexpr uint32_t GSP_FALCON_BOOTVEC = GSP_FALCON_BASE + 0x104;

// The DMA engine's control register, cleared so a copy needs no address space bound to the falcon
constexpr uint32_t GSP_FALCON_DMACTL = GSP_FALCON_BASE + 0x10C;

// The DMA engine's copy registers. A copy takes the system memory address in 256-byte units, split over two
// registers, then for each block the offset in IMEM or DMEM, the offset added to the address, and the command.
constexpr uint32_t GSP_FALCON_DMATRFBASE   = GSP_FALCON_BASE + 0x110;
constexpr uint32_t GSP_FALCON_DMATRFMOFFS  = GSP_FALCON_BASE + 0x114;
constexpr uint32_t GSP_FALCON_DMATRFCMD    = GSP_FALCON_BASE + 0x118;
constexpr uint32_t GSP_FALCON_DMATRFFBOFFS = GSP_FALCON_BASE + 0x11C;
constexpr uint32_t GSP_FALCON_DMATRFBASE1  = GSP_FALCON_BASE + 0x128;
constexpr uint32_t DMA_BLOCK_BYTES         = 256;
constexpr uint32_t DMA_ADDRESS_SHIFT       = 8;
constexpr uint32_t DMA_ADDRESS_HIGH_MASK   = 0x1FF;
constexpr uint32_t DMA_MEMORY_OFFSET_MASK  = 0xFFFFFF;

// The falcon's memory interface: bit 7 of its control lets the DMA engine use physical addresses with no address space
// bound. Its first transfer register is transfer setup 0: the target in bits 1:0, and bit 2 for physical addresses.
constexpr uint32_t GSP_FBIF_BASE                    = 0x110600;
constexpr uint32_t GSP_FBIF_TRANSCFG0               = GSP_FBIF_BASE + 0x000;
constexpr uint32_t GSP_FBIF_CTL                     = GSP_FBIF_BASE + 0x024;
constexpr uint32_t ALLOW_PHYSICAL_WITHOUT_CONTEXT   = 0x80;
constexpr uint32_t TRANSFER_TARGET_MASK             = 0x3;
constexpr uint32_t TARGET_COHERENT_SYSTEM_MEMORY    = 0x1;
constexpr uint32_t TARGET_NONCOHERENT_SYSTEM_MEMORY = 0x2;
constexpr uint32_t PHYSICAL_ADDRESSES               = 0x4;

// The command register reads back with bit 1 set once the DMA engine is idle
constexpr uint32_t DMA_IDLE = 0x2;

// The command's fields for a block of 256 bytes, a block into IMEM rather than DMEM, and code marked secure, as NVIDIA
// loads the code of a program its boot ROM checks. Every command uses transfer setup 0, so its context field is 0.
constexpr uint32_t DMA_256_BYTES            = 0x600;
constexpr uint32_t DMA_TO_IMEM              = 0x10;
constexpr uint32_t DMA_SECURE               = 0x4;
constexpr uint32_t DMA_SECURE_BLOCK_TO_IMEM = DMA_256_BYTES | DMA_TO_IMEM | DMA_SECURE;
constexpr uint32_t DMA_BLOCK_TO_DMEM        = DMA_256_BYTES;

// The boot ROM's registers in the second register block: the signature's offset in the data, the engine mask, the
// ucode id, and the signature's algorithm, RSA-3072
constexpr uint32_t GSP_FALCON2_BROM_PARAADDR0     = GSP_FALCON2_BASE + 0x210;
constexpr uint32_t GSP_FALCON2_BROM_ENGIDMASK     = GSP_FALCON2_BASE + 0x19C;
constexpr uint32_t GSP_FALCON2_BROM_CURR_UCODE_ID = GSP_FALCON2_BASE + 0x198;
constexpr uint32_t GSP_FALCON2_MOD_SEL            = GSP_FALCON2_BASE + 0x180;
constexpr uint32_t RSA_3072                       = 0x1;

// The first fuse register recording a version of a program each falcon runs, the one for ucode id 1
constexpr uint32_t GSP_UCODE_VERSION_FUSES   = 0x8241C0;
constexpr uint32_t SEC2_UCODE_VERSION_FUSES  = 0x824140;
constexpr uint32_t UCODE_VERSION_FUSE_STRIDE = 4; // bytes from one fuse register to the next
constexpr uint32_t UCODE_VERSION_FUSE_COUNT  = 16;

constexpr uint64_t NS_PER_MS = 1000000;

// Spins until the `bits` of the register at `offset` read as `value` or `timeout_ns` passes, reading it at least
// once, and returns whether they did
static bool wait_for_bits(const registers& regs, uint32_t offset, uint32_t bits, uint32_t value, uint64_t timeout_ns) {
    uint64_t deadline = clock::now_ns() + timeout_ns;

    while (true) {
        if ((regs.read(offset) & bits) == value) {
            return true;
        }

        if (clock::now_ns() > deadline) {
            return false;
        }

        cpu::relax();
    }
}

static void let_reset_propagate(const registers& regs) {
    for (uint32_t i = 0; i < RESET_PROPAGATION_READS; i++) {
        (void)regs.read(GSP_FALCON_ENGINE);
    }
}

static void pulse_engine_reset(registers& regs) {
    uint32_t engine = regs.read(GSP_FALCON_ENGINE);
    regs.write(GSP_FALCON_ENGINE, engine | ENGINE_RESET);
    let_reset_propagate(regs);

    engine = regs.read(GSP_FALCON_ENGINE);
    regs.write(GSP_FALCON_ENGINE, engine & ~ENGINE_RESET);
    let_reset_propagate(regs);
}

static int32_t wait_for_cleared_memories(const registers& regs, uint64_t timeout_ns) {
    if (!wait_for_bits(regs, GSP_FALCON_HWCFG2, CLEARING_MEMORIES, 0, timeout_ns)) {
        log::error(
            "nvidia: the GSP's falcon did not finish clearing its memories within %lu ms",
            timeout_ns / NS_PER_MS
        );
        return ERR_TIMEOUT;
    }

    return OK;
}

static int32_t select_falcon_core(registers& regs, uint64_t timeout_ns) {
    bool has_riscv_core = (regs.read(GSP_FALCON_HWCFG2) & HAS_RISCV_CORE) != 0;
    bool is_riscv_selected = has_riscv_core && (regs.read(GSP_RISCV_BCR_CTRL) & RISCV_CORE_SELECTED) != 0;
    if (!is_riscv_selected) {
        return OK;
    }

    regs.write(GSP_RISCV_BCR_CTRL, SELECT_FALCON_CORE);
    if (!wait_for_bits(regs, GSP_RISCV_BCR_CTRL, CORE_SELECT_VALID, CORE_SELECT_VALID, timeout_ns)) {
        log::error(
            "nvidia: the GSP did not switch from its RISC-V core to its falcon core within %lu ms",
            timeout_ns / NS_PER_MS
        );
        return ERR_TIMEOUT;
    }

    return OK;
}

static int32_t reset_in_full(registers& regs, uint64_t ready_timeout_ns, uint64_t timeout_ns) {
    // The ready bit may never be set, so the reset goes on without it, as in NVIDIA's driver
    (void)wait_for_bits(regs, GSP_FALCON_HWCFG2, READY_FOR_RESET, READY_FOR_RESET, ready_timeout_ns);
    pulse_engine_reset(regs);

    int32_t rc = wait_for_cleared_memories(regs, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    return select_falcon_core(regs, timeout_ns);
}

int32_t reset_gsp_falcon(registers& regs, uint32_t boot0, uint64_t ready_timeout_ns, uint64_t timeout_ns) {
    int32_t rc = reset_in_full(regs, ready_timeout_ns, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    rc = reset_in_full(regs, ready_timeout_ns, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    rc = select_falcon_core(regs, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    rc = wait_for_cleared_memories(regs, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    regs.write(GSP_FALCON_RM, boot0);

    return OK;
}

bool is_gsp_falcon_halted(const registers& regs) {
    return (regs.read(GSP_FALCON_CPUCTL) & FALCON_HALTED) != 0;
}

// Lets the DMA engine read system memory by physical address through transfer setup 0, looking in the CPU's caches
// only for a cached program
static void configure_dma(registers& regs, bool is_cached) {
    uint32_t control = regs.read(GSP_FBIF_CTL) | ALLOW_PHYSICAL_WITHOUT_CONTEXT;
    regs.write(GSP_FBIF_CTL, control);
    regs.write(GSP_FALCON_DMACTL, 0);

    uint32_t target = is_cached ? TARGET_COHERENT_SYSTEM_MEMORY : TARGET_NONCOHERENT_SYSTEM_MEMORY;
    uint32_t transfer = (regs.read(GSP_FBIF_TRANSCFG0) & ~TRANSFER_TARGET_MASK) | target | PHYSICAL_ADDRESSES;
    regs.write(GSP_FBIF_TRANSCFG0, transfer);
}

// Copies the `bytes` of the program's `part` into IMEM or DMEM from `memory_offset`. Each block is read at `base`
// plus its offset, which counts from `first_offset`.
static int32_t copy_blocks(
    registers& regs,
    uint64_t base,
    uint32_t first_offset,
    uint32_t memory_offset,
    uint32_t bytes,
    uint32_t command,
    const char* part,
    uint64_t timeout_ns
) {
    uint64_t base_blocks = base >> DMA_ADDRESS_SHIFT;
    regs.write(GSP_FALCON_DMATRFBASE, static_cast<uint32_t>(base_blocks));
    regs.write(GSP_FALCON_DMATRFBASE1, static_cast<uint32_t>(base_blocks >> 32) & DMA_ADDRESS_HIGH_MASK);

    for (uint64_t copied = 0; copied < bytes; copied += DMA_BLOCK_BYTES) {
        uint32_t block = static_cast<uint32_t>(copied);
        regs.write(GSP_FALCON_DMATRFMOFFS, (memory_offset + block) & DMA_MEMORY_OFFSET_MASK);
        regs.write(GSP_FALCON_DMATRFFBOFFS, first_offset + block);
        regs.write(GSP_FALCON_DMATRFCMD, command);

        if (!wait_for_bits(regs, GSP_FALCON_DMATRFCMD, DMA_IDLE, DMA_IDLE, timeout_ns)) {
            log::error(
                "nvidia: the GSP's falcon did not finish copying the block at byte 0x%X of its program's %s within "
                "%lu ms",
                block,
                part,
                timeout_ns / NS_PER_MS
            );
            return ERR_TIMEOUT;
        }
    }

    return OK;
}

int32_t run_gsp_falcon_program(registers& regs, const falcon_program& program, uint64_t timeout_ns) {
    configure_dma(regs, program.is_cached);

    // As in NVIDIA's driver, the code's base sits below the program by its virtual base, and its blocks' offsets
    // count from the virtual base, so they are read from the program's start and tagged with their virtual addresses
    uint64_t code_base = program.dma_address - program.imem_virtual_base;
    uint64_t data_base = program.dma_address + program.code_size;

    int32_t rc = copy_blocks(
        regs,
        code_base,
        program.imem_virtual_base,
        program.imem_physical_base,
        program.code_size,
        DMA_SECURE_BLOCK_TO_IMEM,
        "code",
        timeout_ns
    );
    if (rc != OK) {
        return rc;
    }

    rc = copy_blocks(
        regs,
        data_base,
        0,
        program.dmem_physical_base,
        program.data_size,
        DMA_BLOCK_TO_DMEM,
        "data",
        timeout_ns
    );
    if (rc != OK) {
        return rc;
    }

    regs.write(GSP_FALCON2_BROM_PARAADDR0, program.signature_offset);
    regs.write(GSP_FALCON2_BROM_ENGIDMASK, program.engine_mask);
    regs.write(GSP_FALCON2_BROM_CURR_UCODE_ID, program.ucode_id);
    regs.write(GSP_FALCON2_MOD_SEL, RSA_3072);
    regs.write(GSP_FALCON_BOOTVEC, program.imem_virtual_base);

    bool starts_through_alias = (regs.read(GSP_FALCON_CPUCTL) & STARTS_THROUGH_ALIAS) != 0;
    regs.write(starts_through_alias ? GSP_FALCON_CPUCTL_ALIAS : GSP_FALCON_CPUCTL, START_FALCON);
    if (!wait_for_bits(regs, GSP_FALCON_CPUCTL, FALCON_HALTED, FALCON_HALTED, timeout_ns)) {
        log::error(
            "nvidia: the GSP's falcon did not halt within %lu ms of starting its program",
            timeout_ns / NS_PER_MS
        );
        return ERR_TIMEOUT;
    }

    return OK;
}

// Without a default, the compiler flags a falcon added to `falcon_id` but missing here
static uint32_t first_ucode_version_fuse(falcon_id falcon) {
    switch (falcon) {
        case falcon_id::gsp:
            return GSP_UCODE_VERSION_FUSES;
        case falcon_id::sec2:
            return SEC2_UCODE_VERSION_FUSES;
    }
}

uint32_t read_ucode_fuse_version(const registers& regs, falcon_id falcon, uint32_t ucode_id) {
    uint32_t fuse_index = ucode_id - 1;
    if (fuse_index >= UCODE_VERSION_FUSE_COUNT) {
        return 0;
    }

    uint32_t fuses = regs.read(first_ucode_version_fuse(falcon) + fuse_index * UCODE_VERSION_FUSE_STRIDE);
    uint32_t version = 0;
    while (fuses != 0) {
        version++;
        fuses >>= 1;
    }

    return version;
}

} // namespace drivers::nvidia
