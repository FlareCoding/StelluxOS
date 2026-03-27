#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/net/bcm_genet_regs.h"
#include "drivers/net/phy_regs.h"

TEST_SUITE(genet_regs_test);

// ============================================================================
// DMA descriptor offset tests
// Verify agreement with EDK2/FreeBSD/Linux values
// ============================================================================

TEST(genet_regs_test, rx_desc_offsets_base) {
    using namespace drivers::genet;

    // RX descriptor 0 should be at RX_BASE
    EXPECT_EQ(RX_DESC_STATUS(0), RX_BASE);
    EXPECT_EQ(RX_DESC_ADDR_LO(0), RX_BASE + 0x04u);
    EXPECT_EQ(RX_DESC_ADDR_HI(0), RX_BASE + 0x08u);
}

TEST(genet_regs_test, rx_desc_offsets_indexed) {
    using namespace drivers::genet;

    // Each descriptor is 12 bytes
    EXPECT_EQ(RX_DESC_STATUS(1), RX_BASE + 12u);
    EXPECT_EQ(RX_DESC_STATUS(255), RX_BASE + 255u * 12u);
    EXPECT_EQ(RX_DESC_ADDR_LO(10), RX_BASE + 10u * 12u + 0x04u);
}

TEST(genet_regs_test, tx_desc_offsets_base) {
    using namespace drivers::genet;

    // TX descriptor 0 should be at TX_BASE
    EXPECT_EQ(TX_DESC_STATUS(0), TX_BASE);
    EXPECT_EQ(TX_DESC_ADDR_LO(0), TX_BASE + 0x04u);
    EXPECT_EQ(TX_DESC_ADDR_HI(0), TX_BASE + 0x08u);
}

TEST(genet_regs_test, tx_desc_offsets_indexed) {
    using namespace drivers::genet;

    EXPECT_EQ(TX_DESC_STATUS(1), TX_BASE + 12u);
    EXPECT_EQ(TX_DESC_STATUS(255), TX_BASE + 255u * 12u);
}

// ============================================================================
// DMA ring register offset tests
// ============================================================================

TEST(genet_regs_test, rx_ring_base_default_queue) {
    using namespace drivers::genet;

    // Default queue (16) ring base
    uint32_t expected = RX_BASE + 0xC00 + DMA_RING_SIZE * DMA_DEFAULT_QUEUE;
    EXPECT_EQ(RX_DMA_RING_BASE(DMA_DEFAULT_QUEUE), expected);

    // Ring register offsets from ring base
    EXPECT_EQ(RX_DMA_WRITE_PTR_LO(DMA_DEFAULT_QUEUE), expected + 0x00u);
    EXPECT_EQ(RX_DMA_PROD_INDEX(DMA_DEFAULT_QUEUE), expected + 0x08u);
    EXPECT_EQ(RX_DMA_CONS_INDEX(DMA_DEFAULT_QUEUE), expected + 0x0Cu);
    EXPECT_EQ(RX_DMA_RING_BUF_SIZE(DMA_DEFAULT_QUEUE), expected + 0x10u);
    EXPECT_EQ(RX_DMA_START_ADDR_LO(DMA_DEFAULT_QUEUE), expected + 0x14u);
    EXPECT_EQ(RX_DMA_END_ADDR_LO(DMA_DEFAULT_QUEUE), expected + 0x1Cu);
}

TEST(genet_regs_test, tx_ring_base_default_queue) {
    using namespace drivers::genet;

    uint32_t expected = TX_BASE + 0xC00 + DMA_RING_SIZE * DMA_DEFAULT_QUEUE;
    EXPECT_EQ(TX_DMA_RING_BASE(DMA_DEFAULT_QUEUE), expected);

    EXPECT_EQ(TX_DMA_READ_PTR_LO(DMA_DEFAULT_QUEUE), expected + 0x00u);
    EXPECT_EQ(TX_DMA_CONS_INDEX(DMA_DEFAULT_QUEUE), expected + 0x08u);
    EXPECT_EQ(TX_DMA_PROD_INDEX(DMA_DEFAULT_QUEUE), expected + 0x0Cu);
    EXPECT_EQ(TX_DMA_RING_BUF_SIZE(DMA_DEFAULT_QUEUE), expected + 0x10u);
    EXPECT_EQ(TX_DMA_MBUF_DONE_THRES(DMA_DEFAULT_QUEUE), expected + 0x24u);
    EXPECT_EQ(TX_DMA_WRITE_PTR_LO(DMA_DEFAULT_QUEUE), expected + 0x2Cu);
}

// ============================================================================
// DMA control register offset tests
// ============================================================================

