#include "drivers/net/bcm_genet.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "hw/mmio.h"
#include "hw/cpu.h"
#include "irq/irq.h"
#include "common/logging.h"
#include "common/string.h"
#include "sched/sched.h"
#include "dynpriv/dynpriv.h"
#include "net/net.h"
#include "net/ipv4.h"

#if defined(__aarch64__)
#include "irq/irq_arch.h"
#endif

namespace drivers {

using namespace genet;
using namespace phy;

// ============================================================================
// Construction / factory
// ============================================================================

bcm_genet_driver::bcm_genet_driver(uint64_t reg_phys, uint64_t reg_size,
                                   uint32_t irq0, uint32_t irq1)
    : platform_driver("genet", reg_phys, reg_size, irq0, irq1)
    , m_phy_addr(0)
    , m_link_up(false)
    , m_speed(phy_speed::SPEED_NONE)
    , m_duplex(phy_duplex::HALF)
    , m_rx_buf_vaddr(0)
    , m_rx_buf_phys(0)
    , m_rx_cons_index(0)
    , m_rx_prod_index(0)
    , m_tx_buf_vaddr(0)
    , m_tx_buf_phys(0)
    , m_tx_cons_index(0)
    , m_tx_prod_index(0)
    , m_tx_queued(0)
    , m_has_irq(false) {
    m_lock = sync::SPINLOCK_INIT;
    uint8_t* p = reinterpret_cast<uint8_t*>(&m_netif);
    for (size_t i = 0; i < sizeof(m_netif); i++) p[i] = 0;
}

bcm_genet_driver* create_bcm_genet(uint64_t reg_phys, uint64_t reg_size,
                                    uint32_t irq0, uint32_t irq1) {
    return heap::ualloc_new<bcm_genet_driver>(reg_phys, reg_size, irq0, irq1);
}

// ============================================================================
// Register access
// ============================================================================

uint32_t bcm_genet_driver::reg_read(uint32_t offset) {
    return mmio::read32(m_reg_va + offset);
}

void bcm_genet_driver::reg_write(uint32_t offset, uint32_t value) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    mmio::write32(m_reg_va + offset, value);
}

// ============================================================================
// MDIO bus
// ============================================================================

// Approximate microsecond-level busy-wait delay.
static void delay_us(uint32_t us) {
    for (uint32_t i = 0; i < us; i++) {
        for (uint32_t j = 0; j < 100; j++) {
            cpu::relax();
        }
    }
}

int32_t bcm_genet_driver::mdio_read(uint8_t phy_addr, uint8_t reg, uint16_t* out) {
    uint32_t cmd = MDIO_READ | MDIO_START_BUSY |
                   (static_cast<uint32_t>(phy_addr) << MDIO_PMD_SHIFT) |
                   (static_cast<uint32_t>(reg) << MDIO_REG_SHIFT);
    reg_write(MDIO_CMD, cmd);

    for (uint32_t retry = 0; retry < 1000; retry++) {
        uint32_t val = reg_read(MDIO_CMD);
        if ((val & MDIO_START_BUSY) == 0) {
            if (val & MDIO_READ_FAIL)
                return -1;
            *out = static_cast<uint16_t>(val & MDIO_VAL_MASK);
            return 0;
        }
        delay_us(10);
    }

    log::warn("genet: MDIO read timeout (phy=%u reg=%u)", phy_addr, reg);
    return -1;
}

int32_t bcm_genet_driver::mdio_write(uint8_t phy_addr, uint8_t reg, uint16_t data) {
    uint32_t cmd = MDIO_WRITE | MDIO_START_BUSY |
                   (static_cast<uint32_t>(phy_addr) << MDIO_PMD_SHIFT) |
                   (static_cast<uint32_t>(reg) << MDIO_REG_SHIFT) |
                   data;
    reg_write(MDIO_CMD, cmd);

    for (uint32_t retry = 0; retry < 1000; retry++) {
        uint32_t val = reg_read(MDIO_CMD);
        if ((val & MDIO_START_BUSY) == 0)
            return 0;
        delay_us(10);
    }

    log::warn("genet: MDIO write timeout (phy=%u reg=%u)", phy_addr, reg);
    return -1;
}

// ============================================================================
// PHY management
// ============================================================================

int32_t bcm_genet_driver::phy_detect() {
    for (uint8_t addr = 0; addr < 32; addr++) {
        uint16_t id1 = 0, id2 = 0;
        if (mdio_read(addr, PHYIDR1, &id1) != 0) continue;
        if (mdio_read(addr, PHYIDR2, &id2) != 0) continue;

        if (id1 != 0xFFFF && id2 != 0xFFFF && id1 != 0 && id2 != 0) {
            m_phy_addr = addr;
            log::info("genet: PHY at MDIO address %u (ID 0x%04x:0x%04x)",
                      addr, id1, id2);
            return 0;
        }
    }

    log::error("genet: no PHY found on MDIO bus");
    return -1;
}

