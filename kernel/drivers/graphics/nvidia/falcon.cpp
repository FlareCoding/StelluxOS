// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under the notice in LICENSE-NVIDIA

#include "drivers/graphics/nvidia/falcon.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "hw/cpu.h"

namespace drivers::nvidia {

namespace {

// Where a falcon's registers lie in BAR0, and its engine's name for log lines
struct falcon_layout {
    uint32_t    base;                // the falcon's own block
    uint32_t    second_base;         // the block holding its RISC-V core's and boot ROM's registers
    uint32_t    fbif_base;           // its memory interface's block
    uint32_t    engine;              // its engine register
    uint32_t    ucode_version_fuses; // the first fuse register recording a version of its programs, for ucode id 1
    const char* name;                // its engine's name, for log lines
};

} // namespace

constexpr falcon_layout GSP_FALCON = {
    .base = 0x110000,
    .second_base = 0x111000,
    .fbif_base = 0x110600,
    .engine = 0x1103C0,
    .ucode_version_fuses = 0x8241C0,
    .name = "the GSP",
};

constexpr falcon_layout SEC2_FALCON = {
    .base = 0x840000,
    .second_base = 0x841000,
    .fbif_base = 0x840600,
    .engine = 0x8403C0,
    .ucode_version_fuses = 0x824140,
    .name = "SEC2",
};

// Bit 0 of a falcon's engine register holds the falcon in reset while set, and NVIDIA's driver reads the register
// after each write so the reset signal propagates
constexpr uint32_t ENGINE_RESET            = 0x1;
constexpr uint32_t RESET_PROPAGATION_READS = 10;

// The falcon's hardware configuration register: bit 10 set when the falcon has a RISC-V core, bit 12 set while the
// falcon clears its memories after a reset, bit 31 set once the falcon is ready for a reset
constexpr uint32_t FALCON_HWCFG2     = 0x0F4;
constexpr uint32_t HAS_RISCV_CORE    = 0x400;
constexpr uint32_t CLEARING_MEMORIES = 0x1000;
constexpr uint32_t READY_FOR_RESET   = 0x80000000;

// The falcon register NVIDIA's driver writes the chip's `PMC_BOOT_0` value to as a reset ends
constexpr uint32_t FALCON_RM = 0x084;

// The falcon's two mailbox registers. NVIDIA's driver writes the address of LibOS's arguments into the GSP's, and a
// program's arguments into those of the falcon running it.
constexpr uint32_t FALCON_MAILBOX0 = 0x040;
constexpr uint32_t FALCON_MAILBOX1 = 0x044;

// The register NVIDIA calls the falcon's OS register, which its driver sets to the bootloader's app version
constexpr uint32_t FALCON_OS = 0x080;

// The first of the GSP's eight queue head registers, which NVIDIA's driver writes 0 to after each message it queues
// for the GSP's resource manager
constexpr uint32_t GSP_QUEUE_HEAD0 = 0x110C00;

// The RISC-V core's boot control register: bit 4 set while the RISC-V core is selected over the falcon core, bit 0
// set once a selection has taken effect, and the value NVIDIA's driver writes to select the falcon core
constexpr uint32_t RISCV_BCR_CTRL      = 0x668;
constexpr uint32_t RISCV_CORE_SELECTED = 0x10;
constexpr uint32_t CORE_SELECT_VALID   = 0x1;
constexpr uint32_t SELECT_FALCON_CORE  = 0x0;

// The value NVIDIA's driver writes to select the RISC-V core, setting bits 0 and 4 and also bit 8, the field NVIDIA
// calls BRFETCH
constexpr uint32_t SELECT_RISCV_CORE = 0x111;

// The RISC-V core's control register, whose bit 7 is set while the core is running
constexpr uint32_t RISCV_CPUCTL = 0x388;
constexpr uint32_t RISCV_ACTIVE = 0x80;

// The falcon's control register: writing bit 1 starts the falcon, bit 4 is set while it is halted, and while bit 6
// is set the falcon is started through the register's alias instead
constexpr uint32_t FALCON_CPUCTL        = 0x100;
constexpr uint32_t FALCON_CPUCTL_ALIAS  = 0x130;
constexpr uint32_t START_FALCON         = 0x2;
constexpr uint32_t FALCON_HALTED        = 0x10;
constexpr uint32_t STARTS_THROUGH_ALIAS = 0x40;

// Where the falcon starts running, as a virtual address of its code
constexpr uint32_t FALCON_BOOTVEC = 0x104;

// The DMA engine's control register, cleared so a copy needs no address space bound to the falcon
constexpr uint32_t FALCON_DMACTL = 0x10C;

// The DMA engine's copy registers. A copy takes the system memory address in 256-byte units, split over two
// registers, then for each block the offset in IMEM or DMEM, the offset added to the address, and the command.
constexpr uint32_t FALCON_DMATRFBASE      = 0x110;
constexpr uint32_t FALCON_DMATRFMOFFS     = 0x114;
constexpr uint32_t FALCON_DMATRFCMD       = 0x118;
constexpr uint32_t FALCON_DMATRFFBOFFS    = 0x11C;
constexpr uint32_t FALCON_DMATRFBASE1     = 0x128;
constexpr uint32_t DMA_BLOCK_BYTES        = 256;
constexpr uint32_t DMA_ADDRESS_SHIFT      = 8;
constexpr uint32_t DMA_ADDRESS_HIGH_MASK  = 0x1FF;
constexpr uint32_t DMA_MEMORY_OFFSET_MASK = 0xFFFFFF;

// The falcon's memory interface: bit 7 of its control lets the DMA engine use physical addresses with no address space
// bound. Its first transfer register is transfer setup 0: the target in bits 1:0, and bit 2 for physical addresses.
constexpr uint32_t FBIF_TRANSCFG0                   = 0x000;
constexpr uint32_t FBIF_CTL                         = 0x024;
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
constexpr uint32_t FALCON2_BROM_PARAADDR0     = 0x210;
constexpr uint32_t FALCON2_BROM_ENGIDMASK     = 0x19C;
constexpr uint32_t FALCON2_BROM_CURR_UCODE_ID = 0x198;
constexpr uint32_t FALCON2_MOD_SEL            = 0x180;
constexpr uint32_t RSA_3072                   = 0x1;

// The fuse registers recording the versions of a falcon's programs, one for each ucode id from 1
constexpr uint32_t UCODE_VERSION_FUSE_STRIDE = 4; // bytes from one fuse register to the next
constexpr uint32_t UCODE_VERSION_FUSE_COUNT  = 16;

constexpr uint64_t NS_PER_MS = 1000000;

// Without a default, the compiler flags a falcon added to `falcon_id` but missing here
static const falcon_layout& layout_of(falcon_id falcon) {
    switch (falcon) {
        case falcon_id::gsp:
            return GSP_FALCON;
        case falcon_id::sec2:
            return SEC2_FALCON;
    }
}

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

static void let_reset_propagate(const registers& regs, const falcon_layout& layout) {
    for (uint32_t i = 0; i < RESET_PROPAGATION_READS; i++) {
        (void)regs.read(layout.engine);
    }
}

static void pulse_engine_reset(registers& regs, const falcon_layout& layout) {
    uint32_t engine = regs.read(layout.engine);
    regs.write(layout.engine, engine | ENGINE_RESET);
    let_reset_propagate(regs, layout);

    engine = regs.read(layout.engine);
    regs.write(layout.engine, engine & ~ENGINE_RESET);
    let_reset_propagate(regs, layout);
}

static int32_t wait_for_cleared_memories(const registers& regs, const falcon_layout& layout, uint64_t timeout_ns) {
    if (!wait_for_bits(regs, layout.base + FALCON_HWCFG2, CLEARING_MEMORIES, 0, timeout_ns)) {
        log::error(
            "nvidia: %s's falcon did not finish clearing its memories within %lu ms",
            layout.name,
            timeout_ns / NS_PER_MS
        );
        return ERR_TIMEOUT;
    }

    return OK;
}

static bool has_riscv_core(const registers& regs, const falcon_layout& layout) {
    return (regs.read(layout.base + FALCON_HWCFG2) & HAS_RISCV_CORE) != 0;
}

static int32_t select_falcon_core(registers& regs, const falcon_layout& layout, uint64_t timeout_ns) {
    uint32_t boot_control = layout.second_base + RISCV_BCR_CTRL;
    bool is_riscv_selected = has_riscv_core(regs, layout) && (regs.read(boot_control) & RISCV_CORE_SELECTED) != 0;
    if (!is_riscv_selected) {
        return OK;
    }

    regs.write(boot_control, SELECT_FALCON_CORE);
    if (!wait_for_bits(regs, boot_control, CORE_SELECT_VALID, CORE_SELECT_VALID, timeout_ns)) {
        log::error(
            "nvidia: %s did not switch from its RISC-V core to its falcon core within %lu ms",
            layout.name,
            timeout_ns / NS_PER_MS
        );
        return ERR_TIMEOUT;
    }

    return OK;
}

static int32_t reset_engine(
    registers& regs,
    const falcon_layout& layout,
    uint64_t ready_timeout_ns,
    uint64_t timeout_ns
) {
    // The ready bit may never be set, so the reset goes on without it, as in NVIDIA's driver
    (void)wait_for_bits(regs, layout.base + FALCON_HWCFG2, READY_FOR_RESET, READY_FOR_RESET, ready_timeout_ns);
    pulse_engine_reset(regs, layout);

    return wait_for_cleared_memories(regs, layout, timeout_ns);
}

static int32_t reset_in_full(
    registers& regs,
    const falcon_layout& layout,
    uint64_t ready_timeout_ns,
    uint64_t timeout_ns
) {
    int32_t rc = reset_engine(regs, layout, ready_timeout_ns, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    return select_falcon_core(regs, layout, timeout_ns);
}

int32_t reset_falcon(
    registers& regs,
    falcon_id falcon,
    uint32_t boot0,
    uint64_t ready_timeout_ns,
    uint64_t timeout_ns
) {
    const falcon_layout& layout = layout_of(falcon);

    int32_t rc = reset_in_full(regs, layout, ready_timeout_ns, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    rc = reset_in_full(regs, layout, ready_timeout_ns, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    rc = select_falcon_core(regs, layout, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    rc = wait_for_cleared_memories(regs, layout, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    regs.write(layout.base + FALCON_RM, boot0);

    return OK;
}

bool has_gsp_riscv_core(const registers& regs) {
    return has_riscv_core(regs, GSP_FALCON);
}

int32_t reset_gsp_into_riscv(registers& regs, uint64_t ready_timeout_ns, uint64_t timeout_ns) {
    int32_t rc = reset_engine(regs, GSP_FALCON, ready_timeout_ns, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    regs.write(GSP_FALCON.second_base + RISCV_BCR_CTRL, SELECT_RISCV_CORE);

    return OK;
}

void notify_gsp_of_queued_messages(registers& regs) {
    regs.write(GSP_QUEUE_HEAD0, 0);
}

void write_libos_arguments_address(registers& regs, uint64_t address) {
    regs.write(GSP_FALCON.base + FALCON_MAILBOX0, static_cast<uint32_t>(address));
    regs.write(GSP_FALCON.base + FALCON_MAILBOX1, static_cast<uint32_t>(address >> 32));
}

bool is_falcon_halted(const registers& regs, falcon_id falcon) {
    return (regs.read(layout_of(falcon).base + FALCON_CPUCTL) & FALCON_HALTED) != 0;
}

static bool is_riscv_active(const registers& regs, const falcon_layout& layout) {
    return (regs.read(layout.second_base + RISCV_CPUCTL) & RISCV_ACTIVE) != 0;
}

int32_t stop_falcon(registers& regs, falcon_id falcon, uint32_t boot0, uint64_t ready_timeout_ns, uint64_t timeout_ns) {
    const falcon_layout& layout = layout_of(falcon);

    int32_t rc = reset_falcon(regs, falcon, boot0, ready_timeout_ns, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    if (!is_falcon_halted(regs, falcon)) {
        log::error("nvidia: %s's falcon does not read as halted after its reset", layout.name);
        return ERR_BAD_STATE;
    }

    bool is_riscv_running = has_riscv_core(regs, layout) && is_riscv_active(regs, layout);
    if (is_riscv_running) {
        log::error("nvidia: %s's RISC-V core reads as running after its reset", layout.name);
        return ERR_BAD_STATE;
    }

    return OK;
}

// Lets the DMA engine read system memory by physical address through transfer setup 0, looking in the CPU's caches
// only when memory read through it may be cached
static void configure_dma(registers& regs, const falcon_layout& layout, bool reaches_cached_memory) {
    uint32_t control = regs.read(layout.fbif_base + FBIF_CTL) | ALLOW_PHYSICAL_WITHOUT_CONTEXT;
    regs.write(layout.fbif_base + FBIF_CTL, control);
    regs.write(layout.base + FALCON_DMACTL, 0);

    uint32_t target = reaches_cached_memory ? TARGET_COHERENT_SYSTEM_MEMORY : TARGET_NONCOHERENT_SYSTEM_MEMORY;
    uint32_t transfer = regs.read(layout.fbif_base + FBIF_TRANSCFG0) & ~TRANSFER_TARGET_MASK;
    regs.write(layout.fbif_base + FBIF_TRANSCFG0, transfer | target | PHYSICAL_ADDRESSES);
}

// Copies the `bytes` of the program's `part` into IMEM or DMEM from `memory_offset`. Each block is read at `base`
// plus its offset, which counts from `first_offset`.
static int32_t copy_blocks(
    registers& regs,
    const falcon_layout& layout,
    uint64_t base,
    uint32_t first_offset,
    uint32_t memory_offset,
    uint32_t bytes,
    uint32_t command,
    const char* part,
    uint64_t timeout_ns
) {
    uint64_t base_blocks = base >> DMA_ADDRESS_SHIFT;
    regs.write(layout.base + FALCON_DMATRFBASE, static_cast<uint32_t>(base_blocks));
    regs.write(layout.base + FALCON_DMATRFBASE1, static_cast<uint32_t>(base_blocks >> 32) & DMA_ADDRESS_HIGH_MASK);

    for (uint64_t copied = 0; copied < bytes; copied += DMA_BLOCK_BYTES) {
        uint32_t block = static_cast<uint32_t>(copied);
        regs.write(layout.base + FALCON_DMATRFMOFFS, (memory_offset + block) & DMA_MEMORY_OFFSET_MASK);
        regs.write(layout.base + FALCON_DMATRFFBOFFS, first_offset + block);
        regs.write(layout.base + FALCON_DMATRFCMD, command);

        if (!wait_for_bits(regs, layout.base + FALCON_DMATRFCMD, DMA_IDLE, DMA_IDLE, timeout_ns)) {
            log::error(
                "nvidia: %s's falcon did not finish copying the block at byte 0x%X of its program's %s within %lu ms",
                layout.name,
                block,
                part,
                timeout_ns / NS_PER_MS
            );
            return ERR_TIMEOUT;
        }
    }

    return OK;
}

// Runs `program` on `falcon`, handing it `mailboxes` and reading them back when `mailboxes` is not null
static int32_t run_program(
    registers& regs,
    falcon_id falcon,
    const falcon_program& program,
    falcon_mailboxes* mailboxes,
    uint64_t timeout_ns
) {
    const falcon_layout& layout = layout_of(falcon);
    bool reaches_cached_memory = program.is_cached || program.reads_cached_memory;

    configure_dma(regs, layout, reaches_cached_memory);

    // As in NVIDIA's driver, the code's base sits below the code by its virtual base, and its blocks' offsets count
    // from the virtual base, so they are read from the code's start and tagged with their virtual addresses
    uint64_t code_base = program.dma_address + program.code_offset - program.imem_virtual_base;
    uint64_t data_base = program.dma_address + program.data_offset;

    int32_t rc = copy_blocks(
        regs,
        layout,
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
        layout,
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

    regs.write(layout.second_base + FALCON2_BROM_PARAADDR0, program.signature_offset);
    regs.write(layout.second_base + FALCON2_BROM_ENGIDMASK, program.engine_mask);
    regs.write(layout.second_base + FALCON2_BROM_CURR_UCODE_ID, program.ucode_id);
    regs.write(layout.second_base + FALCON2_MOD_SEL, RSA_3072);
    regs.write(layout.base + FALCON_BOOTVEC, program.imem_virtual_base);

    if (mailboxes) {
        regs.write(layout.base + FALCON_MAILBOX0, mailboxes->mailbox0);
        regs.write(layout.base + FALCON_MAILBOX1, mailboxes->mailbox1);
    }

    bool starts_through_alias = (regs.read(layout.base + FALCON_CPUCTL) & STARTS_THROUGH_ALIAS) != 0;
    regs.write(layout.base + (starts_through_alias ? FALCON_CPUCTL_ALIAS : FALCON_CPUCTL), START_FALCON);
    bool has_halted = wait_for_bits(regs, layout.base + FALCON_CPUCTL, FALCON_HALTED, FALCON_HALTED, timeout_ns);

    if (mailboxes) {
        mailboxes->mailbox0 = regs.read(layout.base + FALCON_MAILBOX0);
        mailboxes->mailbox1 = regs.read(layout.base + FALCON_MAILBOX1);
    }

    if (!has_halted) {
        log::error(
            "nvidia: %s's falcon did not halt within %lu ms of starting its program",
            layout.name,
            timeout_ns / NS_PER_MS
        );
        if (mailboxes) {
            log::error(
                "nvidia: the mailboxes of %s's falcon read 0x%X and 0x%X after the wait for its halt",
                layout.name,
                mailboxes->mailbox0,
                mailboxes->mailbox1
            );
        }

        return ERR_TIMEOUT;
    }

    return OK;
}

int32_t run_falcon_program(registers& regs, falcon_id falcon, const falcon_program& program, uint64_t timeout_ns) {
    return run_program(regs, falcon, program, nullptr, timeout_ns);
}

int32_t run_falcon_program(
    registers& regs,
    falcon_id falcon,
    const falcon_program& program,
    falcon_mailboxes& mailboxes,
    uint64_t timeout_ns
) {
    return run_program(regs, falcon, program, &mailboxes, timeout_ns);
}

int32_t run_booter_load(
    registers& regs,
    const falcon_program& booter_load,
    uint64_t metadata_address,
    uint64_t timeout_ns
) {
    falcon_mailboxes mailboxes = {
        .mailbox0 = static_cast<uint32_t>(metadata_address),
        .mailbox1 = static_cast<uint32_t>(metadata_address >> 32),
    };

    int32_t rc = run_falcon_program(regs, falcon_id::sec2, booter_load, mailboxes, timeout_ns);
    if (rc != OK) {
        return rc;
    }

    if (mailboxes.mailbox0 != 0) {
        log::error(
            "nvidia: the SEC2 falcon's first mailbox reads 0x%X after booter load, which leaves 0 when it succeeds",
            mailboxes.mailbox0
        );
        return ERR_BAD_STATE;
    }

    return OK;
}

void write_gsp_app_version(registers& regs, uint32_t app_version) {
    regs.write(GSP_FALCON.base + FALCON_OS, app_version);
}

bool is_gsp_riscv_running(const registers& regs) {
    return is_riscv_active(regs, GSP_FALCON);
}

uint32_t read_ucode_fuse_version(const registers& regs, falcon_id falcon, uint32_t ucode_id) {
    uint32_t fuse_index = ucode_id - 1;
    if (fuse_index >= UCODE_VERSION_FUSE_COUNT) {
        return 0;
    }

    uint32_t fuses = regs.read(layout_of(falcon).ucode_version_fuses + fuse_index * UCODE_VERSION_FUSE_STRIDE);
    uint32_t version = 0;
    while (fuses != 0) {
        version++;
        fuses >>= 1;
    }

    return version;
}

} // namespace drivers::nvidia