TEST(genet_regs_test, dma_ctrl_offsets) {
    using namespace drivers::genet;

    // Global DMA control registers
    EXPECT_EQ(RX_DMA_RING_CFG, RX_BASE + 0x1040u);
    EXPECT_EQ(RX_DMA_CTRL, RX_BASE + 0x1044u);
    EXPECT_EQ(RX_SCB_BURST_SIZE, RX_BASE + 0x104Cu);

    EXPECT_EQ(TX_DMA_RING_CFG, TX_BASE + 0x1040u);
    EXPECT_EQ(TX_DMA_CTRL, TX_BASE + 0x1044u);
    EXPECT_EQ(TX_SCB_BURST_SIZE, TX_BASE + 0x104Cu);
}

TEST(genet_regs_test, dma_ctrl_ring_enable_bits) {
    using namespace drivers::genet;

    // Queue 0 enable bit should be bit 1
    EXPECT_EQ(DMA_CTRL_RING_EN(0), (1u << 1));
    // Queue 16 (default) enable bit should be bit 17
    EXPECT_EQ(DMA_CTRL_RING_EN(16), (1u << 17));
}

// ============================================================================
// Ring buffer size field encoding
// ============================================================================

TEST(genet_regs_test, ring_buf_size_encoding) {
    using namespace drivers::genet;

    uint32_t val = RING_BUF_SIZE_VAL(256, 1536);
    // desc_count=256 in upper 16 bits, buf_len=1536 in lower 16 bits
    EXPECT_EQ(val >> 16, 256u);
    EXPECT_EQ(val & 0xFFFFu, 1536u);

    // 256 * 12 / 4 - 1 = 767
    EXPECT_EQ(DMA_END_ADDR(256), 767u);
}

// ============================================================================
// XON/XOFF threshold encoding
// ============================================================================

TEST(genet_regs_test, xon_xoff_encoding) {
    using namespace drivers::genet;

    uint32_t val = XON_XOFF_VAL(5, DMA_DESC_COUNT >> 4);
    EXPECT_EQ(val >> 16, 5u);
    EXPECT_EQ(val & 0xFFFFu, 16u); // 256 >> 4 = 16
}

// ============================================================================
// Producer/consumer index math
// ============================================================================

TEST(genet_regs_test, index_wrapping) {
    using namespace drivers::genet;

    // Normal case
    uint16_t prod = 10;
    uint16_t cons = 5;
    EXPECT_EQ((prod - cons) & DMA_INDEX_MASK, 5u);

    // Wrap-around case
    prod = 3;
    cons = 65530;
    uint32_t pending = (prod - cons) & DMA_INDEX_MASK;
    EXPECT_EQ(pending, 9u); // 65536 - 65530 + 3 = 9

    // No pending
    prod = 100;
    cons = 100;
    EXPECT_EQ((prod - cons) & DMA_INDEX_MASK, 0u);
}

TEST(genet_regs_test, desc_index_from_cons) {
    using namespace drivers::genet;

    // Consumer index maps to descriptor index via modulo
    EXPECT_EQ(static_cast<uint16_t>(0) % DMA_DESC_COUNT, 0u);
    EXPECT_EQ(static_cast<uint16_t>(255) % DMA_DESC_COUNT, 255u);
    EXPECT_EQ(static_cast<uint16_t>(256) % DMA_DESC_COUNT, 0u);
    EXPECT_EQ(static_cast<uint16_t>(257) % DMA_DESC_COUNT, 1u);
    EXPECT_EQ(static_cast<uint16_t>(512) % DMA_DESC_COUNT, 0u);
}

// ============================================================================
// MDIO command word construction
// ============================================================================

TEST(genet_regs_test, mdio_read_command) {
    using namespace drivers::genet;

    uint8_t phy_addr = 1;
    uint8_t reg = 0x02; // PHYIDR1

    uint32_t cmd = MDIO_READ | MDIO_START_BUSY |
                   (static_cast<uint32_t>(phy_addr) << MDIO_PMD_SHIFT) |
                   (static_cast<uint32_t>(reg) << MDIO_REG_SHIFT);

    // Verify individual fields
    EXPECT_BITS_SET(cmd, MDIO_READ);
    EXPECT_BITS_SET(cmd, MDIO_START_BUSY);
    EXPECT_EQ((cmd >> MDIO_PMD_SHIFT) & 0x1F, static_cast<uint32_t>(phy_addr));
    EXPECT_EQ((cmd >> MDIO_REG_SHIFT) & 0x1F, static_cast<uint32_t>(reg));
}

TEST(genet_regs_test, mdio_write_command) {
    using namespace drivers::genet;

    uint8_t phy_addr = 1;
    uint8_t reg = 0x00; // BMCR
    uint16_t data = 0x1234;

    uint32_t cmd = MDIO_WRITE | MDIO_START_BUSY |
                   (static_cast<uint32_t>(phy_addr) << MDIO_PMD_SHIFT) |
                   (static_cast<uint32_t>(reg) << MDIO_REG_SHIFT) |
                   data;

    EXPECT_BITS_SET(cmd, MDIO_WRITE);
    EXPECT_BITS_SET(cmd, MDIO_START_BUSY);
    EXPECT_EQ(cmd & MDIO_VAL_MASK, static_cast<uint32_t>(data));
}