int32_t bcm_genet_driver::phy_reset() {
    int32_t rc = mdio_write(m_phy_addr, BMCR, BMCR_RESET);
    if (rc != 0) return rc;

    // Poll until reset completes (up to 500 ms)
    for (uint32_t i = 0; i < 500; i++) {
        uint16_t bmcr = 0;
        rc = mdio_read(m_phy_addr, BMCR, &bmcr);
        if (rc != 0) return rc;
        if ((bmcr & BMCR_RESET) == 0) {
            log::debug("genet: PHY reset complete (%u ms)", i);
            return phy_reset_post_action();
        }
        delay_us(1000);
    }

    log::error("genet: PHY reset timeout");
    return -1;
}

int32_t bcm_genet_driver::phy_reset_post_action() {
    // Configure RGMII clock delays via BCM54213PE shadow registers.
    // RPi4 uses RGMII-RXID: enable RX skew, disable TX delay.
    int32_t rc;
    uint16_t val;

    // Select and read AUXCTL shadow misc register
    rc = mdio_write(m_phy_addr, BRGPHY_AUXCTL,
                    BRGPHY_AUXCTL_SHADOW_MISC |
                    (BRGPHY_AUXCTL_SHADOW_MISC << BRGPHY_AUXCTL_MISC_READ_SHIFT));
    if (rc != 0) return rc;

    rc = mdio_read(m_phy_addr, BRGPHY_AUXCTL, &val);
    if (rc != 0) return rc;

    val &= BRGPHY_AUXCTL_MISC_DATA_MASK;
    val |= BRGPHY_AUXCTL_MISC_RGMII_SKEW_EN;  // enable RX clock skew

    rc = mdio_write(m_phy_addr, BRGPHY_AUXCTL,
                    BRGPHY_AUXCTL_MISC_WRITE_EN | BRGPHY_AUXCTL_SHADOW_MISC | val);
    if (rc != 0) return rc;

    // Select and read shadow 1C clock alignment control register
    rc = mdio_write(m_phy_addr, BRGPHY_SHADOW_1C, BRGPHY_SHADOW_1C_CLK_CTRL);
    if (rc != 0) return rc;

    rc = mdio_read(m_phy_addr, BRGPHY_SHADOW_1C, &val);
    if (rc != 0) return rc;

    val &= BRGPHY_SHADOW_1C_DATA_MASK;
    val &= ~BRGPHY_SHADOW_1C_GTXCLK_EN;  // no TX clock delay for RGMII-RXID

    rc = mdio_write(m_phy_addr, BRGPHY_SHADOW_1C,
                    BRGPHY_SHADOW_1C_WRITE_EN | BRGPHY_SHADOW_1C_CLK_CTRL | val);
    if (rc != 0) return rc;

    log::debug("genet: PHY clock delays configured (RGMII-RXID)");
    return 0;
}

int32_t bcm_genet_driver::phy_auto_negotiate() {
    int32_t rc;
    uint16_t val;

    // Advertise 10/100 capabilities
    rc = mdio_read(m_phy_addr, ANAR, &val);
    if (rc != 0) return rc;
    val |= ANAR_100BASETX_FDX | ANAR_100BASETX | ANAR_10BASET_FDX | ANAR_10BASET;
    rc = mdio_write(m_phy_addr, ANAR, val);
    if (rc != 0) return rc;

    // Advertise 1000 capabilities
    rc = mdio_read(m_phy_addr, GBCR, &val);
    if (rc != 0) return rc;
    val |= GBCR_1000BASET_FDX | GBCR_1000BASET;
    rc = mdio_write(m_phy_addr, GBCR, val);
    if (rc != 0) return rc;

    // Restart auto-negotiation
    rc = mdio_read(m_phy_addr, BMCR, &val);
    if (rc != 0) return rc;
    val |= BMCR_ANE | BMCR_RESTART_AN;
    rc = mdio_write(m_phy_addr, BMCR, val);
    if (rc != 0) return rc;

    log::info("genet: auto-negotiation started");
    return 0;
}

int32_t bcm_genet_driver::phy_update_link() {
    uint16_t bmsr = 0;
    int32_t rc = mdio_read(m_phy_addr, BMSR, &bmsr);
    if (rc != 0) return rc;

    bool was_up = m_link_up;
    m_link_up = (bmsr & BMSR_LINK_STATUS) && (bmsr & BMSR_ANEG_COMPLETE);

    if (m_link_up && !was_up) {
        uint16_t gbcr = 0, gbsr = 0, anlpar = 0, anar = 0;
        mdio_read(m_phy_addr, GBCR, &gbcr);
        mdio_read(m_phy_addr, GBSR, &gbsr);
        mdio_read(m_phy_addr, ANLPAR, &anlpar);
        mdio_read(m_phy_addr, ANAR, &anar);

        uint16_t gb = (gbsr >> 2) & gbcr;
        uint16_t an = anlpar & anar;

        if (gb & (GBCR_1000BASET_FDX | GBCR_1000BASET)) {
            m_speed = phy_speed::SPEED_1000;
            m_duplex = (gb & GBCR_1000BASET_FDX) ? phy_duplex::FULL : phy_duplex::HALF;
        } else if (an & (ANAR_100BASETX_FDX | ANAR_100BASETX)) {
            m_speed = phy_speed::SPEED_100;
            m_duplex = (an & ANAR_100BASETX_FDX) ? phy_duplex::FULL : phy_duplex::HALF;
        } else {
            m_speed = phy_speed::SPEED_10;
            m_duplex = (an & ANAR_10BASET_FDX) ? phy_duplex::FULL : phy_duplex::HALF;
        }

        phy_configure_mac(m_speed, m_duplex);

        log::info("genet: link up %u Mbps %s-duplex",
                  static_cast<uint32_t>(m_speed),
                  m_duplex == phy_duplex::FULL ? "full" : "half");
    } else if (!m_link_up && was_up) {
        log::info("genet: link down");
        m_speed = phy_speed::SPEED_NONE;
    }

    return 0;
}

