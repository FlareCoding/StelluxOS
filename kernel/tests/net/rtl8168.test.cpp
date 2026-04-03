#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/net/rtl8168_regs.h"

TEST_SUITE(rtl8168_regs_test);

// ============================================================================
// Descriptor struct sizes
// Verify agreement with RTL8168B datasheet §6.1 (16 bytes per descriptor)
// ============================================================================

TEST(rtl8168_regs_test, descriptor_sizes) {
    using namespace drivers::rtl8168;

    EXPECT_EQ(sizeof(tx_desc), 16u);
    EXPECT_EQ(sizeof(rx_desc), 16u);
}

// ============================================================================
// Register offsets — verify against RTL8168B datasheet §2.1 MAC Registers
// ============================================================================

TEST(rtl8168_regs_test, mac_register_offsets) {
    using namespace drivers::rtl8168;

    EXPECT_EQ(REG_IDR0, static_cast<uint16_t>(0x0000));
    EXPECT_EQ(REG_IDR4, static_cast<uint16_t>(0x0004));
    EXPECT_EQ(REG_MAR0, static_cast<uint16_t>(0x0008));
    EXPECT_EQ(REG_MAR4, static_cast<uint16_t>(0x000C));
    EXPECT_EQ(REG_DTCCR, static_cast<uint16_t>(0x0010));
    EXPECT_EQ(REG_TNPDS, static_cast<uint16_t>(0x0020));
    EXPECT_EQ(REG_THPDS, static_cast<uint16_t>(0x0028));
    EXPECT_EQ(REG_CMD, static_cast<uint16_t>(0x0037));
    EXPECT_EQ(REG_TPPOLL, static_cast<uint16_t>(0x0038));
    EXPECT_EQ(REG_IMR, static_cast<uint16_t>(0x003C));
    EXPECT_EQ(REG_ISR, static_cast<uint16_t>(0x003E));
    EXPECT_EQ(REG_TCR, static_cast<uint16_t>(0x0040));
    EXPECT_EQ(REG_RCR, static_cast<uint16_t>(0x0044));
    EXPECT_EQ(REG_TCTR, static_cast<uint16_t>(0x0048));
    EXPECT_EQ(REG_9346CR, static_cast<uint16_t>(0x0050));
}

TEST(rtl8168_regs_test, config_register_offsets) {
    using namespace drivers::rtl8168;

    EXPECT_EQ(REG_CONFIG0, static_cast<uint16_t>(0x0051));
    EXPECT_EQ(REG_CONFIG1, static_cast<uint16_t>(0x0052));
    EXPECT_EQ(REG_CONFIG2, static_cast<uint16_t>(0x0053));
    EXPECT_EQ(REG_CONFIG3, static_cast<uint16_t>(0x0054));
    EXPECT_EQ(REG_CONFIG4, static_cast<uint16_t>(0x0055));
    EXPECT_EQ(REG_CONFIG5, static_cast<uint16_t>(0x0056));
}

TEST(rtl8168_regs_test, phy_and_misc_register_offsets) {
    using namespace drivers::rtl8168;

    EXPECT_EQ(REG_PHYAR, static_cast<uint16_t>(0x0060));
    EXPECT_EQ(REG_PHYSTATUS, static_cast<uint16_t>(0x006C));
    EXPECT_EQ(REG_RMS, static_cast<uint16_t>(0x00DA));
    EXPECT_EQ(REG_CPCR, static_cast<uint16_t>(0x00E0));
    EXPECT_EQ(REG_RDSAR, static_cast<uint16_t>(0x00E4));
    EXPECT_EQ(REG_MTPS, static_cast<uint16_t>(0x00EC));
    EXPECT_EQ(REG_MISC, static_cast<uint16_t>(0x00F0));
}

// ============================================================================
// Command register bits
// ============================================================================

TEST(rtl8168_regs_test, command_register_bits) {
    using namespace drivers::rtl8168;

    EXPECT_EQ(CMD_RST, static_cast<uint8_t>(0x10));
    EXPECT_EQ(CMD_RE, static_cast<uint8_t>(0x08));
    EXPECT_EQ(CMD_TE, static_cast<uint8_t>(0x04));
}

// ============================================================================
// Interrupt bit definitions
// ============================================================================

TEST(rtl8168_regs_test, interrupt_bits) {
    using namespace drivers::rtl8168;

    EXPECT_EQ(INT_ROK, static_cast<uint16_t>(1u << 0));
    EXPECT_EQ(INT_RER, static_cast<uint16_t>(1u << 1));
    EXPECT_EQ(INT_TOK, static_cast<uint16_t>(1u << 2));
    EXPECT_EQ(INT_TER, static_cast<uint16_t>(1u << 3));
    EXPECT_EQ(INT_RDU, static_cast<uint16_t>(1u << 4));
    EXPECT_EQ(INT_LINKCHG, static_cast<uint16_t>(1u << 5));
    EXPECT_EQ(INT_FOVW, static_cast<uint16_t>(1u << 6));
    EXPECT_EQ(INT_TDU, static_cast<uint16_t>(1u << 7));
    EXPECT_EQ(INT_SWINT, static_cast<uint16_t>(1u << 8));
    EXPECT_EQ(INT_TIMEOUT, static_cast<uint16_t>(1u << 14));
}

