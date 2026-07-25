#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_REGS_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_REGS_H

#include "common/types.h"

// Register/identity constants for the NVIDIA RTX 3080 (GA102, Ampere) bring-up.
// This is an intentionally x86-only, single-card vertical driver; values are
// specific to GA102 (10de:2216). See gpu-driver-reverse-engineering-spec.md.
namespace nvidia {

// ---- PCI identity ---------------------------------------------------------
constexpr uint16_t PCI_VENDOR_NVIDIA        = 0x10DE;
constexpr uint16_t PCI_DEVICE_GA102_RTX3080 = 0x2216; // GeForce RTX 3080 Lite Hash Rate

// NVIDIA BAR layout, by PCI BAR slot index (confirmed on hardware):
//   BAR0 = 16 MB MMIO register aperture, 32-bit, non-prefetch -> slot 0
//   BAR1 = 256 MB VRAM aperture, 64-bit prefetchable          -> slots 1+2
//   BAR3 = 32 MB instance-memory aperture, 64-bit prefetchable -> slots 3+4
// NOTE: BAR0 is a *32-bit* BAR (reads back type=MMIO32), so it consumes only
// slot 0; the 64-bit BAR1/BAR3 therefore begin at slots 1 and 3 (not 2/4).
constexpr uint8_t BAR_REGS = 0; // BAR0 (regs)
constexpr uint8_t BAR_FB   = 1; // BAR1 (VRAM aperture)
constexpr uint8_t BAR_INST = 3; // BAR3 (instance memory)

// ---- BAR0 register offsets (GA102 / Ampere) -------------------------------
// NV_PMC_BOOT_0: master chip-identity register. Reading != 0xFFFFFFFF proves
// BAR0 decodes and the GPU is alive (the canonical "chip present" probe).
constexpr uint32_t NV_PMC_BOOT_0 = 0x00000000;
// NV_PMC_BOOT_1: endianness/control (little-endian reads 0 on the low bits).
constexpr uint32_t NV_PMC_BOOT_1 = 0x00000004;

// NV_PMC_BOOT_0 field decode (matches the open driver / nouveau):
//   [28:24] architecture, [23:20] implementation, [7:4] major rev, [3:0] minor
// "chipset" = bits [28:20] = (architecture << 4 | implementation).
constexpr uint32_t PMC_BOOT_0_CHIPSET_MASK  = 0x1FF00000;
constexpr uint32_t PMC_BOOT_0_CHIPSET_SHIFT = 20;
constexpr uint32_t PMC_BOOT_0_ARCH_MASK     = 0x1F000000;
constexpr uint32_t PMC_BOOT_0_ARCH_SHIFT    = 24;
constexpr uint32_t PMC_BOOT_0_IMPL_MASK     = 0x00F00000;
constexpr uint32_t PMC_BOOT_0_IMPL_SHIFT    = 20;
constexpr uint32_t PMC_BOOT_0_MAJOR_MASK    = 0x000000F0;
constexpr uint32_t PMC_BOOT_0_MAJOR_SHIFT   = 4;
constexpr uint32_t PMC_BOOT_0_MINOR_MASK    = 0x0000000F;

// Expected chipset value for GA102 (architecture 0x17 = Ampere GA10x, impl 0x2).
constexpr uint32_t CHIPSET_GA102 = 0x172;

// Sentinel returned by MMIO reads when the GPU is off the bus / BAR0 dead.
constexpr uint32_t GPU_LOST = 0xFFFFFFFF;

// ---- GFW (GPU firmware / VBIOS devinit) boot completion --------------------
// Authoritative source: open-gpu-kernel-modules @535.183.01
//   swref/.../ampere/ga102/dev_gc6_island.h:
//     NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK = 0x00118128
//     NV_PGC6_AON_SECURE_SCRATCH_GROUP_05(i)              = 0x00118234 + i*4
//   dev_gc6_island_addendum.h:
//     GROUP_05_0_GFW_BOOT = GROUP_05(0); _PROGRESS = bits 7:0; _COMPLETED = 0xFF
//   logic: _gpuIsGfwBootCompleted_TU102 (kern_gpu_tu102.c:321-370):
//     (1) the secure-scratch PRIV_LEVEL_MASK must show READ_PROTECTION_LEVEL0
//         ENABLE (FWSEC has lowered the PLM), THEN
//     (2) GFW_BOOT PROGRESS must read COMPLETED (0xFF).
constexpr uint32_t NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK = 0x00118128;
constexpr uint32_t SCRATCH_05_PLM_READ_PROTECTION_LEVEL0_ENABLE        = 0x00000001; // bit 0
constexpr uint32_t NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT      = 0x00118234; // GROUP_05(0)
constexpr uint32_t GFW_BOOT_PROGRESS_MASK                              = 0x000000FF; // [7:0]
constexpr uint32_t GFW_BOOT_PROGRESS_COMPLETED                         = 0x000000FF;

// ===========================================================================
// GSP Falcon engine + FWSEC-FRTS registers (open-gpu-kernel-modules @535.183.01;
// offsets cross-checked against the golden boot-trace for this GA102).
//   dev_gsp.h:               NV_PGSP base 0x110000
//   dev_riscv_pri.h:         NV_FALCON2_GSP_BASE 0x111000
//   dev_gsp_addendum.h:      NV_PGSP_FBIF_BASE 0x110600
//   dev_falcon_v4.h / dev_falcon_second_pri.h / dev_fbif_v4.h: offsets below
// ===========================================================================
constexpr uint32_t NV_PGSP_FALCON_BASE = 0x00110000; // Falcon register base
constexpr uint32_t NV_FALCON2_GSP_BASE = 0x00111000; // RISC-V/BROM register base
constexpr uint32_t NV_PGSP_FBIF_BASE   = 0x00110600; // FBIF register base

// GSP command-queue doorbell: NV_PGSP_QUEUE_HEAD(i) = 0x110c00 + i*8 (dev_gsp.h).
// _kgspRpcSendMessage rings queue 0 by writing 0 after submitting buffers.
constexpr uint32_t NV_PGSP_QUEUE_HEAD  = 0x00110c00; // command-queue 0 doorbell

// SEC2 Falcon apertures (the Booter runs here). dev_sec_pri.h NV_PSEC=0x840000,
// dev_sec_addendum.h NV_PSEC_FBIF_BASE=0x840600, dev_falcon_second_pri.h
// NV_FALCON2_SEC_BASE=0x841000.
constexpr uint32_t NV_PSEC_FALCON_BASE = 0x00840000; // SEC2 Falcon register base
constexpr uint32_t NV_FALCON2_SEC_BASE = 0x00841000; // SEC2 RISC-V/BROM register base
constexpr uint32_t NV_PSEC_FBIF_BASE   = 0x00840600; // SEC2 FBIF register base

// Falcon registers (offset from the Falcon register base)
constexpr uint32_t NV_PFALCON_FALCON_MAILBOX0    = 0x040;
constexpr uint32_t NV_PFALCON_FALCON_MAILBOX1    = 0x044;
constexpr uint32_t NV_PFALCON_FALCON_RM          = 0x084; // <- chipId0 written here
constexpr uint32_t NV_PFALCON_FALCON_OS          = 0x080;
constexpr uint32_t NV_PFALCON_FALCON_HWCFG2      = 0x0f4;
constexpr uint32_t NV_PFALCON_FALCON_CPUCTL      = 0x100;
constexpr uint32_t NV_PFALCON_FALCON_BOOTVEC     = 0x104;
constexpr uint32_t NV_PFALCON_FALCON_DMACTL      = 0x10c;
constexpr uint32_t NV_PFALCON_FALCON_DMATRFBASE  = 0x110;
constexpr uint32_t NV_PFALCON_FALCON_DMATRFMOFFS = 0x114;
constexpr uint32_t NV_PFALCON_FALCON_DMATRFCMD   = 0x118;
constexpr uint32_t NV_PFALCON_FALCON_DMATRFFBOFFS= 0x11c;
constexpr uint32_t NV_PFALCON_FALCON_DMATRFBASE1 = 0x128;
constexpr uint32_t NV_PFALCON_FALCON_CPUCTL_ALIAS= 0x130;
constexpr uint32_t NV_PFALCON_FALCON_ENGINE      = 0x3c0; // engine reset (1->0)

// --- GSP Falcon host-interrupt registers (offsets from NV_PGSP_FALCON_BASE) ---
// open-gpu-kernel-modules ampere/ga102/dev_falcon_v4.h. The GSP raises SWGEN0 when it posts a
// message to the status ring; HALT signals a GSP crash. kgspService_TU102 (kernel_gsp_tu102.c:897)
// reads IRQSTAT, clears via IRQSCLR (edge), then re-fires via INTR_RETRIGGER on GA10x
// (kflcnIntrRetrigger_GA100, kernel_falcon_ga100.c:43). Reserved for a future framework-based GSP
// interrupt path (setup_msi + on_interrupt + wait_for_event); the bring-up itself busy-polls.
constexpr uint32_t NV_PFALCON_FALCON_IRQSCLR        = 0x004; // W1C: clear pending (edge) -- SET=write the bit
constexpr uint32_t NV_PFALCON_FALCON_IRQSTAT        = 0x008; // RO: pending interrupts
constexpr uint32_t NV_PFALCON_FALCON_IRQMSET        = 0x010; // W1S: enable (set mask bit)
constexpr uint32_t NV_PFALCON_FALCON_IRQMASK        = 0x018; // RO: current enable mask
constexpr uint32_t NV_PFALCON_FALCON_IRQDEST        = 0x01c; // R/W: HOST_*[15:0]=1->host, TARGET_*[31:16]
constexpr uint32_t NV_PFALCON_FALCON_INTR_RETRIGGER0= 0x3e8; // W: TRIGGER -> re-fire engine->host (GA10x)
constexpr uint32_t FALCON_IRQ_HALT                  = (1u << 4); // bit 4 (0x10)
constexpr uint32_t FALCON_IRQ_SWGEN0                = (1u << 6); // bit 6 (0x40)
constexpr uint32_t FALCON_IRQDEST_HOST_HALT         = (1u << 4); // route HALT  -> host
constexpr uint32_t FALCON_IRQDEST_HOST_SWGEN0       = (1u << 6); // route SWGEN0-> host
constexpr uint32_t FALCON_INTR_RETRIGGER_TRIGGER    = (1u << 0); // INTR_RETRIGGER.TRIGGER=TRUE

// --- GA10x top-level "GIN" CPU interrupt tree (NV_VIRTUAL_FUNCTION_PRIV_*, absolute BAR0 base) ---
// open-gpu-kernel-modules tu102/ga102 dev_vm.h; PF base = NV_VIRTUAL_FUNCTION_FULL_PHYS_OFFSET
// (tu102/dev_vm.h:26, shared by GA102 via HAL -- VERIFY on hardware). GSP = MC_ENGINE_IDX_GSP(49),
// whose stall vector lives in LEAF 6 / subtree 3 on Ampere (intr_ga100.c:147-155). enable = write
// the GSP's bit in LEAF_EN_SET(6) + subtree-3 bit in TOP_EN_SET(0); the tree raises one PCI MSI.
constexpr uint32_t NV_VF_PRIV_BASE              = 0x00B80000; // NV_VIRTUAL_FUNCTION_FULL_PHYS_OFFSET (PF)
constexpr uint32_t NV_VF_CPU_INTR_LEAF         = 0x1000;     // +i*4 : leaf status (W1C per-bit)
constexpr uint32_t NV_VF_CPU_INTR_LEAF_EN_SET  = 0x1200;     // +i*4 : W1S enable
constexpr uint32_t NV_VF_CPU_INTR_LEAF_EN_CLR  = 0x1400;     // +i*4 : W1C disable
constexpr uint32_t NV_VF_CPU_INTR_TOP_EN_SET   = 0x1608;     // +i*4 : W1S subtree enable
constexpr uint32_t GSP_INTR_LEAF_IDX           = 6;          // Ampere stall engines -> leaf 6
constexpr uint32_t GSP_INTR_SUBTREE            = 3;          // stall subtree (TOP word 0, bit 3)

// Falcon field bits / values
constexpr uint32_t FALCON_CPUCTL_STARTCPU      = (1u << 1); // STARTCPU 1:1
constexpr uint32_t FALCON_CPUCTL_HALTED        = (1u << 4); // HALTED 4:4
constexpr uint32_t FALCON_HWCFG2_MEM_SCRUBBING = (1u << 12);// 0 == DONE
constexpr uint32_t FALCON_DMATRFCMD_IDLE       = (1u << 1); // 1 == idle/done
constexpr uint32_t FALCON_DMATRFCMD_SIZE_256B  = (6u << 8); // SIZE 10:8 = 0x6
constexpr uint32_t FALCON_DMATRFCMD_IMEM       = (1u << 4); // IMEM 4:4
constexpr uint32_t FALCON_DMATRFCMD_SEC1       = (1u << 2); // SEC 3:2 = 1
constexpr uint32_t FLCN_BLK_ALIGNMENT          = 256;

// FBIF registers (offset from the FBIF base)
constexpr uint32_t NV_PFALCON_FBIF_TRANSCFG    = 0x000; // TRANSCFG(0)
constexpr uint32_t NV_PFALCON_FBIF_CTL         = 0x024;
constexpr uint32_t FBIF_TRANSCFG_TARGET_MASK            = 0x3;
constexpr uint32_t FBIF_TRANSCFG_TARGET_COHERENT_SYSMEM = 0x1;       // bits 1:0
constexpr uint32_t FBIF_TRANSCFG_MEM_TYPE_PHYSICAL      = (1u << 2); // bit 2
constexpr uint32_t FBIF_CTL_ALLOW_PHYS_NO_CTX          = (1u << 7); // bit 7

// BROM / PKC registers (offset from the RISC-V/FALCON2 base)
constexpr uint32_t NV_PFALCON2_FALCON_MOD_SEL            = 0x180;
constexpr uint32_t NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID = 0x198;
constexpr uint32_t NV_PFALCON2_FALCON_BROM_ENGIDMASK     = 0x19c;
constexpr uint32_t NV_PFALCON2_FALCON_BROM_PARAADDR0     = 0x210;
constexpr uint32_t NV_PFALCON2_FALCON_MOD_SEL_ALGO_RSA3K = 0x1;
constexpr uint32_t NV_PRISCV_RISCV_BCR_CTRL             = 0x668; // riscv-base relative
constexpr uint32_t NV_PRISCV_RISCV_CPUCTL              = 0x388; // riscv-base relative
constexpr uint32_t PRISCV_RISCV_CPUCTL_ACTIVE_STAT     = (1u << 7);  // ACTIVE_STAT 7:7
constexpr uint32_t BCR_CTRL_VALID                      = (1u << 0);  // HW-set status
constexpr uint32_t BCR_CTRL_CORE_SELECT                = (1u << 4);  // 0=FALCON, 1=RISCV
constexpr uint32_t BCR_CTRL_CORE_SELECT_FALCON         = 0x0;        // write to select FALCON
constexpr uint32_t BCR_CTRL_CORE_SELECT_RISCV          = (1u << 4);  // write to select RISC-V
constexpr uint32_t BCR_CTRL_BRFETCH                    = (1u << 8);  // dev_riscv_pri.h BRFETCH 8:8
// reset-into-RISC-V BCR value: CORE_SELECT=RISCV | VALID | BRFETCH = 0x111
constexpr uint32_t BCR_CTRL_RISCV_BOOT = BCR_CTRL_CORE_SELECT_RISCV | BCR_CTRL_VALID | BCR_CTRL_BRFETCH;
// NV_PRISCV_RISCV_CORE_SWITCH_RISCV_STATUS (riscv-base relative), ACTIVE_STAT 0:0.
constexpr uint32_t NV_PRISCV_RISCV_CORE_SWITCH_RISCV_STATUS = 0x240;
constexpr uint32_t RISCV_STATUS_ACTIVE_STAT           = (1u << 0); // 1 = RISC-V active
constexpr uint32_t FALCON_HWCFG2_RESET_READY           = (1u << 31); // RESET_READY 31:31
constexpr uint32_t FALCON_HWCFG2_RISCV                 = (1u << 10); // RISCV present 10:10
constexpr uint32_t FALCON_CPUCTL_ALIAS_EN              = (1u << 6);  // ALIAS_EN 6:6
constexpr uint32_t FALCON_CPUCTL_ALIAS_STARTCPU        = (1u << 1);  // CPUCTL_ALIAS STARTCPU

// FB size / WPR2 / FRTS verification (absolute BAR0 offsets)
constexpr uint32_t NV_USABLE_FB_SIZE_IN_MB        = 0x001183a4; // SECURE_SCRATCH_GROUP_42, MiB
constexpr uint32_t NV_PFB_PRI_MMU_WPR2_ADDR_LO    = 0x001fa824; // VAL 31:4
constexpr uint32_t NV_PFB_PRI_MMU_WPR2_ADDR_HI    = 0x001fa828; // VAL 31:4
constexpr uint32_t NV_PFB_WPR2_ADDR_SHIFT         = 4;
constexpr uint32_t NV_PFB_WPR2_ADDR_ALIGNMENT     = 12;        // frtsOffset >> 12 == VAL
constexpr uint32_t NV_PBUS_VBIOS_SCRATCH_0E       = 0x00001438; // 0x1400 + 0x0E*4

// FUSE (absolute BAR0): GSP ucode version fuse array
constexpr uint32_t NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION  = 0x008241c0; // + 4*index
constexpr uint32_t NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION = 0x00824140; // + 4*index (Booter sig)

// FB layout constants (kgspCalculateFbLayout_TU102)
constexpr uint32_t FRTS_REGION_SIZE      = 0x100000; // 1 MB
constexpr uint32_t FRTS_REGION_SIZE_4K   = 0x100;    // 1 MB in 4K units
constexpr uint32_t VBIOS_WORKSPACE_SIZE  = 0x20000;
constexpr uint32_t FB_WPR_ALIGNMENT      = 0x20000;

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_REGS_H