void bcm_genet_driver::phy_configure_mac(phy_speed speed, phy_duplex duplex) {
    uint32_t oob = reg_read(EXT_RGMII_OOB_CTRL);
    oob &= ~EXT_RGMII_OOB_OOB_DIS;
    oob |= EXT_RGMII_OOB_RGMII_LINK | EXT_RGMII_OOB_RGMII_MODE;
    oob &= ~EXT_RGMII_OOB_ID_MODE_DIS;  // keep ID mode enabled for RGMII-RXID
    reg_write(EXT_RGMII_OOB_CTRL, oob);

    uint32_t cmd = reg_read(UMAC_CMD);
    cmd &= ~UMAC_CMD_SPEED_MASK;
    switch (speed) {
    case phy_speed::SPEED_1000: cmd |= UMAC_CMD_SPEED_1000; break;
    case phy_speed::SPEED_100:  cmd |= UMAC_CMD_SPEED_100;  break;
    default:                    cmd |= UMAC_CMD_SPEED_10;    break;
    }
    if (duplex == phy_duplex::FULL) cmd &= ~UMAC_CMD_HD_EN;
    else                           cmd |= UMAC_CMD_HD_EN;
    reg_write(UMAC_CMD, cmd);
}

// ============================================================================
// Controller reset and MAC configuration
// ============================================================================

void bcm_genet_driver::genet_reset() {
    // RBUF flush
    uint32_t val = reg_read(SYS_RBUF_FLUSH_CTRL);
    val |= SYS_RBUF_FLUSH_RESET;
    reg_write(SYS_RBUF_FLUSH_CTRL, val);
    delay_us(10);
    val &= ~SYS_RBUF_FLUSH_RESET;
    reg_write(SYS_RBUF_FLUSH_CTRL, val);
    delay_us(10);
    reg_write(SYS_RBUF_FLUSH_CTRL, 0);
    delay_us(10);

    // UMAC software reset
    reg_write(UMAC_CMD, 0);
    reg_write(UMAC_CMD, UMAC_CMD_LCL_LOOP_EN | UMAC_CMD_SW_RESET);
    delay_us(10);
    reg_write(UMAC_CMD, 0);

    // Clear MIB counters
    reg_write(UMAC_MIB_CTRL,
              UMAC_MIB_RESET_RUNT | UMAC_MIB_RESET_RX | UMAC_MIB_RESET_TX);
    reg_write(UMAC_MIB_CTRL, 0);

    // Max frame length and buffer configuration
    reg_write(UMAC_MAX_FRAME_LEN, MAX_PACKET_SIZE);
    val = reg_read(RBUF_CTRL);
    val |= RBUF_ALIGN_2B;  // 2-byte padding so IP headers land on 4-byte boundary
    reg_write(RBUF_CTRL, val);
    reg_write(RBUF_TBUF_SIZE_CTRL, 1);

    log::debug("genet: controller reset complete");
}

void bcm_genet_driver::read_mac_address() {
    // Firmware (UEFI) programs the MAC address into these registers at boot.
    uint32_t mac0 = reg_read(UMAC_MAC0);
    uint32_t mac1 = reg_read(UMAC_MAC1);

    m_netif.mac[0] = static_cast<uint8_t>((mac0 >> 24) & 0xFF);
    m_netif.mac[1] = static_cast<uint8_t>((mac0 >> 16) & 0xFF);
    m_netif.mac[2] = static_cast<uint8_t>((mac0 >> 8) & 0xFF);
    m_netif.mac[3] = static_cast<uint8_t>(mac0 & 0xFF);
    m_netif.mac[4] = static_cast<uint8_t>((mac1 >> 8) & 0xFF);
    m_netif.mac[5] = static_cast<uint8_t>(mac1 & 0xFF);

    bool all_zero = true, all_ff = true;
    for (int i = 0; i < 6; i++) {
        if (m_netif.mac[i] != 0x00) all_zero = false;
        if (m_netif.mac[i] != 0xFF) all_ff = false;
    }
    if (all_zero || all_ff) {
        // Locally administered fallback
        m_netif.mac[0] = 0xDC; m_netif.mac[1] = 0xA6; m_netif.mac[2] = 0x32;
        m_netif.mac[3] = 0x01; m_netif.mac[4] = 0x02; m_netif.mac[5] = 0x03;
        log::warn("genet: firmware MAC invalid, using fallback");
    }
}