// ============================================================================
// PHYAR register construction
// ============================================================================

TEST(rtl8168_regs_test, phyar_read_command) {
    using namespace drivers::rtl8168;

    uint8_t reg = 0x01; // BMSR
    uint32_t cmd = (static_cast<uint32_t>(reg) << PHYAR_REG_SHIFT) & PHYAR_REG_MASK;

    // Read: bit 31 should be clear
    EXPECT_EQ(cmd & PHYAR_FLAG, 0u);
    EXPECT_EQ((cmd >> PHYAR_REG_SHIFT) & 0x1F, static_cast<uint32_t>(reg));
}

TEST(rtl8168_regs_test, phyar_write_command) {
    using namespace drivers::rtl8168;

    uint8_t reg = 0x00; // BMCR
    uint16_t data = 0x9200; // reset + ANE + restart_AN

    uint32_t cmd = PHYAR_FLAG |
                   ((static_cast<uint32_t>(reg) << PHYAR_REG_SHIFT) & PHYAR_REG_MASK) |
                   (data & PHYAR_DATA_MASK);

    EXPECT_BITS_SET(cmd, PHYAR_FLAG);
    EXPECT_EQ((cmd >> PHYAR_REG_SHIFT) & 0x1F, static_cast<uint32_t>(reg));
    EXPECT_EQ(cmd & PHYAR_DATA_MASK, static_cast<uint32_t>(data));
}

// ============================================================================
// PHY Status register bits
// ============================================================================

TEST(rtl8168_regs_test, phy_status_bits) {
    using namespace drivers::rtl8168;

    EXPECT_EQ(PHYSTS_LINK, static_cast<uint8_t>(1u << 1));
    EXPECT_EQ(PHYSTS_FULLDUP, static_cast<uint8_t>(1u << 0));
    EXPECT_EQ(PHYSTS_10M, static_cast<uint8_t>(1u << 2));
    EXPECT_EQ(PHYSTS_100M, static_cast<uint8_t>(1u << 3));
    EXPECT_EQ(PHYSTS_1000MF, static_cast<uint8_t>(1u << 4));
}

// ============================================================================
// TX descriptor opts1 construction
// ============================================================================

TEST(rtl8168_regs_test, tx_desc_opts1_normal) {
    using namespace drivers::rtl8168;

    uint32_t len = 1514; // max Ethernet frame
    uint32_t opts1 = TX_OWN | TX_FS | TX_LS | (len & TX_LEN_MASK);

    EXPECT_BITS_SET(opts1, TX_OWN);
    EXPECT_BITS_SET(opts1, TX_FS);
    EXPECT_BITS_SET(opts1, TX_LS);
    EXPECT_EQ(opts1 & TX_LEN_MASK, len);
    EXPECT_EQ(opts1 & TX_LGSEN, 0u); // not TSO
}

TEST(rtl8168_regs_test, tx_desc_eor_preserves_ring) {
    using namespace drivers::rtl8168;

    uint32_t opts1 = TX_OWN | TX_FS | TX_LS | TX_EOR | (100u & TX_LEN_MASK);
    EXPECT_BITS_SET(opts1, TX_EOR);
    EXPECT_BITS_SET(opts1, TX_OWN);
}

// ============================================================================
// RX descriptor opts1 — command mode (OWN=1)
// ============================================================================

TEST(rtl8168_regs_test, rx_desc_command_mode) {
    using namespace drivers::rtl8168;

    uint32_t buf_size = RX_BUF_SIZE;
    uint32_t opts1 = RX_OWN | (buf_size & RX_BUF_SIZE_MASK);

    EXPECT_BITS_SET(opts1, RX_OWN);
    EXPECT_EQ(opts1 & RX_BUF_SIZE_MASK, buf_size);

    uint32_t last = opts1 | RX_EOR;
    EXPECT_BITS_SET(last, RX_EOR);
}

// ============================================================================
// RX descriptor opts1 — status mode (OWN=0, after receive)
// ============================================================================