// ============================================================================
// TX descriptor status construction
// ============================================================================

TEST(genet_regs_test, tx_desc_status_construction) {
    using namespace drivers::genet;

    uint32_t len = 1500;
    uint32_t status = TX_DESC_SOP | TX_DESC_EOP | TX_DESC_CRC |
                      TX_DESC_QTAG_MASK |
                      (len << TX_DESC_BUFLEN_SHIFT);

    EXPECT_BITS_SET(status, TX_DESC_SOP);
    EXPECT_BITS_SET(status, TX_DESC_EOP);
    EXPECT_BITS_SET(status, TX_DESC_CRC);
    EXPECT_EQ((status & TX_DESC_BUFLEN_MASK) >> TX_DESC_BUFLEN_SHIFT, len);
}

// ============================================================================
// RX descriptor status extraction
// ============================================================================

TEST(genet_regs_test, rx_desc_status_extraction) {
    using namespace drivers::genet;

    // Simulate a received frame of 100 bytes with SOP+EOP set
    uint32_t status = (100u << RX_DESC_BUFLEN_SHIFT) | RX_DESC_SOP | RX_DESC_EOP;

    uint32_t buf_len = (status & RX_DESC_BUFLEN_MASK) >> RX_DESC_BUFLEN_SHIFT;
    EXPECT_EQ(buf_len, 100u);
    EXPECT_BITS_SET(status, RX_DESC_SOP);
    EXPECT_BITS_SET(status, RX_DESC_EOP);
    EXPECT_EQ(status & RX_DESC_RX_ERROR, 0u);
}

// ============================================================================
// Key register offsets — verify against known EDK2/FreeBSD values
// ============================================================================

TEST(genet_regs_test, sys_register_offsets) {
    using namespace drivers::genet;

    EXPECT_EQ(SYS_REV_CTRL, 0x000u);
    EXPECT_EQ(SYS_PORT_CTRL, 0x004u);
    EXPECT_EQ(SYS_RBUF_FLUSH_CTRL, 0x008u);
    EXPECT_EQ(SYS_TBUF_FLUSH_CTRL, 0x00Cu);
}

TEST(genet_regs_test, umac_register_offsets) {
    using namespace drivers::genet;

    EXPECT_EQ(UMAC_CMD, 0x808u);
    EXPECT_EQ(UMAC_MAC0, 0x80Cu);
    EXPECT_EQ(UMAC_MAC1, 0x810u);
    EXPECT_EQ(UMAC_MAX_FRAME_LEN, 0x814u);
    EXPECT_EQ(UMAC_TX_FLUSH, 0xB34u);
    EXPECT_EQ(UMAC_MIB_CTRL, 0xD80u);
    EXPECT_EQ(MDIO_CMD, 0xE14u);
    EXPECT_EQ(UMAC_MDF_CTRL, 0xE50u);
}

TEST(genet_regs_test, interrupt_register_offsets) {
    using namespace drivers::genet;

    EXPECT_EQ(INTRL2_CPU_STAT, 0x200u);
    EXPECT_EQ(INTRL2_CPU_CLEAR, 0x208u);
    EXPECT_EQ(INTRL2_CPU_STAT_MASK, 0x20Cu);
    EXPECT_EQ(INTRL2_CPU_SET_MASK, 0x210u);
    EXPECT_EQ(INTRL2_CPU_CLEAR_MASK, 0x214u);
}

TEST(genet_regs_test, ext_and_rbuf_offsets) {
    using namespace drivers::genet;

    EXPECT_EQ(EXT_RGMII_OOB_CTRL, 0x08Cu);
    EXPECT_EQ(RBUF_CTRL, 0x300u);
    EXPECT_EQ(RBUF_TBUF_SIZE_CTRL, 0x3B4u);
}

// ============================================================================
// PHY register sanity
// ============================================================================

TEST(genet_regs_test, phy_standard_registers) {
    using namespace drivers::phy;

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
// MDF register accessor
// ============================================================================

TEST(genet_regs_test, mdf_addr_accessors) {
    using namespace drivers::genet;

    EXPECT_EQ(UMAC_MDF_ADDR0(0), 0xE54u);
    EXPECT_EQ(UMAC_MDF_ADDR1(0), 0xE58u);
    EXPECT_EQ(UMAC_MDF_ADDR0(1), 0xE54u + 0x08u);
    EXPECT_EQ(UMAC_MDF_ADDR1(1), 0xE58u + 0x08u);
}