void bcm_genet_driver::write_mac_address() {
    uint32_t mac0 = (static_cast<uint32_t>(m_netif.mac[0]) << 24) |
                    (static_cast<uint32_t>(m_netif.mac[1]) << 16) |
                    (static_cast<uint32_t>(m_netif.mac[2]) << 8) |
                    static_cast<uint32_t>(m_netif.mac[3]);
    uint32_t mac1 = (static_cast<uint32_t>(m_netif.mac[4]) << 8) |
                    static_cast<uint32_t>(m_netif.mac[5]);
    reg_write(UMAC_MAC0, mac0);
    reg_write(UMAC_MAC1, mac1);
}

void bcm_genet_driver::set_phy_mode() {
    reg_write(SYS_PORT_CTRL, SYS_PORT_MODE_EXT_GPHY);
}

// ============================================================================
// DMA buffer allocation and ring setup
// ============================================================================

int32_t bcm_genet_driver::dma_alloc() {
    size_t buf_total = static_cast<size_t>(DMA_DESC_COUNT) * MAX_PACKET_SIZE;
    size_t pages = (buf_total + 0xFFF) / 0x1000;
    constexpr auto flags = paging::PAGE_READ | paging::PAGE_WRITE |
                           paging::PAGE_USER | paging::PAGE_DMA;

    int32_t rc = 0;
    RUN_ELEVATED(
        rc = vmm::alloc_contiguous(pages, pmm::ZONE_DMA32, flags,
                                   vmm::ALLOC_ZERO, kva::tag::generic,
                                   m_rx_buf_vaddr, m_rx_buf_phys)
    );
    if (rc != vmm::OK) {
        log::error("genet: RX buffer allocation failed (%d)", rc);
        return -1;
    }

    RUN_ELEVATED(
        rc = vmm::alloc_contiguous(pages, pmm::ZONE_DMA32, flags,
                                   vmm::ALLOC_ZERO, kva::tag::generic,
                                   m_tx_buf_vaddr, m_tx_buf_phys)
    );
    if (rc != vmm::OK) {
        log::error("genet: TX buffer allocation failed (%d)", rc);
        return -1;
    }

    log::debug("genet: DMA buffers: RX phys=0x%lx TX phys=0x%lx (%lu pages each)",
               m_rx_buf_phys, m_tx_buf_phys, pages);
    return 0;
}

void bcm_genet_driver::dma_free() {
    // Permanent allocations; freed only if vmm supports it in the future.
}

void bcm_genet_driver::dma_init_rings() {
    constexpr uint32_t Q = DMA_DEFAULT_QUEUE;

    m_tx_queued = 0;
    m_tx_cons_index = 0;
    m_tx_prod_index = 0;
    m_rx_cons_index = 0;
    m_rx_prod_index = 0;

    // TX ring
    reg_write(TX_SCB_BURST_SIZE, BCM2711_SCB_BURST_SIZE);
    reg_write(TX_DMA_READ_PTR_LO(Q), 0);
    reg_write(TX_DMA_READ_PTR_HI(Q), 0);
    reg_write(TX_DMA_CONS_INDEX(Q), 0);
    reg_write(TX_DMA_PROD_INDEX(Q), 0);
    reg_write(TX_DMA_RING_BUF_SIZE(Q), RING_BUF_SIZE_VAL(DMA_DESC_COUNT, MAX_PACKET_SIZE));
    reg_write(TX_DMA_START_ADDR_LO(Q), 0);
    reg_write(TX_DMA_START_ADDR_HI(Q), 0);
    reg_write(TX_DMA_END_ADDR_LO(Q), DMA_END_ADDR(DMA_DESC_COUNT));
    reg_write(TX_DMA_END_ADDR_HI(Q), 0);
    reg_write(TX_DMA_MBUF_DONE_THRES(Q), 1);
    reg_write(TX_DMA_FLOW_PERIOD(Q), 0);
    reg_write(TX_DMA_WRITE_PTR_LO(Q), 0);
    reg_write(TX_DMA_WRITE_PTR_HI(Q), 0);
    reg_write(TX_DMA_RING_CFG, (1u << Q));

    // RX ring
    reg_write(RX_SCB_BURST_SIZE, BCM2711_SCB_BURST_SIZE);
    reg_write(RX_DMA_WRITE_PTR_LO(Q), 0);
    reg_write(RX_DMA_WRITE_PTR_HI(Q), 0);
    reg_write(RX_DMA_PROD_INDEX(Q), 0);
    reg_write(RX_DMA_CONS_INDEX(Q), 0);
    reg_write(RX_DMA_RING_BUF_SIZE(Q), RING_BUF_SIZE_VAL(DMA_DESC_COUNT, MAX_PACKET_SIZE));
    reg_write(RX_DMA_START_ADDR_LO(Q), 0);
    reg_write(RX_DMA_START_ADDR_HI(Q), 0);
    reg_write(RX_DMA_END_ADDR_LO(Q), DMA_END_ADDR(DMA_DESC_COUNT));
    reg_write(RX_DMA_END_ADDR_HI(Q), 0);
    reg_write(RX_DMA_XON_XOFF(Q), XON_XOFF_VAL(5, DMA_DESC_COUNT >> 4));
    reg_write(RX_DMA_READ_PTR_LO(Q), 0);
    reg_write(RX_DMA_READ_PTR_HI(Q), 0);
    reg_write(RX_DMA_RING_CFG, (1u << Q));

    log::debug("genet: DMA rings initialized (queue %u, %u descriptors)", Q, DMA_DESC_COUNT);
}