TEST(rtl8168_regs_test, rx_desc_status_extraction) {
    using namespace drivers::rtl8168;

    // Simulate: 100 byte frame (including CRC), first+last segment, no errors
    uint32_t status = RX_FS | RX_LS | (100u & RX_FRAME_LEN_MASK);

    EXPECT_EQ(status & RX_OWN, 0u);
    EXPECT_BITS_SET(status, RX_FS);
    EXPECT_BITS_SET(status, RX_LS);
    EXPECT_EQ(status & RX_RES, 0u);
    EXPECT_EQ(status & RX_FRAME_LEN_MASK, 100u);
}

TEST(rtl8168_regs_test, rx_desc_error_flags) {
    using namespace drivers::rtl8168;

    uint32_t status = RX_FS | RX_LS | RX_RES | RX_CRC | (64u & RX_FRAME_LEN_MASK);
    EXPECT_BITS_SET(status, RX_RES);
    EXPECT_BITS_SET(status, RX_CRC);
}

// ============================================================================
// Chip version XID extraction
// ============================================================================

TEST(rtl8168_regs_test, chip_version_xid_extraction) {
    using namespace drivers::rtl8168;

    // Simulate a TxConfig value for RTL8168B: XID = 0x380
    // bits [30:28]=011, [26]=1, [23]=1, [22:20]=000 → (0x38 << 20) ignoring bit 26/23
    // Actually: (TxConfig >> 20) & 0xFCF
    // For 0x380: the raw bits in positions [30:28,26,23,22:20] encode to 0x380 after extraction

    // We just verify the mask and shift are consistent
    uint32_t xid = 0x380;
    EXPECT_EQ(xid & TCR_XID_MASK, xid);

    // A value that exercises all bits in the mask
    uint32_t full_mask_xid = TCR_XID_MASK;
    EXPECT_EQ(full_mask_xid, 0x000007CFu);
}

// ============================================================================
// Ring parameters
// ============================================================================

TEST(rtl8168_regs_test, ring_parameters) {
    using namespace drivers::rtl8168;

    EXPECT_EQ(TX_DESC_COUNT, 256u);
    EXPECT_EQ(RX_DESC_COUNT, 256u);
    EXPECT_EQ(RX_BUF_SIZE, 2048u);

    // RX buffer size must be multiple of 8 per datasheet
    EXPECT_EQ(RX_BUF_SIZE % 8, 0u);
    // RX buffer size must fit in 14-bit field
    EXPECT_EQ(RX_BUF_SIZE <= 0x3FFF, true);
}

// ============================================================================
// Config register lock/unlock values
// ============================================================================

TEST(rtl8168_regs_test, config_lock_unlock) {
    using namespace drivers::rtl8168;

    EXPECT_EQ(CFG_9346_LOCK, static_cast<uint8_t>(0x00));
    EXPECT_EQ(CFG_9346_UNLOCK, static_cast<uint8_t>(0xC0));
}

// ============================================================================
// MII PHY register addresses
// ============================================================================

TEST(rtl8168_regs_test, phy_register_addresses) {
    using namespace drivers::rtl8168::phy;

    EXPECT_EQ(BMCR, static_cast<uint8_t>(0x00));
    EXPECT_EQ(BMSR, static_cast<uint8_t>(0x01));
    EXPECT_EQ(PHYIDR1, static_cast<uint8_t>(0x02));
    EXPECT_EQ(PHYIDR2, static_cast<uint8_t>(0x03));
    EXPECT_EQ(ANAR, static_cast<uint8_t>(0x04));
    EXPECT_EQ(ANLPAR, static_cast<uint8_t>(0x05));
    EXPECT_EQ(GBCR, static_cast<uint8_t>(0x09));
    EXPECT_EQ(GBSR, static_cast<uint8_t>(0x0A));
}

// ============================================================================
// PHY BMCR bit definitions
// ============================================================================

TEST(rtl8168_regs_test, phy_bmcr_bits) {
    using namespace drivers::rtl8168::phy;

    EXPECT_EQ(BMCR_RESET, static_cast<uint16_t>(1u << 15));
    EXPECT_EQ(BMCR_ANE, static_cast<uint16_t>(1u << 12));
    EXPECT_EQ(BMCR_RESTART_AN, static_cast<uint16_t>(1u << 9));
    EXPECT_EQ(BMCR_SPEED_1000, static_cast<uint16_t>(1u << 6));
}

// ============================================================================
// TX max packet size register values
// ============================================================================

TEST(rtl8168_regs_test, mtps_values) {
    using namespace drivers::rtl8168;

    // MTPS_NORMAL = 0x0C → 0x0C * 128 = 1536 bytes (covers 1518 frame + CRC)
    EXPECT_EQ(static_cast<uint32_t>(MTPS_NORMAL) * 128u, 1536u);
    // MTPS_JUMBO = 0x3B → 0x3B * 128 = 7552 bytes (covers 7440 jumbo)
    EXPECT_EQ(static_cast<uint32_t>(MTPS_JUMBO) * 128u, 7552u);
}
