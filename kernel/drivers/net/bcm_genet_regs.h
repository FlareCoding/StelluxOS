#ifndef STELLUX_DRIVERS_NET_BCM_GENET_REGS_H
#define STELLUX_DRIVERS_NET_BCM_GENET_REGS_H

/**
 * BCM GENET v5 (Gigabit Ethernet Network Engine Technology) register definitions.
 *
 * Used in BCM2711 (Raspberry Pi 4). These registers are MMIO-mapped starting
 * at the controller base address (0xfd580000 on RPi4, 64KB region).
 *
 * Cross-referenced against:
 *   - EDK2 BcmGenetDxe.h
 *   - FreeBSD if_genetreg.h
 *   - Linux bcmgenet.h
 *
 * All three sources agree on the offsets below.
 */

#include "common/types.h"

namespace drivers::genet {

// ============================================================================
// System registers
// ============================================================================

constexpr uint32_t SYS_REV_CTRL              = 0x000;
constexpr uint32_t  SYS_REV_MAJOR_MASK       = 0x0F000000;
constexpr uint32_t  SYS_REV_MAJOR_SHIFT      = 24;
constexpr uint32_t  SYS_REV_MAJOR_V5         = 6;  // GENET v5 reports major=6
constexpr uint32_t  SYS_REV_MINOR_MASK       = 0x000F0000;
constexpr uint32_t  SYS_REV_MINOR_SHIFT      = 16;

constexpr uint32_t SYS_PORT_CTRL             = 0x004;
constexpr uint32_t  SYS_PORT_MODE_EXT_GPHY   = 3;

constexpr uint32_t SYS_RBUF_FLUSH_CTRL       = 0x008;
constexpr uint32_t  SYS_RBUF_FLUSH_RESET     = (1u << 1);

constexpr uint32_t SYS_TBUF_FLUSH_CTRL       = 0x00C;

// ============================================================================
// External RGMII control
// ============================================================================

constexpr uint32_t EXT_RGMII_OOB_CTRL        = 0x08C;
constexpr uint32_t  EXT_RGMII_OOB_ID_MODE_DIS = (1u << 16);
constexpr uint32_t  EXT_RGMII_OOB_RGMII_MODE  = (1u << 6);
constexpr uint32_t  EXT_RGMII_OOB_OOB_DIS     = (1u << 5);
constexpr uint32_t  EXT_RGMII_OOB_RGMII_LINK  = (1u << 4);

// ============================================================================
// Interrupt registers (INTRL2 bank 0 — CPU interrupts)
// ============================================================================

constexpr uint32_t INTRL2_CPU_STAT           = 0x200;
constexpr uint32_t INTRL2_CPU_CLEAR          = 0x208;
constexpr uint32_t INTRL2_CPU_STAT_MASK      = 0x20C;
constexpr uint32_t INTRL2_CPU_SET_MASK       = 0x210;
constexpr uint32_t INTRL2_CPU_CLEAR_MASK     = 0x214;

// Interrupt bit definitions
constexpr uint32_t IRQ_MDIO_ERROR            = (1u << 24);
constexpr uint32_t IRQ_MDIO_DONE             = (1u << 23);
constexpr uint32_t IRQ_TXDMA_DONE            = (1u << 16);
constexpr uint32_t IRQ_RXDMA_DONE            = (1u << 13);

// ============================================================================
// Receive buffer control (RBUF)
// ============================================================================

constexpr uint32_t RBUF_CTRL                 = 0x300;
constexpr uint32_t  RBUF_64B_EN              = (1u << 0);
constexpr uint32_t  RBUF_ALIGN_2B            = (1u << 1);
constexpr uint32_t  RBUF_BAD_DIS             = (1u << 2);

constexpr uint32_t RBUF_CHECK_CTRL           = 0x314;
constexpr uint32_t  RBUF_CHECK_CTRL_EN       = (1u << 0);
constexpr uint32_t  RBUF_CHECK_SKIP_FCS      = (1u << 4);

constexpr uint32_t RBUF_TBUF_SIZE_CTRL       = 0x3B4;

// ============================================================================
// Transmit buffer control (TBUF)
// ============================================================================

constexpr uint32_t TBUF_CTRL                 = 0x600;

// ============================================================================
// UniMAC (UMAC) registers
// ============================================================================

constexpr uint32_t UMAC_CMD                  = 0x808;
constexpr uint32_t  UMAC_CMD_LCL_LOOP_EN     = (1u << 15);
constexpr uint32_t  UMAC_CMD_SW_RESET        = (1u << 13);
constexpr uint32_t  UMAC_CMD_HD_EN           = (1u << 10);
constexpr uint32_t  UMAC_CMD_CRC_FWD         = (1u << 6);
constexpr uint32_t  UMAC_CMD_PROMISC         = (1u << 4);
constexpr uint32_t  UMAC_CMD_SPEED_MASK      = (3u << 2);
constexpr uint32_t  UMAC_CMD_SPEED_10        = (0u << 2);
constexpr uint32_t  UMAC_CMD_SPEED_100       = (1u << 2);
constexpr uint32_t  UMAC_CMD_SPEED_1000      = (2u << 2);
constexpr uint32_t  UMAC_CMD_RXEN            = (1u << 1);
constexpr uint32_t  UMAC_CMD_TXEN            = (1u << 0);

constexpr uint32_t UMAC_MAC0                 = 0x80C;  // MAC address bytes [0:3]
constexpr uint32_t UMAC_MAC1                 = 0x810;  // MAC address bytes [4:5]
constexpr uint32_t UMAC_MAX_FRAME_LEN        = 0x814;

constexpr uint32_t UMAC_TX_FLUSH             = 0xB34;

constexpr uint32_t UMAC_MIB_CTRL             = 0xD80;
constexpr uint32_t  UMAC_MIB_RESET_RX        = (1u << 0);
constexpr uint32_t  UMAC_MIB_RESET_RUNT      = (1u << 1);
constexpr uint32_t  UMAC_MIB_RESET_TX        = (1u << 2);

// ============================================================================
// MDIO bus registers
// ============================================================================

constexpr uint32_t MDIO_CMD                  = 0xE14;
constexpr uint32_t  MDIO_START_BUSY          = (1u << 29);
constexpr uint32_t  MDIO_READ_FAIL           = (1u << 28);
constexpr uint32_t  MDIO_READ                = (1u << 27);
constexpr uint32_t  MDIO_WRITE               = (1u << 26);
constexpr uint32_t  MDIO_PMD_SHIFT           = 21;
constexpr uint32_t  MDIO_PMD_MASK            = (0x1Fu << 21);
constexpr uint32_t  MDIO_REG_SHIFT           = 16;
constexpr uint32_t  MDIO_REG_MASK            = (0x1Fu << 16);
constexpr uint32_t  MDIO_VAL_MASK            = 0xFFFF;

// ============================================================================
// MAC Destination Filter (MDF) registers
// ============================================================================

constexpr uint32_t UMAC_MDF_CTRL             = 0xE50;
constexpr uint32_t MAX_MDF_FILTER            = 17;

inline constexpr uint32_t UMAC_MDF_ADDR0(uint32_t n) {
    return 0xE54 + n * 0x8;
}

inline constexpr uint32_t UMAC_MDF_ADDR1(uint32_t n) {
    return 0xE58 + n * 0x8;
}

// ============================================================================
// DMA descriptor layout
//
// Descriptors are MMIO-mapped at RX_BASE/TX_BASE. Each descriptor is 12 bytes:
//   [0x00] STATUS   — length, flags
//   [0x04] ADDR_LO  — low 32 bits of buffer physical address
//   [0x08] ADDR_HI  — high 32 bits of buffer physical address
// ============================================================================

constexpr uint32_t DMA_DESC_COUNT            = 256;
constexpr uint32_t DMA_DESC_SIZE             = 12;  // bytes per descriptor
constexpr uint32_t DMA_DEFAULT_QUEUE         = 16;  // default queue index
constexpr uint32_t DMA_RING_SIZE             = 0x40; // bytes per ring control block
constexpr uint32_t MAX_PACKET_SIZE           = 1536;

// Burst size for BCM2711 (RPi4). Other GENET versions may differ.
constexpr uint32_t BCM2711_SCB_BURST_SIZE    = 0x08;

// DMA base addresses (offsets from controller base)
constexpr uint32_t RX_BASE                   = 0x2000;
constexpr uint32_t TX_BASE                   = 0x4000;

// ============================================================================
// RX DMA descriptor accessors
// ============================================================================

inline constexpr uint32_t RX_DESC_STATUS(uint32_t idx) {
    return RX_BASE + DMA_DESC_SIZE * idx + 0x00;
}

inline constexpr uint32_t RX_DESC_ADDR_LO(uint32_t idx) {
    return RX_BASE + DMA_DESC_SIZE * idx + 0x04;
}

inline constexpr uint32_t RX_DESC_ADDR_HI(uint32_t idx) {
    return RX_BASE + DMA_DESC_SIZE * idx + 0x08;
}

// RX descriptor status bits
constexpr uint32_t RX_DESC_BUFLEN_MASK       = 0x0FFF0000;
constexpr uint32_t RX_DESC_BUFLEN_SHIFT      = 16;
constexpr uint32_t RX_DESC_OWN               = (1u << 15);
constexpr uint32_t RX_DESC_EOP               = (1u << 14);
constexpr uint32_t RX_DESC_SOP               = (1u << 13);
constexpr uint32_t RX_DESC_RX_ERROR          = (1u << 2);

// ============================================================================
// TX DMA descriptor accessors
// ============================================================================

inline constexpr uint32_t TX_DESC_STATUS(uint32_t idx) {
    return TX_BASE + DMA_DESC_SIZE * idx + 0x00;
}

inline constexpr uint32_t TX_DESC_ADDR_LO(uint32_t idx) {
    return TX_BASE + DMA_DESC_SIZE * idx + 0x04;
}

inline constexpr uint32_t TX_DESC_ADDR_HI(uint32_t idx) {
    return TX_BASE + DMA_DESC_SIZE * idx + 0x08;
}

// TX descriptor status bits
constexpr uint32_t TX_DESC_BUFLEN_MASK       = 0x0FFF0000;
constexpr uint32_t TX_DESC_BUFLEN_SHIFT      = 16;
constexpr uint32_t TX_DESC_OWN               = (1u << 15);
constexpr uint32_t TX_DESC_EOP               = (1u << 14);
constexpr uint32_t TX_DESC_SOP               = (1u << 13);
constexpr uint32_t TX_DESC_QTAG_MASK         = 0x1F80;  // bits [12:7]
constexpr uint32_t TX_DESC_CRC               = (1u << 6);
constexpr uint32_t TX_DESC_CKSUM             = (1u << 4);

// ============================================================================
// DMA ring registers — per-queue
//
// Each queue has a 0x40-byte control block. We only use the default queue (16).
// ============================================================================

// Ring base address calculation
inline constexpr uint32_t RX_DMA_RING_BASE(uint32_t qid) {
    return RX_BASE + 0xC00 + DMA_RING_SIZE * qid;
}

inline constexpr uint32_t TX_DMA_RING_BASE(uint32_t qid) {
    return TX_BASE + 0xC00 + DMA_RING_SIZE * qid;
}

// RX ring registers (offsets from RX_DMA_RING_BASE)
inline constexpr uint32_t RX_DMA_WRITE_PTR_LO(uint32_t qid) { return RX_DMA_RING_BASE(qid) + 0x00; }
inline constexpr uint32_t RX_DMA_WRITE_PTR_HI(uint32_t qid) { return RX_DMA_RING_BASE(qid) + 0x04; }
inline constexpr uint32_t RX_DMA_PROD_INDEX(uint32_t qid)    { return RX_DMA_RING_BASE(qid) + 0x08; }
inline constexpr uint32_t RX_DMA_CONS_INDEX(uint32_t qid)    { return RX_DMA_RING_BASE(qid) + 0x0C; }
inline constexpr uint32_t RX_DMA_RING_BUF_SIZE(uint32_t qid) { return RX_DMA_RING_BASE(qid) + 0x10; }
inline constexpr uint32_t RX_DMA_START_ADDR_LO(uint32_t qid) { return RX_DMA_RING_BASE(qid) + 0x14; }
inline constexpr uint32_t RX_DMA_START_ADDR_HI(uint32_t qid) { return RX_DMA_RING_BASE(qid) + 0x18; }
inline constexpr uint32_t RX_DMA_END_ADDR_LO(uint32_t qid)   { return RX_DMA_RING_BASE(qid) + 0x1C; }
inline constexpr uint32_t RX_DMA_END_ADDR_HI(uint32_t qid)   { return RX_DMA_RING_BASE(qid) + 0x20; }
inline constexpr uint32_t RX_DMA_XON_XOFF(uint32_t qid)      { return RX_DMA_RING_BASE(qid) + 0x28; }
inline constexpr uint32_t RX_DMA_READ_PTR_LO(uint32_t qid)   { return RX_DMA_RING_BASE(qid) + 0x2C; }
inline constexpr uint32_t RX_DMA_READ_PTR_HI(uint32_t qid)   { return RX_DMA_RING_BASE(qid) + 0x30; }

// TX ring registers (offsets from TX_DMA_RING_BASE)
inline constexpr uint32_t TX_DMA_READ_PTR_LO(uint32_t qid)   { return TX_DMA_RING_BASE(qid) + 0x00; }
inline constexpr uint32_t TX_DMA_READ_PTR_HI(uint32_t qid)   { return TX_DMA_RING_BASE(qid) + 0x04; }
inline constexpr uint32_t TX_DMA_CONS_INDEX(uint32_t qid)     { return TX_DMA_RING_BASE(qid) + 0x08; }
inline constexpr uint32_t TX_DMA_PROD_INDEX(uint32_t qid)     { return TX_DMA_RING_BASE(qid) + 0x0C; }
inline constexpr uint32_t TX_DMA_RING_BUF_SIZE(uint32_t qid)  { return TX_DMA_RING_BASE(qid) + 0x10; }
inline constexpr uint32_t TX_DMA_START_ADDR_LO(uint32_t qid)  { return TX_DMA_RING_BASE(qid) + 0x14; }
inline constexpr uint32_t TX_DMA_START_ADDR_HI(uint32_t qid)  { return TX_DMA_RING_BASE(qid) + 0x18; }
inline constexpr uint32_t TX_DMA_END_ADDR_LO(uint32_t qid)    { return TX_DMA_RING_BASE(qid) + 0x1C; }
inline constexpr uint32_t TX_DMA_END_ADDR_HI(uint32_t qid)    { return TX_DMA_RING_BASE(qid) + 0x20; }
inline constexpr uint32_t TX_DMA_MBUF_DONE_THRES(uint32_t qid){ return TX_DMA_RING_BASE(qid) + 0x24; }
inline constexpr uint32_t TX_DMA_FLOW_PERIOD(uint32_t qid)    { return TX_DMA_RING_BASE(qid) + 0x28; }
inline constexpr uint32_t TX_DMA_WRITE_PTR_LO(uint32_t qid)   { return TX_DMA_RING_BASE(qid) + 0x2C; }
inline constexpr uint32_t TX_DMA_WRITE_PTR_HI(uint32_t qid)   { return TX_DMA_RING_BASE(qid) + 0x30; }

// Producer/consumer index mask (16-bit wrapping counters)
constexpr uint32_t DMA_INDEX_MASK            = 0xFFFF;

// Ring buffer size field encoding
inline constexpr uint32_t RING_BUF_SIZE_VAL(uint32_t desc_count, uint32_t buf_len) {
    return (desc_count << 16) | (buf_len & 0xFFFF);
}

// XON/XOFF threshold encoding
inline constexpr uint32_t XON_XOFF_VAL(uint32_t xon, uint32_t xoff) {
    return (xon << 16) | (xoff & 0xFFFF);
}

// End address for descriptor ring (in 32-bit words, minus 1)
inline constexpr uint32_t DMA_END_ADDR(uint32_t desc_count) {
    return (desc_count * DMA_DESC_SIZE / 4) - 1;
}

// ============================================================================
// Global DMA control registers (outside per-queue ring blocks)
// ============================================================================

constexpr uint32_t RX_DMA_RING_CFG           = RX_BASE + 0x1040;
constexpr uint32_t RX_DMA_CTRL               = RX_BASE + 0x1044;
constexpr uint32_t RX_SCB_BURST_SIZE         = RX_BASE + 0x104C;

constexpr uint32_t TX_DMA_RING_CFG           = TX_BASE + 0x1040;
constexpr uint32_t TX_DMA_CTRL               = TX_BASE + 0x1044;
constexpr uint32_t TX_SCB_BURST_SIZE         = TX_BASE + 0x104C;

// DMA control bits
constexpr uint32_t DMA_CTRL_EN               = (1u << 0);
inline constexpr uint32_t DMA_CTRL_RING_EN(uint32_t qid) {
    return (1u << (qid + 1));
}

} // namespace drivers::genet

#endif // STELLUX_DRIVERS_NET_BCM_GENET_REGS_H