int32_t bcm_genet_driver::dma_map_rx_descriptors() {
    for (uint32_t i = 0; i < DMA_DESC_COUNT; i++) {
        uint64_t phys = m_rx_buf_phys + static_cast<uint64_t>(i) * MAX_PACKET_SIZE;
        reg_write(RX_DESC_ADDR_LO(i), static_cast<uint32_t>(phys & 0xFFFFFFFF));
        reg_write(RX_DESC_ADDR_HI(i), static_cast<uint32_t>(phys >> 32));
        reg_write(RX_DESC_STATUS(i), 0);
    }
    log::debug("genet: %u RX descriptors mapped", DMA_DESC_COUNT);
    return 0;
}

void bcm_genet_driver::dma_enable_tx_rx() {
    uint32_t val = reg_read(TX_DMA_CTRL);
    val |= DMA_CTRL_EN | DMA_CTRL_RING_EN(DMA_DEFAULT_QUEUE);
    reg_write(TX_DMA_CTRL, val);

    val = reg_read(RX_DMA_CTRL);
    val |= DMA_CTRL_EN | DMA_CTRL_RING_EN(DMA_DEFAULT_QUEUE);
    reg_write(RX_DMA_CTRL, val);

    val = reg_read(UMAC_CMD);
    val |= UMAC_CMD_TXEN | UMAC_CMD_RXEN;
    reg_write(UMAC_CMD, val);
}

void bcm_genet_driver::dma_disable_tx_rx() {
    reg_write(INTRL2_CPU_SET_MASK, 0xFFFFFFFF);
    reg_write(INTRL2_CPU_CLEAR, 0xFFFFFFFF);

    uint32_t val = reg_read(UMAC_CMD);
    val &= ~UMAC_CMD_RXEN;
    reg_write(UMAC_CMD, val);

    val = reg_read(RX_DMA_CTRL);
    val &= ~(DMA_CTRL_EN | DMA_CTRL_RING_EN(DMA_DEFAULT_QUEUE));
    reg_write(RX_DMA_CTRL, val);

    val = reg_read(TX_DMA_CTRL);
    val &= ~(DMA_CTRL_EN | DMA_CTRL_RING_EN(DMA_DEFAULT_QUEUE));
    reg_write(TX_DMA_CTRL, val);

    reg_write(UMAC_TX_FLUSH, 1);
    delay_us(10);
    reg_write(UMAC_TX_FLUSH, 0);

    val = reg_read(UMAC_CMD);
    val &= ~UMAC_CMD_TXEN;
    reg_write(UMAC_CMD, val);
}

// ============================================================================
// TX path
// ============================================================================

int32_t bcm_genet_driver::tx_callback(net::netif* iface, const uint8_t* frame, size_t len) {
    if (!iface || !frame || len == 0) return -1;
    auto* drv = static_cast<bcm_genet_driver*>(iface->driver_data);
    if (!drv) return -1;

    int32_t result = -1;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(drv->m_lock);
        drv->process_tx_completions();

        if (drv->m_tx_queued >= DMA_DESC_COUNT || len > MAX_PACKET_SIZE) {
            result = -1;
        } else {
            uint16_t idx = drv->m_tx_prod_index % DMA_DESC_COUNT;

            uintptr_t buf_va = drv->m_tx_buf_vaddr +
                               static_cast<uintptr_t>(idx) * MAX_PACKET_SIZE;
            string::memcpy(reinterpret_cast<uint8_t*>(buf_va), frame, len);

            uint64_t buf_phys = drv->m_tx_buf_phys +
                                static_cast<uint64_t>(idx) * MAX_PACKET_SIZE;

            uint32_t status = TX_DESC_SOP | TX_DESC_EOP | TX_DESC_CRC |
                              TX_DESC_QTAG_MASK |
                              (static_cast<uint32_t>(len) << TX_DESC_BUFLEN_SHIFT);

            drv->reg_write(TX_DESC_ADDR_LO(idx), static_cast<uint32_t>(buf_phys & 0xFFFFFFFF));
            drv->reg_write(TX_DESC_ADDR_HI(idx), static_cast<uint32_t>(buf_phys >> 32));
            drv->reg_write(TX_DESC_STATUS(idx), status);

            drv->m_tx_prod_index = (drv->m_tx_prod_index + 1) & DMA_INDEX_MASK;
            drv->reg_write(TX_DMA_PROD_INDEX(DMA_DEFAULT_QUEUE), drv->m_tx_prod_index);
            drv->m_tx_queued++;
            result = 0;
        }
    });
    return result;
}

