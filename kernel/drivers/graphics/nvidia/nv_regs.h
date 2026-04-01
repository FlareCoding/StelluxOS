#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_REGS_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_REGS_H

#include "common/types.h"

// =============================================================================
// NVIDIA GPU BAR0 MMIO Register Offsets
//
// All offsets are relative to BAR0 base address.
// Reference: Linux nouveau driver, envytools documentation
// =============================================================================

namespace nv::reg {

// ---------------------------------------------------------------------------
// PMC — Master Control (0x000000 - 0x000FFF)
// ---------------------------------------------------------------------------

constexpr uint32_t PMC_BOOT_0             = 0x000000; // Chip ID register
constexpr uint32_t PMC_BOOT_1             = 0x000004; // Endianness

// PMC_BOOT_0 field masks
constexpr uint32_t PMC_BOOT_0_CHIPSET_MASK = 0x1FF00000; // bits [28:20]
constexpr uint32_t PMC_BOOT_0_CHIPSET_SHIFT = 20;
constexpr uint32_t PMC_BOOT_0_CHIPREV_MASK = 0x000000FF; // bits [7:0]
constexpr uint32_t PMC_BOOT_0_FAMILY_MASK  = 0x1F000000; // bits [28:24] for family test

// PMC_ENABLE — engine enable registers
constexpr uint32_t PMC_ENABLE_LEGACY      = 0x000200; // Legacy engine enable (NV04-GP10x)
constexpr uint32_t PMC_ENABLE             = 0x000600; // GA100+ engine enable

// PMC interrupt registers
constexpr uint32_t PMC_INTR_STATUS_0      = 0x000100;
constexpr uint32_t PMC_INTR_EN_0          = 0x000140;
constexpr uint32_t PMC_INTR_ALLOW_0       = 0x000160; // GP100+ allow (per leaf)
constexpr uint32_t PMC_INTR_BLOCK_0       = 0x000180; // GP100+ block (per leaf)

// PMC interrupt bit assignments (consistent GF100+)
constexpr uint32_t PMC_INTR_DISP          = 0x04000000; // bit 26 — display engine
constexpr uint32_t PMC_INTR_GPIO_I2C      = 0x00200000; // bit 21 — GPIO/I2C
constexpr uint32_t PMC_INTR_TIMER         = 0x00100000; // bit 20 — timer

// ---------------------------------------------------------------------------
// PBUS — Bus Control (0x001000 - 0x001FFF)
// ---------------------------------------------------------------------------

constexpr uint32_t PBUS_PCI_NV_19         = 0x001850; // ROM access disable

// PRAMIN window control
constexpr uint32_t PRAMIN_WINDOW          = 0x001700; // PRAMIN window address (<<16)

// ---------------------------------------------------------------------------
// PTIMER — Timer (0x009000 - 0x009FFF)
// ---------------------------------------------------------------------------

constexpr uint32_t PTIMER_TIME_0          = 0x009400; // Low 32 bits of time
constexpr uint32_t PTIMER_TIME_1          = 0x009410; // High 32 bits of time

// ---------------------------------------------------------------------------
// PGPIO — GPIO Controller (0x00D000 - 0x00DFFF)
// ---------------------------------------------------------------------------

// Per-GPIO-line register: 0x00d610 + (line * 4)
constexpr uint32_t GPIO_PIN_BASE          = 0x00D610;
constexpr uint32_t GPIO_PIN_STRIDE        = 0x04;

// GPIO pin register bits
constexpr uint32_t GPIO_PIN_OUTPUT_VAL    = (1 << 12); // Output value
constexpr uint32_t GPIO_PIN_DIRECTION     = (1 << 13); // 0=output, 1=input (inverted)
constexpr uint32_t GPIO_PIN_INPUT_VAL     = (1 << 14); // Read-back input state

// GPIO update trigger
constexpr uint32_t GPIO_UPDATE            = 0x00D604;
constexpr uint32_t GPIO_UPDATE_TRIGGER    = 0x00000001;

// GPIO interrupt registers (GK104+)
constexpr uint32_t GPIO_INTR_STAT_0       = 0x00DC00; // Status for lines 0-15
constexpr uint32_t GPIO_INTR_EN_0         = 0x00DC08; // Enable for lines 0-15
constexpr uint32_t GPIO_INTR_STAT_1       = 0x00DC80; // Status for lines 16-31
constexpr uint32_t GPIO_INTR_EN_1         = 0x00DC88; // Enable for lines 16-31

// ---------------------------------------------------------------------------
// I2C — Bus Control Registers (via PNVIO/PGPIO)
// ---------------------------------------------------------------------------

// I2C bus register: 0x00d014 + (port * 0x20)
constexpr uint32_t I2C_PORT_BASE          = 0x00D014;
constexpr uint32_t I2C_PORT_STRIDE        = 0x20;

// I2C bus register bits
constexpr uint32_t I2C_SCL_OUT            = (1 << 0); // Drive SCL: 1=release, 0=pull low
constexpr uint32_t I2C_SDA_OUT            = (1 << 1); // Drive SDA: 1=release, 0=pull low
constexpr uint32_t I2C_INIT_BITS          = 0x00000007; // Init value (release both + bit 2)
constexpr uint32_t I2C_SCL_SENSE          = (1 << 4); // SCL read-back
constexpr uint32_t I2C_SDA_SENSE          = (1 << 5); // SDA read-back

// I2C pad mode registers (shared I2C/AUX pads)
// Pad base = 0x00e500 + (pad_index * 0x50)
constexpr uint32_t I2C_PAD_BASE           = 0x00E500;
constexpr uint32_t I2C_PAD_STRIDE         = 0x50;

// I2C pad mode control register offsets (from pad base)
constexpr uint32_t I2C_PAD_MODE           = 0x00; // Mode: I2C/AUX/OFF
constexpr uint32_t I2C_PAD_ENABLE         = 0x0C; // Enable: bit 0

// I2C pad mode values for PAD_MODE register
constexpr uint32_t I2C_PAD_MODE_I2C_MASK  = 0x0000C003;
constexpr uint32_t I2C_PAD_MODE_I2C_VAL   = 0x0000C001;
constexpr uint32_t I2C_PAD_MODE_AUX_MASK  = 0x0000C003;
constexpr uint32_t I2C_PAD_MODE_AUX_VAL   = 0x00000002;
constexpr uint32_t I2C_PAD_ENABLE_BIT     = 0x00000001;

// I2C timing parameters (nanoseconds)
constexpr uint64_t I2C_T_TIMEOUT_NS       = 2200000; // 2.2ms clock stretch timeout
constexpr uint64_t I2C_T_RISEFALL_NS      = 1000;    // 1µs rise/fall
constexpr uint64_t I2C_T_HOLD_NS          = 5000;    // 5µs hold time

// I2C DDC addresses
constexpr uint8_t  I2C_DDC_ADDR           = 0x50;    // EDID device address (7-bit)
constexpr uint8_t  I2C_DDC_ADDR_WR        = 0xA0;    // DDC write (addr<<1 | 0)
constexpr uint8_t  I2C_DDC_ADDR_RD        = 0xA1;    // DDC read (addr<<1 | 1)

// ---------------------------------------------------------------------------
// DP AUX Channel Registers
// ---------------------------------------------------------------------------

// AUX channel base: 0x00e4c0 + (ch * 0x50)
constexpr uint32_t AUX_CH_BASE            = 0x00E4C0;
constexpr uint32_t AUX_CH_STRIDE          = 0x50;

// AUX register offsets (from channel base)
constexpr uint32_t AUX_TX_DATA            = 0x00; // TX buffer (16 bytes)
constexpr uint32_t AUX_RX_DATA            = 0x10; // RX buffer (16 bytes)
constexpr uint32_t AUX_ADDR               = 0x20; // DP AUX address (20-bit)
constexpr uint32_t AUX_CTRL               = 0x24; // Control register
constexpr uint32_t AUX_STAT               = 0x28; // Status register

// AUX control register bits
constexpr uint32_t AUX_CTRL_RESET         = (1 << 31);
constexpr uint32_t AUX_CTRL_REQ_OWN       = (1 << 20); // Request ownership
constexpr uint32_t AUX_CTRL_ACK_OWN       = (1 << 24); // Ownership ack
constexpr uint32_t AUX_CTRL_TRIGGER       = (1 << 16); // Transaction trigger
constexpr uint32_t AUX_CTRL_TYPE_SHIFT    = 12;
constexpr uint32_t AUX_CTRL_TYPE_MASK     = (0xF << 12);
constexpr uint32_t AUX_CTRL_ADDR_ONLY     = (1 << 8);

// AUX transaction types (value goes into bits [15:12] via CTRL_TYPE_SHIFT)
// Per nouveau g94_aux_xfer(): cmd is placed in bits [15:12] of CTRL
constexpr uint32_t AUX_TYPE_I2C_WR        = 0x0;
constexpr uint32_t AUX_TYPE_I2C_RD        = 0x1;
constexpr uint32_t AUX_TYPE_I2C_WR_STOP   = 0x4; // I2C write with STOP (MOT=0)
constexpr uint32_t AUX_TYPE_I2C_RD_STOP   = 0x5; // I2C read with STOP (MOT=0)
constexpr uint32_t AUX_TYPE_NATIVE_WR     = 0x8; // Native AUX write
constexpr uint32_t AUX_TYPE_NATIVE_RD     = 0x9; // Native AUX read

// AUX status register bits
constexpr uint32_t AUX_STAT_SINK_DET      = (1 << 28); // Sink/HPD detected
constexpr uint32_t AUX_STAT_REPLY_MASK    = 0x000F0000;
constexpr uint32_t AUX_STAT_REPLY_SHIFT   = 16;
constexpr uint32_t AUX_STAT_TIMEOUT       = (1 << 8);
constexpr uint32_t AUX_STAT_RX_SIZE_MASK  = 0x1F;

// AUX reply codes
constexpr uint32_t AUX_REPLY_ACK          = 0x0;
constexpr uint32_t AUX_REPLY_NACK         = 0x2;
constexpr uint32_t AUX_REPLY_DEFER        = 0x8;

// AUX auto-DPCD register (GM200+)
// 0x00d968 + (aux_ch * 0x50)
constexpr uint32_t AUX_AUTO_DPCD_BASE     = 0x00D968;
constexpr uint32_t AUX_AUTO_DPCD_DISABLE  = (1 << 16);

// ---------------------------------------------------------------------------
// PGSP — GPU System Processor (0x118000+)
// ---------------------------------------------------------------------------

// GFW (GPU Firmware) boot status registers
constexpr uint32_t GFW_BOOT_STATUS        = 0x118128;
constexpr uint32_t GFW_BOOT_PROGRESS      = 0x118234;

// GFW_BOOT_STATUS bits
constexpr uint32_t GFW_BOOT_STARTED       = 0x00000001; // bit 0

// GFW_BOOT_PROGRESS values
constexpr uint32_t GFW_BOOT_COMPLETED     = 0xFF;
constexpr uint32_t GFW_BOOT_PROGRESS_MASK = 0x000000FF;

// ---------------------------------------------------------------------------
// Display Fuse Detection
// ---------------------------------------------------------------------------

// Display fuse register — if bit 0 set, display engine is fused off
constexpr uint32_t DISP_FUSE_GM107        = 0x021C04; // GM107-TU1xx
constexpr uint32_t DISP_FUSE_GA100        = 0x820C04; // GA100+ (Ampere)
constexpr uint32_t DISP_FUSE_DISABLED     = 0x00000001;

// ---------------------------------------------------------------------------
// PROM / PRAMIN — VBIOS Access
// ---------------------------------------------------------------------------

constexpr uint32_t PROM_BASE              = 0x300000; // Direct ROM read (1MB window)
constexpr uint32_t PRAMIN_BASE            = 0x700000; // Instance memory read (1MB)
constexpr uint32_t VBIOS_INST_GV100       = 0x625F04; // VBIOS instance ptr (Volta+)
constexpr uint32_t VBIOS_INST_GM100       = 0x619F04; // VBIOS instance ptr (Maxwell+)

// VBIOS instance register bits
constexpr uint32_t VBIOS_INST_ENABLED     = (1 << 3); // Window enabled
constexpr uint32_t VBIOS_INST_TARGET_MASK = 0x00000003; // bits [1:0], 1 = VRAM

// PCI ROM shadow control (via BAR0 NV_PBUS)
constexpr uint32_t PCI_ROM_SHADOW         = 0x000050;  // NV config alias
constexpr uint32_t PCI_ROM_SHADOW_ENABLE  = 0x00000001;

// ROM signatures
constexpr uint16_t ROM_SIG_PCI            = 0xAA55;
constexpr uint16_t ROM_SIG_ALT1           = 0xBB77;
constexpr uint16_t ROM_SIG_NV             = 0x4E56; // "NV"
constexpr uint32_t PCIR_SIGNATURE         = 0x52494350; // "PCIR" (LE)

// PCI Expansion ROM BAR (config space offset)
constexpr uint16_t PCI_EXPANSION_ROM_BAR  = 0x30;
constexpr uint32_t PCI_ROM_ENABLE         = 0x00000001;
constexpr uint32_t PCI_ROM_ADDR_MASK      = 0xFFFFF800;

// ---------------------------------------------------------------------------
// PDISPLAY — Display Engine (0x610000+)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Falcon Microcontroller Registers (relative to falcon base)
// GSP Falcon base: 0x110000, SEC2 Falcon base: 0x840000
// ---------------------------------------------------------------------------

constexpr uint32_t FALCON_GSP_BASE           = 0x110000;
constexpr uint32_t FALCON_SEC2_BASE          = 0x840000;

// Core control
constexpr uint32_t FALCON_IRQSCLR            = 0x004;
constexpr uint32_t FALCON_IRQSTAT            = 0x008;
constexpr uint32_t FALCON_IRQMASK            = 0x014;
constexpr uint32_t FALCON_MAILBOX0           = 0x040;
constexpr uint32_t FALCON_MAILBOX1           = 0x044;
constexpr uint32_t FALCON_IRQMODE            = 0x048;
constexpr uint32_t FALCON_RM                 = 0x084;
constexpr uint32_t FALCON_HWCFG2             = 0x0F4;
constexpr uint32_t FALCON_CPUCTL             = 0x100;
constexpr uint32_t FALCON_BOOTVEC            = 0x104;
constexpr uint32_t FALCON_HWCFG_CODE_DATA    = 0x108;
constexpr uint32_t FALCON_DMACTL             = 0x10C;
constexpr uint32_t FALCON_DMATRFBASE         = 0x110;
constexpr uint32_t FALCON_DMATRFMOFFS        = 0x114;
constexpr uint32_t FALCON_DMATRFCMD          = 0x118;
constexpr uint32_t FALCON_DMATRFFBOFFS       = 0x11C;
constexpr uint32_t FALCON_DMATRFBASE1        = 0x128;
constexpr uint32_t FALCON_CPUCTL_ALIAS       = 0x130;
constexpr uint32_t FALCON_ENGINE             = 0x3C0;

// FBIF (Framebuffer Interface)
constexpr uint32_t FALCON_FBIF_TRANSCFG      = 0x600;
constexpr uint32_t FALCON_FBIF_CTL           = 0x624;

// DMEM PIO access
constexpr uint32_t FALCON_DMEMC0             = 0x1C0;
constexpr uint32_t FALCON_DMEMD0             = 0x1C4;

// BROM registers (at base + 0x1000)
constexpr uint32_t FALCON_BROM_MOD_SEL       = 0x1180;
constexpr uint32_t FALCON_BROM_UCODE_ID      = 0x1198;
constexpr uint32_t FALCON_BROM_ENGIDMASK     = 0x119C;
constexpr uint32_t FALCON_BROM_PARAADDR      = 0x1210;

// RISC-V core select (GA102+)
constexpr uint32_t FALCON_RISCV_BCR_CTRL     = 0x1668;

// CPUCTL bits
constexpr uint32_t FALCON_CPUCTL_STARTCPU    = (1 << 1);
constexpr uint32_t FALCON_CPUCTL_HALTED      = (1 << 4);
constexpr uint32_t FALCON_CPUCTL_ALIAS_EN    = (1 << 6);

// HWCFG2 bits
constexpr uint32_t FALCON_HWCFG2_RESET_READY = (1u << 31);
constexpr uint32_t FALCON_HWCFG2_MEM_SCRUB   = (1 << 12);

// DMA transfer command bits
constexpr uint32_t FALCON_DMA_IDLE           = (1 << 1);
constexpr uint32_t FALCON_DMA_SEC            = (1 << 2);
constexpr uint32_t FALCON_DMA_IMEM           = (1 << 4);
constexpr uint32_t FALCON_DMA_SIZE_256       = (6 << 8); // ilog2(256) - 2 = 6

// FBIF TRANSCFG values
constexpr uint32_t FALCON_FBIF_TARGET_COHERENT = 0x01; // bits[1:0]
constexpr uint32_t FALCON_FBIF_MEM_PHYSICAL    = 0x04; // bit[2]
constexpr uint32_t FALCON_FBIF_CTL_ALLOW_PHYS  = (1 << 7);

// Fuse version registers (absolute BAR0 addresses)
constexpr uint32_t FUSE_GSP_BASE             = 0x8241C0; // + (ucode_id-1)*4
constexpr uint32_t FUSE_SEC2_BASE            = 0x824140; // + (ucode_id-1)*4

// FWSEC error/status registers (absolute BAR0 addresses)
constexpr uint32_t FWSEC_SCRATCH_E           = 0x001438; // FRTS error (upper 16 bits)
constexpr uint32_t WPR2_ADDR_LO              = 0x1FA824; // WPR2 lower bound
constexpr uint32_t WPR2_ADDR_HI              = 0x1FA828; // WPR2 upper bound

// VRAM size register (GA102+)
constexpr uint32_t VIDMEM_SIZE_GA102         = 0x1183A4; // value << 20 = bytes

// VGA workspace register
constexpr uint32_t VGA_WORKSPACE_BASE        = 0x625F04;

// ---------------------------------------------------------------------------
// PDISPLAY — Display Engine (0x610000+)
// ---------------------------------------------------------------------------

// Display global control
constexpr uint32_t DISP_CAPS_HEAD_MASK    = 0x610060; // bits [7:0] = head present
constexpr uint32_t DISP_CAPS_SOR_MASK     = 0x610060; // bits [15:8] = SOR present
constexpr uint32_t DISP_CAPS_WIN_MASK     = 0x610064; // Window present mask
constexpr uint32_t DISP_CAPS_MAX          = 0x610074; // Max counts
constexpr uint32_t DISP_ENABLE            = 0x610078; // bit 0 = display enable
constexpr uint32_t DISP_INST_MEM_0        = 0x610010; // Instance memory config
constexpr uint32_t DISP_INST_MEM_1        = 0x610014; // Instance memory addr >>16

// Display ownership
constexpr uint32_t DISP_OWNER             = 0x6254E8; // bit 1=busy, bit 0=release

// VPLL programming (GA100+, per head)
// Register = base + (head * 0x40)
constexpr uint32_t VPLL_CTRL0_BASE        = 0x00EF00; // VPLL control
constexpr uint32_t VPLL_COEFF_BASE        = 0x00EF04; // (P << 16) | M
constexpr uint32_t VPLL_FRAC_BASE         = 0x00EF18; // (N << 16) | fN
constexpr uint32_t VPLL_STRIDE            = 0x40;
constexpr uint32_t VPLL_TRIGGER_BASE      = 0x00E9C0; // per head, stride 0x04

// SOR clock (GA102-specific)
constexpr uint32_t SOR_CLK_CTRL_BASE      = 0x00EC04; // per SOR
constexpr uint32_t SOR_CLK_CTRL2_BASE     = 0x00EC08; // per SOR
constexpr uint32_t SOR_CLK_STRIDE         = 0x10;

// ---------------------------------------------------------------------------
// Core Channel / EVO Shadow Registers (0x680000+)
// ---------------------------------------------------------------------------

// Head state (core channel, per-head stride 0x400)
// Assembly (asy) at 0x682000, Armed (arm) at 0x68a000
constexpr uint32_t HEAD_ASY_BASE          = 0x682000;
constexpr uint32_t HEAD_ARM_BASE          = 0x68A000;
constexpr uint32_t HEAD_STRIDE            = 0x400;

// Head register offsets (from HEAD_ASY_BASE + head * HEAD_STRIDE)
constexpr uint32_t HEAD_SET_CONTROL       = 0x000;
constexpr uint32_t HEAD_SET_CONTROL_DEPTH = 0x004; // bits [7:4] = depth
constexpr uint32_t HEAD_SET_OUTPUT_RES    = 0x008;
constexpr uint32_t HEAD_SET_PIXEL_CLOCK   = 0x00C; // Pixel clock in Hz
constexpr uint32_t HEAD_SET_DISPLAY_ID    = 0x060;
constexpr uint32_t HEAD_SET_DISPLAY_TOTAL = 0x064; // [31:16]=vtotal, [15:0]=htotal
constexpr uint32_t HEAD_SET_SYNC_END      = 0x068; // [31:16]=vsynce, [15:0]=hsynce
constexpr uint32_t HEAD_SET_BLANK_END     = 0x06C; // [31:16]=vblanke, [15:0]=hblanke
constexpr uint32_t HEAD_SET_BLANK_START   = 0x070; // [31:16]=vblanks, [15:0]=hblanks

// Depth values for HEAD_SET_CONTROL_DEPTH bits [7:4]
constexpr uint32_t HEAD_DEPTH_18BPP      = 0x10; // (1 << 4)
constexpr uint32_t HEAD_DEPTH_24BPP      = 0x40; // (4 << 4)
constexpr uint32_t HEAD_DEPTH_30BPP      = 0x50; // (5 << 4)

// SOR state (core channel, per-SOR stride 0x20)
constexpr uint32_t SOR_ASY_BASE          = 0x680300;
constexpr uint32_t SOR_ARM_BASE          = 0x688300;
constexpr uint32_t SOR_STATE_STRIDE      = 0x20;

// SOR state register bits
constexpr uint32_t SOR_STATE_PROTO_MASK  = 0x00000F00; // bits [11:8] = protocol
constexpr uint32_t SOR_STATE_PROTO_SHIFT = 8;
constexpr uint32_t SOR_STATE_HEAD_MASK   = 0x000000FF; // bits [7:0] = head mask

// SOR protocol values
constexpr uint32_t SOR_PROTO_LVDS_SL     = 0; // LVDS single-link
constexpr uint32_t SOR_PROTO_TMDS_SL     = 1; // TMDS single-link (HDMI/DVI)
constexpr uint32_t SOR_PROTO_TMDS_DL_B   = 2; // TMDS dual-link B
constexpr uint32_t SOR_PROTO_TMDS_DL_AB  = 5; // TMDS dual-link A+B
constexpr uint32_t SOR_PROTO_DP_A        = 8; // DP link A
constexpr uint32_t SOR_PROTO_DP_B        = 9; // DP link B

// ---------------------------------------------------------------------------
// SOR — Serial Output Resource (0x61C000+)
// ---------------------------------------------------------------------------

// SOR base: 0x61c000 + (sor_id * 0x800)
constexpr uint32_t SOR_BASE               = 0x61C000;
constexpr uint32_t SOR_STRIDE             = 0x800;

// SOR register offsets (from SOR base)
constexpr uint32_t SOR_CAP               = 0x000; // Capability readback
constexpr uint32_t SOR_PWR               = 0x004; // Power control
constexpr uint32_t SOR_SEQ_CTRL          = 0x030; // Sequencer control
constexpr uint32_t SOR_DP_LANE_PWR       = 0x034; // DP lane power trigger
constexpr uint32_t SOR_SEQ_PROG_BASE     = 0x040; // Sequencer program entries

// SOR power register bits
constexpr uint32_t SOR_PWR_TRIGGER       = (1 << 31); // Trigger/busy
constexpr uint32_t SOR_PWR_NORMAL        = (1 << 0);  // Normal power up
constexpr uint32_t SOR_PWR_SLEEP         = (1 << 16); // Sleep power up

// SOR sequencer control bits
constexpr uint32_t SOR_SEQ_BUSY          = (1 << 28);

// SOR DP lane power trigger
constexpr uint32_t SOR_DP_LANE_TRIGGER   = (1 << 31);

// SOR DP link control (link A at +0x10C, link B at +0x10C+0x80)
constexpr uint32_t SOR_DP_CTRL           = 0x10C;
constexpr uint32_t SOR_DP_PATTERN        = 0x110; // Training pattern (link A)
constexpr uint32_t SOR_DP_DRIVE_DC       = 0x118; // Drive current (4 lanes packed)
constexpr uint32_t SOR_DP_DRIVE_PE       = 0x120; // Pre-emphasis (4 lanes packed)
constexpr uint32_t SOR_DP_WATERMARK      = 0x128; // Watermark / active symbol
constexpr uint32_t SOR_DP_PATTERN_B      = 0x12C; // Training pattern (link B)
constexpr uint32_t SOR_DP_LANE_EN        = 0x130; // Lane enable + TX_PU
constexpr uint32_t SOR_DP_POST_CURSOR2   = 0x13C; // Post-cursor2 (4 lanes packed)
constexpr uint32_t SOR_LINK_B_OFFSET     = 0x080; // Link B register offset from link A

// SOR DP control register bits
constexpr uint32_t SOR_DP_CTRL_LANE_MASK  = 0x000F0000; // bits [19:16] = lane enable
constexpr uint32_t SOR_DP_CTRL_LANE_SHIFT = 16;
constexpr uint32_t SOR_DP_CTRL_MST        = (1 << 30);  // MST enable
constexpr uint32_t SOR_DP_CTRL_EF         = (1 << 14);  // Enhanced Framing

// SOR clock/bandwidth register (0x612300 + sor_id * 0x800)
constexpr uint32_t SOR_CLK_CONFIG_BASE    = 0x612300;
constexpr uint32_t SOR_CLK_CONFIG_STRIDE  = 0x800;

// SOR routing table (GM200+)
constexpr uint32_t SOR_ROUTE_LINK_A       = 0x612308; // + (output_or * 0x100)
constexpr uint32_t SOR_ROUTE_LINK_B       = 0x612388; // + (output_or * 0x100)
constexpr uint32_t SOR_ROUTE_STRIDE       = 0x100;

// HDMI SCDC scrambling control
constexpr uint32_t SOR_HDMI_SCDC_BASE     = 0x61C5BC; // + soff

// ---------------------------------------------------------------------------
// Head Direct MMIO (0x616000+)
// ---------------------------------------------------------------------------

// Per-head MMIO base: 0x616000 + (head_id * 0x800)
constexpr uint32_t HEAD_MMIO_BASE         = 0x616000;
constexpr uint32_t HEAD_MMIO_STRIDE       = 0x800;

// Head MMIO register offsets
constexpr uint32_t HEAD_RG_CAP            = 0x300; // RG capability / sync
constexpr uint32_t HEAD_RG_VLINE          = 0x330; // Current vline (locks hline)
constexpr uint32_t HEAD_RG_HLINE          = 0x334; // Current hline
constexpr uint32_t HEAD_HDMI_CTRL         = 0x5C0; // HDMI control

// ---------------------------------------------------------------------------
// Display Interrupt Registers (0x611xxx)
// ---------------------------------------------------------------------------

// Interrupt status (read to check, write to clear)
constexpr uint32_t DISP_INTR_HEAD_TIMING_BASE = 0x611800; // + (head * 4)
constexpr uint32_t DISP_INTR_EXC_WIN     = 0x61184C;
constexpr uint32_t DISP_INTR_EXC_WINIM   = 0x611850;
constexpr uint32_t DISP_INTR_EXC_OTHER   = 0x611854;
constexpr uint32_t DISP_INTR_AWAKEN_WIN  = 0x611858;
constexpr uint32_t DISP_INTR_AWAKEN_OTHER = 0x61185C;
constexpr uint32_t DISP_INTR_SUPERVISOR  = 0x611C30;
constexpr uint32_t DISP_INTR_MASTER      = 0x611EC0;

// Interrupt mask registers
constexpr uint32_t DISP_MASK_HEAD_TIMING_BASE = 0x611CC0; // + (head * 4)
constexpr uint32_t DISP_MASK_EXC_WIN     = 0x611CE4;
constexpr uint32_t DISP_MASK_EXC_WINIM   = 0x611CE8;
constexpr uint32_t DISP_MASK_EXC_OTHER   = 0x611CEC;
constexpr uint32_t DISP_MASK_CTRL_DISP   = 0x611CF0;
constexpr uint32_t DISP_MASK_OR          = 0x611CF4;

// Interrupt enable registers
constexpr uint32_t DISP_EN_HEAD_TIMING_BASE = 0x611D80; // + (head * 4)
constexpr uint32_t DISP_EN_EXC_WIN       = 0x611DA4;
constexpr uint32_t DISP_EN_EXC_WINIM     = 0x611DA8;
constexpr uint32_t DISP_EN_EXC_OTHER     = 0x611DAC;
constexpr uint32_t DISP_EN_CTRL_DISP     = 0x611DB0;
constexpr uint32_t DISP_EN_OR            = 0x611DB4;

// CTRL_DISP interrupt mask bits
constexpr uint32_t DISP_CTRL_AWAKEN      = 0x001; // bit 0
constexpr uint32_t DISP_CTRL_ERROR       = 0x002; // bit 1
constexpr uint32_t DISP_CTRL_SUPER1      = 0x004; // bit 2
constexpr uint32_t DISP_CTRL_SUPER2      = 0x008; // bit 3 (unused currently)
constexpr uint32_t DISP_CTRL_SUPER3      = 0x080; // bit 7
constexpr uint32_t DISP_CTRL_ALL         = 0x187; // AWAKEN|ERROR|SUPER1|SUPER2|SUPER3

// HEAD_TIMING interrupt bits
constexpr uint32_t HEAD_TIMING_VBLANK    = (1 << 2);

// Supervisor registers
constexpr uint32_t DISP_SUPER_STATUS     = 0x6107A8; // Write 0x80000000 to ack
constexpr uint32_t DISP_SUPER_HEAD_BASE  = 0x6107AC; // + (head * 4), per-head mask
constexpr uint32_t DISP_SUPER_PENDING    = 0x611860; // Write to ack supervisor

// Supervisor head mask bits
constexpr uint32_t SUPER_HEAD_MODE       = 0x00001000; // bit 12 — head mode change
constexpr uint32_t SUPER_HEAD_SOR        = 0x00010000; // bit 16 — SOR assignment

// ---------------------------------------------------------------------------
// HDMI InfoFrame Registers (0x6F0000+)
// ---------------------------------------------------------------------------

// Per-head stride 0x400
constexpr uint32_t HDMI_AVI_BASE          = 0x6F0000; // + (head * 0x400)
constexpr uint32_t HDMI_AVI_STRIDE        = 0x400;

// AVI InfoFrame register offsets
constexpr uint32_t HDMI_AVI_CTRL          = 0x000; // bit 0 = enable
constexpr uint32_t HDMI_AVI_HEADER        = 0x008;
constexpr uint32_t HDMI_AVI_SUBPACK0_LO   = 0x00C;
constexpr uint32_t HDMI_AVI_SUBPACK0_HI   = 0x010;
constexpr uint32_t HDMI_AVI_SUBPACK1_LO   = 0x014;
constexpr uint32_t HDMI_AVI_SUBPACK1_HI   = 0x018;

// Audio Clock Recovery
constexpr uint32_t HDMI_ACR               = 0x080; // Write 0x82000000

// General Control Packet
constexpr uint32_t HDMI_GCP_CTRL          = 0x0C0; // bit 0 = enable
constexpr uint32_t HDMI_GCP_DATA          = 0x0CC; // Write 0x00000010

// HDA capability detection
constexpr uint32_t HDA_SOR_CAP            = 0x08A15C; // bit n = SOR n has HDA

// ---------------------------------------------------------------------------
// Strap Pins
// ---------------------------------------------------------------------------

constexpr uint32_t PSTRAPS                = 0x101000; // Crystal frequency etc.
constexpr uint32_t PSTRAPS_CRYSTAL_MASK   = 0x00400040;

// Crystal frequency decoding from strap bits
// 0x00000000 → 13.5 MHz
// 0x00000040 → 14.318 MHz
// 0x00400000 → 27.0 MHz
// 0x00400040 → 25.0 MHz

// ---------------------------------------------------------------------------
// Display Pin Capabilities Lock
// ---------------------------------------------------------------------------

constexpr uint32_t DISP_PIN_CAP_LOCK      = 0x640008; // Write 0x00000021 for TU102+

// Capability shadow registers
constexpr uint32_t DISP_SOR_CAP_BASE      = 0x640000; // bit (8+i) = SOR i enable
constexpr uint32_t DISP_SOR_CAP_SHADOW    = 0x640144; // + (i * 0x08)
constexpr uint32_t DISP_HEAD_CAP_SHADOW   = 0x640048; // + (id * 0x020)
constexpr uint32_t DISP_HEAD_PC_SHADOW    = 0x640680; // + (id * 0x20)
constexpr uint32_t DISP_WIN_CAP_ENABLE    = 0x640004; // bit i = window i enable
constexpr uint32_t DISP_WIN_CAP_SHADOW    = 0x640780; // + (i * 0x20)
constexpr uint32_t DISP_WIN_CAP_COMMIT    = 0x64000C; // bit 8 = final commit
constexpr uint32_t DISP_IHUB_CAP_BASE     = 0x62E000; // + (i * 4)
constexpr uint32_t DISP_IHUB_CAP_SHADOW   = 0x640010; // + (i * 4)

// Display Core Channel status
constexpr uint32_t DISP_CORE_CHAN_STATUS   = 0x610630;

} // namespace nv::reg

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_REGS_H
