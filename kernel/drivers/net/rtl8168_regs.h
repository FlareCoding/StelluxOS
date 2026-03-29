#ifndef STELLUX_DRIVERS_NET_RTL8168_REGS_H
#define STELLUX_DRIVERS_NET_RTL8168_REGS_H

/**
 * Realtek RTL8111/RTL8168 PCIe Gigabit Ethernet register definitions.
 *
 * PCI ID: 10EC:8168. The RTL8111 (desktop) and RTL8168 (mobile) share
 * the same programming model. All registers are MMIO-mapped via BAR 2
 * (64-bit memory BAR at PCI config offset 0x18).
 *
 * References:
 *   - RTL8111B/RTL8168B Registers Datasheet Rev 1.0 (2006-01-26)
 *   - Linux kernel drivers/net/ethernet/realtek/r8169_main.c
 *   - FreeBSD sys/dev/re/if_re.c, sys/dev/rl/if_rlreg.h
 */

#include "common/types.h"

namespace drivers::rtl8168 {

// ============================================================================
// PCI identification
// ============================================================================

constexpr uint16_t RTL_VENDOR_ID             = 0x10EC;
constexpr uint16_t RTL_DEVICE_ID_8168        = 0x8168;

// BAR index: RTL8168 uses a 64-bit memory BAR starting at BAR 2
// (PCI config offset 0x18). This consumes BAR indices 2 and 3.
constexpr uint8_t  RTL_MMIO_BAR              = 2;

// ============================================================================
// MAC registers (offsets from MMIO base)
//
// Cross-referenced: datasheet §2.1, Linux enum rtl_registers,
// FreeBSD RL_IDR0 etc.
// ============================================================================

// MAC address (IDR0-IDR5). Must be read/written as 32-bit aligned accesses.
constexpr uint16_t REG_IDR0                  = 0x0000;  // ID register 0 (MAC byte 0)
constexpr uint16_t REG_IDR4                  = 0x0004;  // ID register 4 (MAC bytes 4-5)

// Multicast address register (MAR0-MAR7). 32-bit aligned access only.
constexpr uint16_t REG_MAR0                  = 0x0008;  // Multicast bits [63:32]
constexpr uint16_t REG_MAR4                  = 0x000C;  // Multicast bits [31:0]

// Dump Tally Counter Command (64-byte aligned physical address + command bit)
constexpr uint16_t REG_DTCCR                 = 0x0010;  // 64-bit

// TX descriptor start addresses (256-byte aligned, 64-bit)
constexpr uint16_t REG_TNPDS                 = 0x0020;  // TX Normal Priority Descriptor Start
constexpr uint16_t REG_THPDS                 = 0x0028;  // TX High Priority Descriptor Start

// Command register
constexpr uint16_t REG_CMD                   = 0x0037;  // 8-bit
constexpr uint8_t  CMD_RST                   = (1u << 4);
constexpr uint8_t  CMD_RE                    = (1u << 3);  // Receiver Enable
constexpr uint8_t  CMD_TE                    = (1u << 2);  // Transmitter Enable

// Transmit Priority Polling
constexpr uint16_t REG_TPPOLL               = 0x0038;  // 8-bit, write-only
constexpr uint8_t  TPPOLL_HPQ               = (1u << 7);  // High Priority Queue poll
constexpr uint8_t  TPPOLL_NPQ               = (1u << 6);  // Normal Priority Queue poll
constexpr uint8_t  TPPOLL_FSWINT            = (1u << 0);  // Forced Software Interrupt

// Interrupt Mask Register
constexpr uint16_t REG_IMR                   = 0x003C;  // 16-bit
// Interrupt Status Register
constexpr uint16_t REG_ISR                   = 0x003E;  // 16-bit

// Shared IMR/ISR bit definitions
constexpr uint16_t INT_TIMEOUT               = (1u << 14);
constexpr uint16_t INT_FEMP                  = (1u << 9);   // RX FIFO empty after full
constexpr uint16_t INT_SWINT                 = (1u << 8);   // Software interrupt
constexpr uint16_t INT_TDU                   = (1u << 7);   // TX Descriptor Unavailable
constexpr uint16_t INT_FOVW                  = (1u << 6);   // RX FIFO Overflow
constexpr uint16_t INT_LINKCHG               = (1u << 5);   // Link Change
constexpr uint16_t INT_RDU                   = (1u << 4);   // RX Descriptor Unavailable
constexpr uint16_t INT_TER                   = (1u << 3);   // TX Error
constexpr uint16_t INT_TOK                   = (1u << 2);   // TX OK
constexpr uint16_t INT_RER                   = (1u << 1);   // RX Error
constexpr uint16_t INT_ROK                   = (1u << 0);   // RX OK

// Aggregate masks for driver use
constexpr uint16_t INT_MASK_TX               = INT_TOK | INT_TER | INT_TDU;
constexpr uint16_t INT_MASK_RX               = INT_ROK | INT_RER | INT_RDU | INT_FOVW;
constexpr uint16_t INT_MASK_LINK             = INT_LINKCHG;
constexpr uint16_t INT_MASK_DEFAULT          = INT_MASK_TX | INT_MASK_RX |
                                               INT_MASK_LINK | INT_TIMEOUT;

// Transmit Configuration Register
constexpr uint16_t REG_TCR                   = 0x0040;  // 32-bit
constexpr uint32_t TCR_HWVERID_MASK          = 0x7CF00000; // bits [30:28, 26, 23, 22:20]
constexpr uint32_t TCR_IFG_MASK              = 0x03080000; // IFG[2:0]
constexpr uint32_t TCR_IFG_DEFAULT           = 0x03000000; // IFG=011 (96ns at 1G)
constexpr uint32_t TCR_MXDMA_MASK            = 0x00000700; // Max DMA burst size
constexpr uint32_t TCR_MXDMA_UNLIMITED       = 0x00000700; // 111 = unlimited
constexpr uint32_t TCR_NOCRC                 = (1u << 16);

// Receive Configuration Register
constexpr uint16_t REG_RCR                   = 0x0044;  // 32-bit
constexpr uint32_t RCR_RXFTH_MASK            = 0x0000E000; // RX FIFO threshold
constexpr uint32_t RCR_RXFTH_NONE            = 0x0000E000; // 111 = no threshold
constexpr uint32_t RCR_MXDMA_MASK            = 0x00000700; // Max DMA burst size
constexpr uint32_t RCR_MXDMA_UNLIMITED       = 0x00000700; // 111 = unlimited
constexpr uint32_t RCR_9356SEL               = (1u << 6);
constexpr uint32_t RCR_AER                   = (1u << 5);  // Accept Error Packets
constexpr uint32_t RCR_AR                    = (1u << 4);  // Accept Runt
constexpr uint32_t RCR_AB                    = (1u << 3);  // Accept Broadcast
constexpr uint32_t RCR_AM                    = (1u << 2);  // Accept Multicast
constexpr uint32_t RCR_APM                   = (1u << 1);  // Accept Physical Match
constexpr uint32_t RCR_AAP                   = (1u << 0);  // Accept All Packets

// Timer Count Register (125 MHz internal clock -> 8ns per tick)
constexpr uint16_t REG_TCTR                  = 0x0048;  // 32-bit

// 93C46/93C56 EEPROM Command Register
constexpr uint16_t REG_9346CR                = 0x0050;  // 8-bit
constexpr uint8_t  CFG_9346_LOCK             = 0x00;    // EEM=00 normal mode (locked)
constexpr uint8_t  CFG_9346_UNLOCK           = 0xC0;    // EEM=11 config write enable

// Configuration registers 0-5
constexpr uint16_t REG_CONFIG0               = 0x0051;
constexpr uint16_t REG_CONFIG1               = 0x0052;
constexpr uint16_t REG_CONFIG2               = 0x0053;
constexpr uint16_t REG_CONFIG3               = 0x0054;
constexpr uint16_t REG_CONFIG4               = 0x0055;
constexpr uint16_t REG_CONFIG5               = 0x0056;

// Timer interrupt register
constexpr uint16_t REG_TIMERINT              = 0x0058;

// PHY Access Register (indirect MDIO via MMIO)
constexpr uint16_t REG_PHYAR                 = 0x0060;  // 32-bit
constexpr uint32_t PHYAR_FLAG                = (1u << 31);  // 1=write, auto-clears; 0=read, auto-sets
constexpr uint32_t PHYAR_REG_SHIFT           = 16;
constexpr uint32_t PHYAR_REG_MASK            = 0x001F0000;
constexpr uint32_t PHYAR_DATA_MASK           = 0x0000FFFF;

// PHY Status Register (real-time, updated every ~300us)
constexpr uint16_t REG_PHYSTATUS             = 0x006C;  // 8-bit
constexpr uint8_t  PHYSTS_TXFLOW             = (1u << 6);
constexpr uint8_t  PHYSTS_RXFLOW             = (1u << 5);
constexpr uint8_t  PHYSTS_1000MF             = (1u << 4);  // 1000 Mbps full-duplex
constexpr uint8_t  PHYSTS_100M               = (1u << 3);
constexpr uint8_t  PHYSTS_10M                = (1u << 2);
constexpr uint8_t  PHYSTS_LINK               = (1u << 1);
constexpr uint8_t  PHYSTS_FULLDUP            = (1u << 0);

// Extended Register Access (ERIAR) — used by 8168g+ chips
constexpr uint16_t REG_ERIAR                 = 0x00DC;  // 32-bit
constexpr uint32_t ERIAR_FLAG                = (1u << 31);
constexpr uint32_t ERIAR_WRITE               = 0x80000000;
constexpr uint32_t ERIAR_READ                = 0x00000000;
constexpr uint32_t ERIAR_ADDR_MASK           = 0x0000FFFF;
constexpr uint32_t ERIAR_TYPE_SHIFT          = 16;

// Extended Register Data
constexpr uint16_t REG_ERIDR                 = 0x00D0;  // 32-bit

// RX packet Maximum Size
constexpr uint16_t REG_RMS                   = 0x00DA;  // 16-bit
constexpr uint16_t RMS_MAX                   = 0x3FFF;  // 14 bits, max 16383

// C+ Command Register
constexpr uint16_t REG_CPCR                  = 0x00E0;  // 16-bit (word access only)
constexpr uint16_t CPCR_RXVLAN              = (1u << 6);  // VLAN de-tag on RX
constexpr uint16_t CPCR_RXCHKSUM            = (1u << 5);  // RX checksum offload

// RX Descriptor Start Address (256-byte aligned, 64-bit)
constexpr uint16_t REG_RDSAR                 = 0x00E4;  // low 32 bits at E4, high at E8

// Max Transmit Packet Size (in 128-byte units)
constexpr uint16_t REG_MTPS                  = 0x00EC;  // 8-bit
constexpr uint8_t  MTPS_NORMAL               = 0x0C;    // >1518 bytes (0x0C * 128 = 1536)
constexpr uint8_t  MTPS_JUMBO                = 0x3B;    // >=7440 bytes

// Miscellaneous register (used on 8168G+ for RXDV gating)
constexpr uint16_t REG_MISC                  = 0x00F0;  // 32-bit
constexpr uint32_t MISC_RXDV_GATED           = (1u << 19);
constexpr uint32_t MISC_EARLY_TALLY_EN       = (1u << 16);
constexpr uint32_t MISC_PWM_EN               = (1u << 22);

// ============================================================================
// Chip version identification
//
// The hardware version is encoded in TxConfig[30:28,26,23,22:20].
// Linux reads (TxConfig >> 20) & 0xFCF to extract the version ID (XID).
// ============================================================================

constexpr uint32_t TCR_HWVERID_SHIFT         = 20;
constexpr uint32_t TCR_XID_MASK              = 0x00000FCF; // after >> 20

// Common RTL8168 chip versions (XID values after extraction).
// Only listing versions likely to be encountered with PCI ID 10ec:8168.
// Ref: Linux r8169.h enum mac_version + rtl_chip_infos[].
enum class chip_version : uint16_t {
    UNKNOWN               = 0x000,
    RTL8168B_1            = 0x380,  // 8168B, early
    RTL8168B_2            = 0x380,  // 8168Bp
    RTL8168C_1            = 0x3C0,  // 8168C, early
    RTL8168C_2            = 0x3C4,  // 8168C
    RTL8168C_3            = 0x3C8,  // 8168C
    RTL8168CP_1           = 0x3C8,  // 8168CP (shares XID with 8168C_3)
    RTL8168CP_2           = 0x3CC,  // 8168CP
    RTL8168D_1            = 0x281,  // 8168D
    RTL8168D_2            = 0x2C1,  // 8168D
    RTL8168DP_1           = 0x288,  // 8168DP (DASH)
    RTL8168DP_2           = 0x2C8,  // 8168DP
    RTL8168E_1            = 0x2C2,  // 8168E (VL)
    RTL8168E_2            = 0x2C6,  // 8168E-VL
    RTL8168F_1            = 0x481,  // 8168F
    RTL8168F_2            = 0x4C1,  // 8168F
    RTL8168G_1            = 0x4C0,  // 8168G
    RTL8168G_2            = 0x540,  // 8168GU/8111GU
    RTL8168H_1            = 0x541,  // 8168H
    RTL8168H_2            = 0x5C0,  // 8168H
    RTL8168FP_1           = 0x5C8,  // 8168FP (DASH)
};

// 8168G+ chips (XID >= 0x4C0) use the RXDV gate to block RX data
// during initialization. The gate must be opened after setup completes.
inline constexpr bool chip_is_8168g_plus(chip_version ver) {
    return static_cast<uint16_t>(ver) >= static_cast<uint16_t>(chip_version::RTL8168G_1);
}

// ============================================================================
// TX descriptor format (16 bytes)
//
// Each TX descriptor is 4 DWORDs:
//   DWORD 0 (opts1): OWN | EOR | FS | LS | flags | length
//   DWORD 1 (opts2): VLAN tag control + checksum offload
//   DWORD 2: buffer physical address low 32 bits
//   DWORD 3: buffer physical address high 32 bits
//
// The ring wraps at the descriptor where EOR is set.
// ============================================================================

struct tx_desc {
    uint32_t opts1;
    uint32_t opts2;
    uint32_t addr_lo;
    uint32_t addr_hi;
} __attribute__((packed));

static_assert(sizeof(tx_desc) == 16, "tx_desc must be 16 bytes");

// TX opts1 bit definitions
constexpr uint32_t TX_OWN                    = (1u << 31);  // NIC owns descriptor
constexpr uint32_t TX_EOR                    = (1u << 30);  // End Of Ring
constexpr uint32_t TX_FS                     = (1u << 29);  // First Segment
constexpr uint32_t TX_LS                     = (1u << 28);  // Last Segment
constexpr uint32_t TX_LGSEN                  = (1u << 27);  // Large Send (TSO) enable
constexpr uint32_t TX_IPCS                   = (1u << 18);  // IP Checksum Offload
constexpr uint32_t TX_UDPCS                  = (1u << 17);  // UDP Checksum Offload
constexpr uint32_t TX_TCPCS                  = (1u << 16);  // TCP Checksum Offload
constexpr uint32_t TX_LEN_MASK               = 0x0000FFFF;  // Frame length (bits [15:0])

// TX opts2 bit definitions
constexpr uint32_t TX_TAGC                   = (1u << 17);  // Insert VLAN tag
constexpr uint32_t TX_VLAN_MASK              = 0x0000FFFF;  // VLAN tag value

// ============================================================================
// RX descriptor format (16 bytes)
//
// Each RX descriptor is 4 DWORDs:
//   DWORD 0 (opts1): OWN | EOR | status flags | frame length / buffer size
//   DWORD 1 (opts2): VLAN tag info
//   DWORD 2: buffer physical address low 32 bits
//   DWORD 3: buffer physical address high 32 bits
//
// When OWN=1 (command mode): bits [13:0] = buffer size (max 0x1FF8)
// When OWN=0 (status mode):  bits [13:0] = frame length including CRC
// ============================================================================

struct rx_desc {
    uint32_t opts1;
    uint32_t opts2;
    uint32_t addr_lo;
    uint32_t addr_hi;
} __attribute__((packed));

static_assert(sizeof(rx_desc) == 16, "rx_desc must be 16 bytes");

// RX opts1 bit definitions (command mode, OWN=1)
constexpr uint32_t RX_OWN                    = (1u << 31);  // NIC owns descriptor
constexpr uint32_t RX_EOR                    = (1u << 30);  // End Of Ring
constexpr uint32_t RX_BUF_SIZE_MASK          = 0x00003FFF;  // Buffer size [13:0] max 0x1FF8

// RX opts1 bit definitions (status mode, OWN=0)
constexpr uint32_t RX_FS                     = (1u << 29);  // First Segment
constexpr uint32_t RX_LS                     = (1u << 28);  // Last Segment
constexpr uint32_t RX_MAR                    = (1u << 27);  // Multicast Address match
constexpr uint32_t RX_PAM                    = (1u << 26);  // Physical Address Match
constexpr uint32_t RX_BAR                    = (1u << 25);  // Broadcast Address
constexpr uint32_t RX_RWT                    = (1u << 22);  // Watchdog Timer expired
constexpr uint32_t RX_RES                    = (1u << 21);  // Receive Error Summary
constexpr uint32_t RX_RUNT                   = (1u << 20);  // Runt packet (<64 bytes)
constexpr uint32_t RX_CRC                    = (1u << 19);  // CRC error

// Protocol ID field [18:17]
constexpr uint32_t RX_PID_MASK               = (3u << 17);
constexpr uint32_t RX_PID_NON_IP             = (0u << 17);
constexpr uint32_t RX_PID_TCP_IP             = (1u << 17);
constexpr uint32_t RX_PID_UDP_IP             = (2u << 17);
constexpr uint32_t RX_PID_IP                 = (3u << 17);

constexpr uint32_t RX_IPF                    = (1u << 16);  // IP Checksum Failure
constexpr uint32_t RX_UDPF                   = (1u << 15);  // UDP Checksum Failure
constexpr uint32_t RX_TCPF                   = (1u << 14);  // TCP Checksum Failure
constexpr uint32_t RX_FRAME_LEN_MASK         = 0x00003FFF;  // Frame length [13:0]

// RX opts2 bit definitions
constexpr uint32_t RX_TAVA                   = (1u << 16);  // Tag Available (VLAN)
constexpr uint32_t RX_VLAN_MASK              = 0x0000FFFF;

// ============================================================================
// Descriptor ring parameters
// ============================================================================

constexpr uint32_t TX_DESC_COUNT             = 256;
constexpr uint32_t RX_DESC_COUNT             = 256;

// RX buffer size: must be multiple of 8, >= frame size. We use 2048 to
// accommodate MTU + Ethernet header + possible VLAN + CRC with room.
constexpr uint32_t RX_BUF_SIZE               = 2048;

// Descriptor rings must be 256-byte aligned
constexpr size_t   DESC_RING_ALIGN           = 256;

// ============================================================================
// Standard MII PHY registers (accessed via PHYAR)
//
// These are IEEE 802.3 standard. Same as phy_regs.h but in the rtl8168
// namespace for self-contained usage. The RTL8168 has an integrated PHY
// so there is no external MDIO bus — access is through PHYAR (offset 0x60).
// ============================================================================

namespace phy {

constexpr uint8_t  BMCR                      = 0x00;
constexpr uint16_t BMCR_RESET                = (1u << 15);
constexpr uint16_t BMCR_LOOPBACK             = (1u << 14);
constexpr uint16_t BMCR_SPEED_100            = (1u << 13);
constexpr uint16_t BMCR_ANE                  = (1u << 12);
constexpr uint16_t BMCR_POWER_DOWN           = (1u << 11);
constexpr uint16_t BMCR_RESTART_AN           = (1u << 9);
constexpr uint16_t BMCR_FULL_DUPLEX          = (1u << 8);
constexpr uint16_t BMCR_SPEED_1000           = (1u << 6);

constexpr uint8_t  BMSR                      = 0x01;
constexpr uint16_t BMSR_100BASETX_FDX        = (1u << 14);
constexpr uint16_t BMSR_100BASETX            = (1u << 13);
constexpr uint16_t BMSR_10BASET_FDX          = (1u << 12);
constexpr uint16_t BMSR_10BASET              = (1u << 11);
constexpr uint16_t BMSR_ANEG_COMPLETE        = (1u << 5);
constexpr uint16_t BMSR_LINK_STATUS          = (1u << 2);

constexpr uint8_t  PHYIDR1                   = 0x02;
constexpr uint8_t  PHYIDR2                   = 0x03;

constexpr uint8_t  ANAR                      = 0x04;
constexpr uint16_t ANAR_PAUSE                = (1u << 10);
constexpr uint16_t ANAR_100BASETX_FDX        = (1u << 8);
constexpr uint16_t ANAR_100BASETX            = (1u << 7);
constexpr uint16_t ANAR_10BASET_FDX          = (1u << 6);
constexpr uint16_t ANAR_10BASET              = (1u << 5);
constexpr uint16_t ANAR_SELECTOR_8023        = 0x0001;

constexpr uint8_t  ANLPAR                    = 0x05;

constexpr uint8_t  GBCR                      = 0x09;
constexpr uint16_t GBCR_1000BASET_FDX        = (1u << 9);
constexpr uint16_t GBCR_1000BASET            = (1u << 8);

constexpr uint8_t  GBSR                      = 0x0A;
constexpr uint16_t GBSR_1000BASET_FDX        = (1u << 11);
constexpr uint16_t GBSR_1000BASET            = (1u << 10);

} // namespace phy

} // namespace drivers::rtl8168

#endif // STELLUX_DRIVERS_NET_RTL8168_REGS_H