void bcm_genet_driver::process_tx_completions() {
    uint32_t hw_cons = reg_read(TX_DMA_CONS_INDEX(DMA_DEFAULT_QUEUE)) & DMA_INDEX_MASK;
    uint32_t completed = (hw_cons - m_tx_cons_index) & DMA_INDEX_MASK;
    if (completed > 0) {
        if (completed > m_tx_queued) completed = m_tx_queued;
        m_tx_queued -= static_cast<uint16_t>(completed);
        m_tx_cons_index = static_cast<uint16_t>(hw_cons);
    }
}

// ============================================================================
// RX path
// ============================================================================

void bcm_genet_driver::process_rx() {
    uint32_t hw_prod = reg_read(RX_DMA_PROD_INDEX(DMA_DEFAULT_QUEUE)) & DMA_INDEX_MASK;
    uint32_t pending = (hw_prod - m_rx_cons_index) & DMA_INDEX_MASK;

    for (uint32_t i = 0; i < pending; i++) {
        uint16_t idx = m_rx_cons_index % DMA_DESC_COUNT;
        uint32_t desc_status = reg_read(RX_DESC_STATUS(idx));
        uint32_t buf_len = (desc_status & RX_DESC_BUFLEN_MASK) >> RX_DESC_BUFLEN_SHIFT;

        if (desc_status & RX_DESC_RX_ERROR) {
            log::warn("genet: RX error on desc %u (status=0x%08x)", idx, desc_status);
            goto recycle;
        }

        if ((desc_status & (RX_DESC_SOP | RX_DESC_EOP)) != (RX_DESC_SOP | RX_DESC_EOP)) {
            log::warn("genet: RX multi-descriptor frame on desc %u, dropping", idx);
            goto recycle;
        }

        if (buf_len > MAX_PACKET_SIZE || buf_len <= 2)
            goto recycle;

        {
            uintptr_t buf_va = m_rx_buf_vaddr +
                               static_cast<uintptr_t>(idx) * MAX_PACKET_SIZE;
            // Skip 2-byte RBUF alignment padding
            const uint8_t* frame = reinterpret_cast<const uint8_t*>(buf_va + 2);
            net::rx_frame(&m_netif, frame, buf_len - 2);
        }

    recycle:
        rx_remap_descriptor(idx);
        m_rx_cons_index = (m_rx_cons_index + 1) & DMA_INDEX_MASK;
        reg_write(RX_DMA_CONS_INDEX(DMA_DEFAULT_QUEUE), m_rx_cons_index);
    }
}

void bcm_genet_driver::rx_remap_descriptor(uint16_t idx) {
    uint64_t phys = m_rx_buf_phys + static_cast<uint64_t>(idx) * MAX_PACKET_SIZE;
    reg_write(RX_DESC_ADDR_LO(idx), static_cast<uint32_t>(phys & 0xFFFFFFFF));
    reg_write(RX_DESC_ADDR_HI(idx), static_cast<uint32_t>(phys >> 32));
    reg_write(RX_DESC_STATUS(idx), 0);
}

// ============================================================================
// Interrupts
// ============================================================================

int32_t bcm_genet_driver::setup_interrupts() {
    if (m_irq[0] == 0) {
        log::warn("genet: no IRQ configured, using polling");
        return -1;
    }

    int32_t rc = 0;
    RUN_ELEVATED({ rc = irq::register_handler(m_irq[0], isr, this); });
    if (rc != irq::OK) {
        log::warn("genet: failed to register IRQ %u (%d)", m_irq[0], rc);
        return -1;
    }

    RUN_ELEVATED({
#if defined(__aarch64__)
        irq::set_level_triggered(m_irq[0]);
        irq::set_spi_target(m_irq[0], 0x01);
        irq::set_group1(m_irq[0]);
#endif
        irq::unmask(m_irq[0]);
    });

    if (m_irq[1] != 0 && m_irq[1] != m_irq[0]) {
        RUN_ELEVATED({
            rc = irq::register_handler(m_irq[1], isr, this);
            if (rc == irq::OK) {
#if defined(__aarch64__)
                irq::set_level_triggered(m_irq[1]);
                irq::set_spi_target(m_irq[1], 0x01);
                irq::set_group1(m_irq[1]);
#endif
                irq::unmask(m_irq[1]);
            }
        });
    }

    m_has_irq = true;
    log::info("genet: interrupts registered (IRQ %u, %u)", m_irq[0], m_irq[1]);
    return 0;
}

void bcm_genet_driver::teardown_interrupts() {
    for (int i = 0; i < 2; i++) {
        if (m_irq[i] != 0) {
            RUN_ELEVATED({
                irq::mask(m_irq[i]);
                irq::unregister_handler(m_irq[i]);
            });
        }
    }
    m_has_irq = false;
}

void bcm_genet_driver::isr(uint32_t /* irq */, void* context) {
    auto* drv = static_cast<bcm_genet_driver*>(context);
    if (!drv) return;

    uint32_t status = drv->reg_read(INTRL2_CPU_STAT);
    if (status != 0)
        drv->reg_write(INTRL2_CPU_CLEAR, status);

    drv->notify_event();
}

void bcm_genet_driver::enable_interrupts() {
    reg_write(INTRL2_CPU_CLEAR_MASK, IRQ_TXDMA_DONE | IRQ_RXDMA_DONE);
}

void bcm_genet_driver::disable_interrupts() {
    reg_write(INTRL2_CPU_SET_MASK, 0xFFFFFFFF);
    reg_write(INTRL2_CPU_CLEAR, 0xFFFFFFFF);
}

// ============================================================================
// Net interface callbacks
// ============================================================================

bool bcm_genet_driver::link_callback(net::netif* iface) {
    if (!iface) return false;
    auto* drv = static_cast<bcm_genet_driver*>(iface->driver_data);
    return drv ? drv->m_link_up : false;
}

void bcm_genet_driver::poll_callback(net::netif* iface) {
    if (!iface) return;
    auto* drv = static_cast<bcm_genet_driver*>(iface->driver_data);
    if (!drv) return;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(drv->m_lock);
        drv->process_rx();
        drv->process_tx_completions();
    });
    RUN_ELEVATED(net::drain_deferred_tx());
}

// ============================================================================
// MAC filter
// ============================================================================

void bcm_genet_driver::set_promisc(bool enable) {
    uint32_t val = reg_read(UMAC_CMD);
    if (enable) val |= UMAC_CMD_PROMISC;
    else        val &= ~UMAC_CMD_PROMISC;
    reg_write(UMAC_CMD, val);
}

void bcm_genet_driver::setup_rx_filter() {
    // Slot 0: unicast (our MAC)
    reg_write(UMAC_MDF_ADDR0(0),
              static_cast<uint32_t>(m_netif.mac[1]) |
              (static_cast<uint32_t>(m_netif.mac[0]) << 8));
    reg_write(UMAC_MDF_ADDR1(0),
              static_cast<uint32_t>(m_netif.mac[5]) |
              (static_cast<uint32_t>(m_netif.mac[4]) << 8) |
              (static_cast<uint32_t>(m_netif.mac[3]) << 16) |
              (static_cast<uint32_t>(m_netif.mac[2]) << 24));

    // Slot 1: broadcast
    reg_write(UMAC_MDF_ADDR0(1), 0xFFFF);
    reg_write(UMAC_MDF_ADDR1(1), 0xFFFFFFFF);

    // Enable both filter slots
    reg_write(UMAC_MDF_CTRL, (1u << 16) | (1u << 15));
}

// ============================================================================
// Debug
// ============================================================================

void bcm_genet_driver::dump_state() {
    constexpr uint32_t Q = DMA_DEFAULT_QUEUE;

    log::info("genet: --- register dump ---");
    log::info("genet:  SYS_REV_CTRL  = 0x%08x", reg_read(SYS_REV_CTRL));
    log::info("genet:  UMAC_CMD      = 0x%08x", reg_read(UMAC_CMD));
    log::info("genet:  UMAC_MAC0     = 0x%08x  MAC1 = 0x%08x",
              reg_read(UMAC_MAC0), reg_read(UMAC_MAC1));
    log::info("genet:  MAX_FRAME_LEN = %u", reg_read(UMAC_MAX_FRAME_LEN));
    log::info("genet:  RBUF_CTRL     = 0x%08x", reg_read(RBUF_CTRL));
    log::info("genet:  RGMII_OOB     = 0x%08x", reg_read(EXT_RGMII_OOB_CTRL));
    log::info("genet:  INTRL2 stat=0x%08x mask=0x%08x",
              reg_read(INTRL2_CPU_STAT), reg_read(INTRL2_CPU_STAT_MASK));
    log::info("genet:  TX_DMA_CTRL=0x%08x  RX_DMA_CTRL=0x%08x",
              reg_read(TX_DMA_CTRL), reg_read(RX_DMA_CTRL));
    log::info("genet:  TX hw_prod=%u hw_cons=%u sw_prod=%u sw_cons=%u queued=%u",
              reg_read(TX_DMA_PROD_INDEX(Q)) & DMA_INDEX_MASK,
              reg_read(TX_DMA_CONS_INDEX(Q)) & DMA_INDEX_MASK,
              m_tx_prod_index, m_tx_cons_index, m_tx_queued);
    log::info("genet:  RX hw_prod=%u hw_cons=%u sw_cons=%u",
              reg_read(RX_DMA_PROD_INDEX(Q)) & DMA_INDEX_MASK,
              reg_read(RX_DMA_CONS_INDEX(Q)) & DMA_INDEX_MASK,
              m_rx_cons_index);
    log::info("genet:  RX desc[0] status=0x%08x lo=0x%08x hi=0x%08x",
              reg_read(RX_DESC_STATUS(0)),
              reg_read(RX_DESC_ADDR_LO(0)), reg_read(RX_DESC_ADDR_HI(0)));

    uint16_t bmcr = 0, bmsr = 0;
    mdio_read(m_phy_addr, BMCR, &bmcr);
    mdio_read(m_phy_addr, BMSR, &bmsr);
    log::info("genet:  PHY BMCR=0x%04x BMSR=0x%04x link=%s aneg=%s",
              bmcr, bmsr,
              (bmsr & BMSR_LINK_STATUS) ? "up" : "down",
              (bmsr & BMSR_ANEG_COMPLETE) ? "done" : "pending");
    log::info("genet: --- end dump ---");
}

// ============================================================================
// Driver lifecycle
// ============================================================================

int32_t bcm_genet_driver::attach() {
    log::info("genet: attaching (phys=0x%lx size=0x%lx IRQs=%u,%u)",
              m_reg_phys, m_reg_size, m_irq[0], m_irq[1]);

    // Map MMIO registers
    RUN_ELEVATED({ map_regs(); });
    if (m_reg_va == 0) {
        log::error("genet: failed to map MMIO registers");
        return -1;
    }
    log::info("genet: MMIO mapped at VA 0x%lx", m_reg_va);

    // Verify hardware revision
    uint32_t rev = reg_read(SYS_REV_CTRL);
    uint32_t major = (rev & SYS_REV_MAJOR_MASK) >> SYS_REV_MAJOR_SHIFT;
    uint32_t minor = (rev & SYS_REV_MINOR_MASK) >> SYS_REV_MINOR_SHIFT;
    log::info("genet: hardware rev %u.%u (SYS_REV_CTRL=0x%08x)", major, minor, rev);
    if (major != SYS_REV_MAJOR_V5) {
        log::error("genet: expected GENET v5 (major=6), got %u", major);
        return -1;
    }

    // Read MAC before reset (in case reset clears the firmware-programmed value)
    read_mac_address();
    log::info("genet: MAC %02x:%02x:%02x:%02x:%02x:%02x",
              m_netif.mac[0], m_netif.mac[1], m_netif.mac[2],
              m_netif.mac[3], m_netif.mac[4], m_netif.mac[5]);

    // Reset controller and stop any DMA left running by firmware
    genet_reset();
    dma_disable_tx_rx();

    // Restore MAC and set PHY mode
    write_mac_address();
    set_phy_mode();

    // PHY init
    int32_t rc = phy_detect();
    if (rc != 0) return rc;
    rc = phy_reset();
    if (rc != 0) return rc;
    rc = phy_auto_negotiate();
    if (rc != 0) return rc;

    // DMA setup
    rc = dma_alloc();
    if (rc != 0) return rc;
    dma_init_rings();
    rc = dma_map_rx_descriptors();
    if (rc != 0) return rc;

    // Interrupts (non-fatal if it fails; we fall back to polling)
    setup_interrupts();

    // Enable hardware
    setup_rx_filter();
    dma_enable_tx_rx();
    if (m_has_irq)
        enable_interrupts();

    // Register with network stack
    string::memcpy(m_netif.name, "eth0", 5);
    m_netif.transmit = tx_callback;
    m_netif.link_up = link_callback;
    m_netif.poll = poll_callback;
    m_netif.driver_data = this;
    net::register_netif(&m_netif);

    // Static IP (replace with DHCP when available)
    net::configure(&m_netif,
                   net::ipv4_addr(10, 0, 0, 200),
                   net::ipv4_addr(255, 255, 255, 0),
                   net::ipv4_addr(10, 0, 0, 1));

    log::info("genet: attached successfully");
    dump_state();
    return 0;
}

int32_t bcm_genet_driver::detach() {
    log::info("genet: detaching");
    dma_disable_tx_rx();
    disable_interrupts();
    teardown_interrupts();
    net::unregister_netif(&m_netif);
    dma_free();
    return 0;
}

void bcm_genet_driver::run() {
    log::info("genet: driver task running (%s)",
              m_has_irq ? "interrupt-driven" : "polling");

    // Allow time for auto-negotiation before first link check
    RUN_ELEVATED(sched::sleep_ms(2000));
    RUN_ELEVATED(phy_update_link());
    if (!m_link_up)
        log::info("genet: link not yet up, will keep checking");

    uint32_t link_poll_counter = 0;
    constexpr uint32_t LINK_POLL_INTERVAL = 100;

    while (true) {
        if (m_has_irq)
            RUN_ELEVATED(sched::sleep_ms(10));
        else
            RUN_ELEVATED(sched::sleep_ms(1));

        RUN_ELEVATED({
            sync::irq_lock_guard guard(m_lock);
            process_rx();
            process_tx_completions();
        });

        RUN_ELEVATED(net::drain_deferred_tx());

        if (++link_poll_counter >= LINK_POLL_INTERVAL) {
            link_poll_counter = 0;
            RUN_ELEVATED(phy_update_link());
        }
    }
}

} // namespace drivers
