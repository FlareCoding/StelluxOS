#include "drivers/net/rtl8168.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "hw/mmio.h"
#include "hw/cpu.h"
#include "hw/delay.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"
#include "net/net.h"
#include "net/dhcp.h"

namespace drivers {

using namespace rtl8168;

rtl8168_driver::rtl8168_driver(pci::device* dev)
    : pci_driver("rtl8168", dev)
    , m_mmio_va(0)
    , m_chip_version(chip_version::UNKNOWN)
    , m_link_up(false)
    , m_speed(0)
    , m_full_duplex(false)
    , m_tx_ring(nullptr)
    , m_tx_ring_phys(0)
    , m_tx_prod(0)
    , m_tx_cons(0)
    , m_tx_queued(0)
    , m_tx_buf_vaddr(0)
    , m_tx_buf_phys(0)
    , m_rx_ring(nullptr)
    , m_rx_ring_phys(0)
    , m_rx_cur(0)
    , m_rx_buf_vaddr(0)
    , m_rx_buf_phys(0)
    , m_has_msi(false)
    , m_imr(INT_MASK_DEFAULT) {
    m_lock = sync::SPINLOCK_INIT;
    string::memset(&m_netif, 0, sizeof(m_netif));
}

uint8_t rtl8168_driver::reg_read8(uint16_t offset) {
    return mmio::read8(m_mmio_va + offset);
}

uint16_t rtl8168_driver::reg_read16(uint16_t offset) {
    return mmio::read16(m_mmio_va + offset);
}

uint32_t rtl8168_driver::reg_read32(uint16_t offset) {
    return mmio::read32(m_mmio_va + offset);
}

void rtl8168_driver::reg_write8(uint16_t offset, uint8_t value) {
    mmio::write8(m_mmio_va + offset, value);
}

void rtl8168_driver::reg_write16(uint16_t offset, uint16_t value) {
    mmio::write16(m_mmio_va + offset, value);
}

void rtl8168_driver::reg_write32(uint16_t offset, uint32_t value) {
    mmio::write32(m_mmio_va + offset, value);
}

chip_version rtl8168_driver::identify_chip() {
    uint32_t tcr = reg_read32(REG_TCR);
    uint32_t xid = (tcr >> TCR_HWVERID_SHIFT) & TCR_XID_MASK;

    log::info("rtl8168: TxConfig=0x%08x XID=0x%03x", tcr, xid);

    switch (xid) {
    case 0x380: return chip_version::RTL8168B_1;
    case 0x3C0: return chip_version::RTL8168C_1;
    case 0x3C4: return chip_version::RTL8168C_2;
    case 0x3C8: return chip_version::RTL8168C_3;
    case 0x3CC: return chip_version::RTL8168CP_2;
    case 0x281: return chip_version::RTL8168D_1;
    case 0x2C1: return chip_version::RTL8168D_2;
    case 0x288: return chip_version::RTL8168DP_1;
    case 0x2C8: return chip_version::RTL8168DP_2;
    case 0x2C2: return chip_version::RTL8168E_1;
    case 0x2C6: return chip_version::RTL8168E_2;
    case 0x481: return chip_version::RTL8168F_1;
    case 0x4C1: return chip_version::RTL8168F_2;
    case 0x4C0: return chip_version::RTL8168G_1;
    case 0x540: return chip_version::RTL8168G_2;
    case 0x541: return chip_version::RTL8168H_1;
    case 0x5C0: return chip_version::RTL8168H_2;
    case 0x5C8: return chip_version::RTL8168FP_1;
    default:
        log::warn("rtl8168: unknown chip XID 0x%03x, using generic path", xid);
        return chip_version::UNKNOWN;
    }
}

// Per datasheet section 2.9: EEM=11 (0xC0) unlocks CONFIG0-5 writes.
void rtl8168_driver::config_unlock() {
    reg_write8(REG_9346CR, CFG_9346_UNLOCK);
}

void rtl8168_driver::config_lock() {
    reg_write8(REG_9346CR, CFG_9346_LOCK);
}

/**
 * Per datasheet section 2.3: set CMD.RST, poll until self-cleared.
 * Preserves MAC address, MAR, and PCI config space.
 */
void rtl8168_driver::hw_reset() {
    reg_write8(REG_CMD, CMD_RST);

    for (uint32_t i = 0; i < 1000; i++) {
        if ((reg_read8(REG_CMD) & CMD_RST) == 0) {
            log::debug("rtl8168: hardware reset complete (%u iterations)", i);
            return;
        }
        for (uint32_t j = 0; j < 100; j++) cpu::relax();
    }

    log::warn("rtl8168: hardware reset timeout (CMD still has RST set)");
}

/**
 * Read MAC from IDR0-IDR5 (EEPROM autoloaded at power-on).
 * Falls back to a fixed address if EEPROM is blank.
 */
void rtl8168_driver::read_mac_address() {
    uint32_t idr0 = reg_read32(REG_IDR0);
    uint32_t idr4 = reg_read32(REG_IDR4);

    m_netif.mac[0] = static_cast<uint8_t>(idr0 & 0xFF);
    m_netif.mac[1] = static_cast<uint8_t>((idr0 >> 8) & 0xFF);
    m_netif.mac[2] = static_cast<uint8_t>((idr0 >> 16) & 0xFF);
    m_netif.mac[3] = static_cast<uint8_t>((idr0 >> 24) & 0xFF);
    m_netif.mac[4] = static_cast<uint8_t>(idr4 & 0xFF);
    m_netif.mac[5] = static_cast<uint8_t>((idr4 >> 8) & 0xFF);

    bool all_zero = true, all_ff = true;
    for (int i = 0; i < 6; i++) {
        if (m_netif.mac[i] != 0x00) all_zero = false;
        if (m_netif.mac[i] != 0xFF) all_ff = false;
    }
    if (all_zero || all_ff) {
        m_netif.mac[0] = 0x52; m_netif.mac[1] = 0x54; m_netif.mac[2] = 0x00;
        m_netif.mac[3] = 0x12; m_netif.mac[4] = 0x34; m_netif.mac[5] = 0x56;
        log::warn("rtl8168: EEPROM MAC invalid, using fallback");
    }
}

/**
 * Per datasheet section 2.16: PHYAR at offset 0x60.
 * FreeBSD mandates a 20us inter-operation delay.
 */
int32_t rtl8168_driver::phy_read(uint8_t reg, uint16_t* out) {
    uint32_t cmd = (static_cast<uint32_t>(reg) << PHYAR_REG_SHIFT) & PHYAR_REG_MASK;
    reg_write32(REG_PHYAR, cmd);

    for (uint32_t i = 0; i < 2000; i++) {
        delay::us(1);
        uint32_t val = reg_read32(REG_PHYAR);
        if (val & PHYAR_FLAG) {
            *out = static_cast<uint16_t>(val & PHYAR_DATA_MASK);
            delay::us(20);
            return 0;
        }
    }

    log::warn("rtl8168: PHY read timeout (reg=0x%02x)", reg);
    return -1;
}

int32_t rtl8168_driver::phy_write(uint8_t reg, uint16_t data) {
    uint32_t cmd = PHYAR_FLAG |
                   ((static_cast<uint32_t>(reg) << PHYAR_REG_SHIFT) & PHYAR_REG_MASK) |
                   (data & PHYAR_DATA_MASK);
    reg_write32(REG_PHYAR, cmd);

    for (uint32_t i = 0; i < 2000; i++) {
        delay::us(1);
        uint32_t val = reg_read32(REG_PHYAR);
        if ((val & PHYAR_FLAG) == 0) {
            delay::us(20);
            return 0;
        }
    }

    log::warn("rtl8168: PHY write timeout (reg=0x%02x data=0x%04x)", reg, data);
    return -1;
}

int32_t rtl8168_driver::phy_reset() {
    int32_t rc = phy_write(phy::BMCR, phy::BMCR_RESET);
    if (rc != 0) return rc;

    for (uint32_t i = 0; i < 500; i++) {
        uint16_t bmcr = 0;
        rc = phy_read(phy::BMCR, &bmcr);
        if (rc != 0) return rc;
        if ((bmcr & phy::BMCR_RESET) == 0) {
            log::debug("rtl8168: PHY reset complete (%u ms)", i);
            return 0;
        }
        delay::us(1000);
    }

    log::error("rtl8168: PHY reset timeout");
    return -1;
}

int32_t rtl8168_driver::phy_auto_negotiate() {
    int32_t rc;
    uint16_t val;

    rc = phy_read(phy::ANAR, &val);
    if (rc != 0) return rc;
    val |= phy::ANAR_100BASETX_FDX | phy::ANAR_100BASETX |
           phy::ANAR_10BASET_FDX | phy::ANAR_10BASET |
           phy::ANAR_PAUSE;
    val = (val & ~0x1F) | phy::ANAR_SELECTOR_8023;
    rc = phy_write(phy::ANAR, val);
    if (rc != 0) return rc;

    rc = phy_read(phy::GBCR, &val);
    if (rc != 0) return rc;
    val |= phy::GBCR_1000BASET_FDX | phy::GBCR_1000BASET;
    rc = phy_write(phy::GBCR, val);
    if (rc != 0) return rc;

    rc = phy_read(phy::BMCR, &val);
    if (rc != 0) return rc;
    val |= phy::BMCR_ANE | phy::BMCR_RESTART_AN;
    rc = phy_write(phy::BMCR, val);
    if (rc != 0) return rc;

    log::info("rtl8168: auto-negotiation started");
    return 0;
}

void rtl8168_driver::phy_update_link() {
    uint8_t sts = reg_read8(REG_PHYSTATUS);
    bool was_up = m_link_up;
    m_link_up = (sts & PHYSTS_LINK) != 0;

    if (m_link_up && !was_up) {
        if (sts & PHYSTS_1000MF) {
            m_speed = 1000;
            m_full_duplex = true;
        } else if (sts & PHYSTS_100M) {
            m_speed = 100;
            m_full_duplex = (sts & PHYSTS_FULLDUP) != 0;
        } else if (sts & PHYSTS_10M) {
            m_speed = 10;
            m_full_duplex = (sts & PHYSTS_FULLDUP) != 0;
        } else {
            m_speed = 0;
            m_full_duplex = false;
        }
        log::info("rtl8168: link up %u Mbps %s-duplex",
                  m_speed, m_full_duplex ? "full" : "half");
    } else if (!m_link_up && was_up) {
        log::info("rtl8168: link down");
        m_speed = 0;
    }
}

int32_t rtl8168_driver::alloc_rings() {
    constexpr auto DMA_FLAGS = paging::PAGE_READ | paging::PAGE_WRITE |
                               paging::PAGE_USER | paging::PAGE_DMA;

    size_t tx_ring_bytes = TX_DESC_COUNT * sizeof(tx_desc);
    size_t tx_ring_pages = (tx_ring_bytes + 0xFFF) / 0x1000;
    uintptr_t tx_ring_va = 0;
    uint64_t tx_ring_pa = 0;
    int32_t rc = 0;

    RUN_ELEVATED(
        rc = vmm::alloc_contiguous(tx_ring_pages, pmm::ZONE_DMA32, DMA_FLAGS,
                                   vmm::ALLOC_ZERO, kva::tag::generic,
                                   tx_ring_va, tx_ring_pa)
    );
    if (rc != vmm::OK) {
        log::error("rtl8168: TX ring allocation failed (%d)", rc);
        return -1;
    }
    m_tx_ring = reinterpret_cast<tx_desc*>(tx_ring_va);
    m_tx_ring_phys = tx_ring_pa;

    size_t tx_buf_total = static_cast<size_t>(TX_DESC_COUNT) * net::ETH_FRAME_MAX;
    size_t tx_buf_pages = (tx_buf_total + 0xFFF) / 0x1000;
    RUN_ELEVATED(
        rc = vmm::alloc_contiguous(tx_buf_pages, pmm::ZONE_DMA32, DMA_FLAGS,
                                   vmm::ALLOC_ZERO, kva::tag::generic,
                                   m_tx_buf_vaddr, m_tx_buf_phys)
    );
    if (rc != vmm::OK) {
        log::error("rtl8168: TX buffer allocation failed (%d)", rc);
        return -1;
    }

    size_t rx_ring_bytes = RX_DESC_COUNT * sizeof(rx_desc);
    size_t rx_ring_pages = (rx_ring_bytes + 0xFFF) / 0x1000;
    uintptr_t rx_ring_va = 0;
    uint64_t rx_ring_pa = 0;
    RUN_ELEVATED(
        rc = vmm::alloc_contiguous(rx_ring_pages, pmm::ZONE_DMA32, DMA_FLAGS,
                                   vmm::ALLOC_ZERO, kva::tag::generic,
                                   rx_ring_va, rx_ring_pa)
    );
    if (rc != vmm::OK) {
        log::error("rtl8168: RX ring allocation failed (%d)", rc);
        return -1;
    }
    m_rx_ring = reinterpret_cast<rx_desc*>(rx_ring_va);
    m_rx_ring_phys = rx_ring_pa;

    size_t rx_buf_total = static_cast<size_t>(RX_DESC_COUNT) * RX_BUF_SIZE;
    size_t rx_buf_pages = (rx_buf_total + 0xFFF) / 0x1000;
    RUN_ELEVATED(
        rc = vmm::alloc_contiguous(rx_buf_pages, pmm::ZONE_DMA32, DMA_FLAGS,
                                   vmm::ALLOC_ZERO, kva::tag::generic,
                                   m_rx_buf_vaddr, m_rx_buf_phys)
    );
    if (rc != vmm::OK) {
        log::error("rtl8168: RX buffer allocation failed (%d)", rc);
        return -1;
    }

    log::debug("rtl8168: rings allocated: TX ring phys=0x%lx bufs=0x%lx, "
               "RX ring phys=0x%lx bufs=0x%lx",
               m_tx_ring_phys, m_tx_buf_phys,
               m_rx_ring_phys, m_rx_buf_phys);
    return 0;
}

void rtl8168_driver::free_rings() {
    if (m_tx_ring) {
        RUN_ELEVATED(vmm::free(reinterpret_cast<uintptr_t>(m_tx_ring)));
        m_tx_ring = nullptr;
        m_tx_ring_phys = 0;
    }
    if (m_tx_buf_vaddr) {
        RUN_ELEVATED(vmm::free(m_tx_buf_vaddr));
        m_tx_buf_vaddr = 0;
        m_tx_buf_phys = 0;
    }
    if (m_rx_ring) {
        RUN_ELEVATED(vmm::free(reinterpret_cast<uintptr_t>(m_rx_ring)));
        m_rx_ring = nullptr;
        m_rx_ring_phys = 0;
    }
    if (m_rx_buf_vaddr) {
        RUN_ELEVATED(vmm::free(m_rx_buf_vaddr));
        m_rx_buf_vaddr = 0;
        m_rx_buf_phys = 0;
    }
}

void rtl8168_driver::init_tx_ring() {
    m_tx_prod = 0;
    m_tx_cons = 0;
    m_tx_queued = 0;

    for (uint32_t i = 0; i < TX_DESC_COUNT; i++) {
        m_tx_ring[i].opts1 = 0;
        m_tx_ring[i].opts2 = 0;
        m_tx_ring[i].addr_lo = 0;
        m_tx_ring[i].addr_hi = 0;
    }
    m_tx_ring[TX_DESC_COUNT - 1].opts1 = TX_EOR;
}

void rtl8168_driver::init_rx_ring() {
    m_rx_cur = 0;

    for (uint32_t i = 0; i < RX_DESC_COUNT; i++) {
        m_rx_ring[i].opts1 = 0;
        m_rx_ring[i].opts2 = 0;
        m_rx_ring[i].addr_lo = 0;
        m_rx_ring[i].addr_hi = 0;
    }
}

int32_t rtl8168_driver::fill_rx_ring() {
    for (uint32_t i = 0; i < RX_DESC_COUNT; i++) {
        uint64_t phys = m_rx_buf_phys + static_cast<uint64_t>(i) * RX_BUF_SIZE;
        m_rx_ring[i].addr_lo = static_cast<uint32_t>(phys & 0xFFFFFFFF);
        m_rx_ring[i].addr_hi = static_cast<uint32_t>(phys >> 32);
        m_rx_ring[i].opts2 = 0;

        uint32_t flags = RX_OWN | (RX_BUF_SIZE & RX_BUF_SIZE_MASK);
        if (i == RX_DESC_COUNT - 1)
            flags |= RX_EOR;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        m_rx_ring[i].opts1 = flags;
    }

    log::debug("rtl8168: %u RX descriptors filled", RX_DESC_COUNT);
    return 0;
}

void rtl8168_driver::set_descriptor_addresses() {
    reg_write32(REG_TNPDS, static_cast<uint32_t>(m_tx_ring_phys & 0xFFFFFFFF));
    reg_write32(REG_TNPDS + 4, static_cast<uint32_t>(m_tx_ring_phys >> 32));
    reg_write32(REG_RDSAR, static_cast<uint32_t>(m_rx_ring_phys & 0xFFFFFFFF));
    reg_write32(REG_RDSAR + 4, static_cast<uint32_t>(m_rx_ring_phys >> 32));
}

int32_t rtl8168_driver::tx_callback(
    net::netif* iface, const uint8_t* frame, size_t len
) {
    if (!iface || !frame || len == 0) return -1;
    auto* drv = static_cast<rtl8168_driver*>(iface->driver_data);
    if (!drv) return -1;

    int32_t result = -1;
    RUN_ELEVATED({
        sync::irq_lock_guard guard(drv->m_lock);

        drv->process_tx_completions();

        if (drv->m_tx_queued >= TX_DESC_COUNT || len > net::ETH_FRAME_MAX) {
            result = -1;
        } else {
            uint32_t idx = drv->m_tx_prod;

            uintptr_t buf_va = drv->m_tx_buf_vaddr +
                               static_cast<uintptr_t>(idx) * net::ETH_FRAME_MAX;
            string::memcpy(reinterpret_cast<uint8_t*>(buf_va), frame, len);

            uint64_t buf_phys = drv->m_tx_buf_phys +
                                static_cast<uint64_t>(idx) * net::ETH_FRAME_MAX;

            drv->m_tx_ring[idx].addr_lo = static_cast<uint32_t>(buf_phys & 0xFFFFFFFF);
            drv->m_tx_ring[idx].addr_hi = static_cast<uint32_t>(buf_phys >> 32);
            drv->m_tx_ring[idx].opts2 = 0;

            uint32_t opts1 = TX_OWN | TX_FS | TX_LS |
                             (static_cast<uint32_t>(len) & TX_LEN_MASK);
            if (idx == TX_DESC_COUNT - 1)
                opts1 |= TX_EOR;

            // Release fence: NIC must see addr/opts2 before OWN is set.
            __atomic_thread_fence(__ATOMIC_RELEASE);
            drv->m_tx_ring[idx].opts1 = opts1;

            drv->m_tx_prod = (idx + 1) % TX_DESC_COUNT;
            drv->m_tx_queued++;

            drv->reg_write8(REG_TPPOLL, TPPOLL_NPQ);

            result = 0;
        }
    });

    return result;
}

void rtl8168_driver::process_tx_completions() {
    while (m_tx_queued > 0) {
        uint32_t idx = m_tx_cons;
        uint32_t opts1 = m_tx_ring[idx].opts1;

        if (opts1 & TX_OWN)
            break;

        m_tx_ring[idx].opts1 = (idx == TX_DESC_COUNT - 1) ? TX_EOR : 0;
        m_tx_ring[idx].addr_lo = 0;
        m_tx_ring[idx].addr_hi = 0;

        m_tx_cons = (idx + 1) % TX_DESC_COUNT;
        m_tx_queued--;
    }
}

void rtl8168_driver::process_rx() {
    uint32_t budget = RX_DESC_COUNT;

    while (budget > 0) {
        uint32_t idx = m_rx_cur;
        uint32_t opts1 = m_rx_ring[idx].opts1;

        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        if (opts1 & RX_OWN)
            break;

        budget--;

        if (opts1 & RX_RES) {
            log::warn("rtl8168: RX error on desc %u (opts1=0x%08x)", idx, opts1);
            goto recycle;
        }

        if ((opts1 & (RX_FS | RX_LS)) != (RX_FS | RX_LS)) {
            log::warn("rtl8168: RX multi-desc frame on desc %u, dropping", idx);
            goto recycle;
        }

        {
            uint32_t frame_len = opts1 & RX_FRAME_LEN_MASK;

            // Frame length includes 4-byte CRC
            if (frame_len > 4)
                frame_len -= 4;
            else
                goto recycle;

            if (frame_len > net::ETH_FRAME_MAX)
                goto recycle;

            uintptr_t buf_va = m_rx_buf_vaddr +
                               static_cast<uintptr_t>(idx) * RX_BUF_SIZE;
            const uint8_t* frame = reinterpret_cast<const uint8_t*>(buf_va);

            net::rx_frame(&m_netif, frame, frame_len);
        }

    recycle:
        {
            uint64_t phys = m_rx_buf_phys + static_cast<uint64_t>(idx) * RX_BUF_SIZE;
            m_rx_ring[idx].addr_lo = static_cast<uint32_t>(phys & 0xFFFFFFFF);
            m_rx_ring[idx].addr_hi = static_cast<uint32_t>(phys >> 32);
            m_rx_ring[idx].opts2 = 0;

            uint32_t new_opts1 = RX_OWN | (RX_BUF_SIZE & RX_BUF_SIZE_MASK);
            if (idx == RX_DESC_COUNT - 1)
                new_opts1 |= RX_EOR;

            __atomic_thread_fence(__ATOMIC_RELEASE);
            m_rx_ring[idx].opts1 = new_opts1;
        }

        m_rx_cur = (idx + 1) % RX_DESC_COUNT;
    }
}

/**
 * MSI interrupt handler. Masks IMR before ACKing ISR to prevent the
 * RTL8168's edge-triggered MSI from becoming permanently stuck: the
 * chip only fires an MSI on a 0->nonzero transition of (ISR & IMR).
 * The main loop re-enables IMR after draining all work.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void rtl8168_driver::on_interrupt(uint32_t) {
    reg_write16(REG_IMR, 0);
    uint16_t status = reg_read16(REG_ISR);
    if (status)
        reg_write16(REG_ISR, status);
}

void rtl8168_driver::enable_interrupts() {
    reg_write16(REG_IMR, m_imr);
}

void rtl8168_driver::disable_interrupts() {
    reg_write16(REG_IMR, 0);
    reg_read16(REG_IMR); // flush
    uint16_t isr = reg_read16(REG_ISR);
    if (isr)
        reg_write16(REG_ISR, isr);
}

bool rtl8168_driver::link_callback(net::netif* iface) {
    if (!iface) return false;
    auto* drv = static_cast<rtl8168_driver*>(iface->driver_data);
    return drv ? drv->m_link_up : false;
}

void rtl8168_driver::poll_callback(net::netif* iface) {
    if (!iface) return;
    auto* drv = static_cast<rtl8168_driver*>(iface->driver_data);
    if (!drv) return;

    RUN_ELEVATED({
        sync::irq_lock_guard guard(drv->m_lock);
        drv->process_rx();
        drv->process_tx_completions();
    });
    RUN_ELEVATED(net::drain_deferred_tx());
}

/**
 * Start sequence per datasheet section 7: configure C+CR, CMD, TCR, RCR.
 * TX+RX engines are enabled before writing TCR/RCR (datasheet requirement).
 */
void rtl8168_driver::hw_start() {
    disable_interrupts();
    hw_reset();

    config_unlock();
    reg_write16(REG_CPCR, CPCR_RXCHKSUM);
    reg_write16(REG_RMS, static_cast<uint16_t>(RX_BUF_SIZE));
    reg_write8(REG_MTPS, MTPS_NORMAL);
    set_descriptor_addresses();
    config_lock();

    reg_write8(REG_CMD, CMD_TE | CMD_RE);

    // Preserve hardware version ID bits in TCR
    uint32_t tcr = reg_read32(REG_TCR);
    tcr &= ~(TCR_IFG_MASK | TCR_MXDMA_MASK);
    tcr |= TCR_IFG_DEFAULT | TCR_MXDMA_UNLIMITED;
    reg_write32(REG_TCR, tcr);

    reg_write32(REG_RCR, RCR_RXFTH_NONE | RCR_MXDMA_UNLIMITED |
                          RCR_AB | RCR_APM | RCR_AM);

    reg_write32(REG_MAR0, 0xFFFFFFFF);
    reg_write32(REG_MAR4, 0xFFFFFFFF);

    // 8168G+ chips gate RX behind the RXDV bit; open it.
    if (chip_is_8168g_plus(m_chip_version)) {
        uint32_t misc = reg_read32(REG_MISC);
        misc &= ~MISC_RXDV_GATED;
        reg_write32(REG_MISC, misc);
        log::debug("rtl8168: RXDV gate opened (8168G+ chip)");
    }

    enable_interrupts();
    log::debug("rtl8168: hardware started");
}

void rtl8168_driver::hw_stop() {
    disable_interrupts();

    if (chip_is_8168g_plus(m_chip_version)) {
        uint32_t misc = reg_read32(REG_MISC);
        misc |= MISC_RXDV_GATED;
        reg_write32(REG_MISC, misc);
        delay::us(2000);
    }

    reg_write8(REG_CMD, 0);
    hw_reset();

    log::debug("rtl8168: hardware stopped");
}

void rtl8168_driver::dump_state() {
    log::debug("rtl8168: --- register dump ---");
    log::debug("rtl8168:  TxConfig    = 0x%08x", reg_read32(REG_TCR));
    log::debug("rtl8168:  RxConfig    = 0x%08x", reg_read32(REG_RCR));
    log::debug("rtl8168:  CMD         = 0x%02x", reg_read8(REG_CMD));
    log::debug("rtl8168:  IMR         = 0x%04x  ISR = 0x%04x",
              reg_read16(REG_IMR), reg_read16(REG_ISR));
    log::debug("rtl8168:  PHYStatus   = 0x%02x", reg_read8(REG_PHYSTATUS));
    log::debug("rtl8168:  C+CR        = 0x%04x", reg_read16(REG_CPCR));
    log::debug("rtl8168:  RMS         = 0x%04x", reg_read16(REG_RMS));
    log::debug("rtl8168:  MTPS        = 0x%02x", reg_read8(REG_MTPS));
    log::debug("rtl8168:  MISC        = 0x%08x (RXDV_GATED=%s)",
              reg_read32(REG_MISC),
              (reg_read32(REG_MISC) & MISC_RXDV_GATED) ? "ON" : "off");
    log::debug("rtl8168:  MAC %02x:%02x:%02x:%02x:%02x:%02x",
              m_netif.mac[0], m_netif.mac[1], m_netif.mac[2],
              m_netif.mac[3], m_netif.mac[4], m_netif.mac[5]);
    log::debug("rtl8168:  chip=0x%03x link=%s speed=%u duplex=%s",
              static_cast<uint16_t>(m_chip_version),
              m_link_up ? "up" : "down",
              m_speed,
              m_full_duplex ? "full" : "half");
    log::debug("rtl8168:  TX prod=%u cons=%u queued=%u",
              m_tx_prod, m_tx_cons, m_tx_queued);
    log::debug("rtl8168:  TX ring phys=0x%lx bufs phys=0x%lx",
              m_tx_ring_phys, m_tx_buf_phys);
    log::debug("rtl8168:  RX cur=%u", m_rx_cur);
    log::debug("rtl8168:  RX ring phys=0x%lx bufs phys=0x%lx",
              m_rx_ring_phys, m_rx_buf_phys);
    log::debug("rtl8168:  RX desc[0] opts1=0x%08x lo=0x%08x hi=0x%08x",
              m_rx_ring[0].opts1, m_rx_ring[0].addr_lo, m_rx_ring[0].addr_hi);

    uint16_t bmcr = 0, bmsr = 0, id1 = 0, id2 = 0;
    phy_read(phy::BMCR, &bmcr);
    phy_read(phy::BMSR, &bmsr);
    phy_read(phy::PHYIDR1, &id1);
    phy_read(phy::PHYIDR2, &id2);
    log::debug("rtl8168:  PHY ID=0x%04x:0x%04x BMCR=0x%04x BMSR=0x%04x",
              id1, id2, bmcr, bmsr);
    log::debug("rtl8168: --- end dump ---");
}

int32_t rtl8168_driver::attach() {
    log::info("rtl8168: attaching to %02x:%02x.%x (10EC:8168)",
              m_dev->bus(), m_dev->slot(), m_dev->func());

    RUN_ELEVATED({
        m_dev->enable();
        m_dev->enable_bus_mastering();
    });

    int32_t rc = map_bar(RTL_MMIO_BAR, m_mmio_va, paging::PAGE_USER);
    if (rc != 0) {
        log::error("rtl8168: failed to map BAR %u (%d)", RTL_MMIO_BAR, rc);
        return rc;
    }
    log::info("rtl8168: MMIO mapped at VA 0x%lx", m_mmio_va);

    hw_reset();
    m_chip_version = identify_chip();

    read_mac_address();
    log::info("rtl8168: MAC %02x:%02x:%02x:%02x:%02x:%02x",
              m_netif.mac[0], m_netif.mac[1], m_netif.mac[2],
              m_netif.mac[3], m_netif.mac[4], m_netif.mac[5]);

    rc = phy_reset();
    if (rc != 0) {
        log::warn("rtl8168: PHY reset failed (%d), continuing anyway", rc);
    }

    rc = phy_auto_negotiate();
    if (rc != 0) {
        log::warn("rtl8168: PHY auto-negotiation start failed (%d)", rc);
    }

    rc = alloc_rings();
    if (rc != 0) return rc;

    init_tx_ring();
    init_rx_ring();
    rc = fill_rx_ring();
    if (rc != 0) return rc;

    int32_t msi_rc = setup_msi(1);
    if (msi_rc != 0) {
        log::warn("rtl8168: MSI setup failed, will use polling");
        m_has_msi = false;
    } else {
        m_has_msi = true;
        log::info("rtl8168: MSI configured");
    }

    hw_start();

    string::memcpy(m_netif.name, "eth0", 5);
    m_netif.transmit = tx_callback;
    m_netif.link_up = link_callback;
    m_netif.poll = poll_callback;
    m_netif.driver_data = this;
    net::register_netif(&m_netif);

    log::info("rtl8168: attached successfully (%s)",
              m_has_msi ? "MSI" : "polling");
    dump_state();
    return 0;
}

int32_t rtl8168_driver::detach() {
    log::info("rtl8168: detaching");
    hw_stop();
    net::unregister_netif(&m_netif);
    free_rings();
    return pci_driver::detach();
}

void rtl8168_driver::run() {
    log::info("rtl8168: driver task running (%s)",
              m_has_msi ? "interrupt-driven" : "polling");

    // Wait for PHY auto-negotiation (gigabit typically takes 2-4s)
    log::info("rtl8168: waiting for link...");
    bool got_link = false;
    for (uint32_t i = 0; i < 50; i++) {
        RUN_ELEVATED(sched::sleep_ms(100));
        phy_update_link();
        if (m_link_up) {
            got_link = true;
            break;
        }
    }
    if (!got_link) {
        log::warn("rtl8168: link not up after 5 seconds, proceeding anyway");
    }

    int32_t dhcp_rc = net::dhcp_configure(&m_netif);
    if (dhcp_rc != net::OK) {
        log::warn("rtl8168: DHCP failed (%d), interface left unconfigured", dhcp_rc);
    }

    uint32_t link_poll_counter = 0;
    constexpr uint32_t LINK_POLL_INTERVAL = 100;

    while (true) {
        if (m_has_msi) {
            wait_for_event();
        } else {
            RUN_ELEVATED(sched::sleep_ms(1));
        }

        RUN_ELEVATED({
            sync::irq_lock_guard guard(m_lock);
            process_rx();
            process_tx_completions();
        });

        RUN_ELEVATED(net::drain_deferred_tx());

        if (++link_poll_counter >= LINK_POLL_INTERVAL) {
            link_poll_counter = 0;
            phy_update_link();
        }

        // Re-enable IMR after draining work. If ISR bits accumulated
        // while masked, this creates a 0->nonzero (ISR & IMR) transition
        // that fires a fresh MSI.
        if (m_has_msi) {
            reg_write16(REG_IMR, m_imr);
        }
    }
}

REGISTER_PCI_DRIVER(rtl8168_driver,
    PCI_MATCH(RTL_VENDOR_ID, RTL_DEVICE_ID_8168, 0x02, 0x00, PCI_MATCH_ANY_8),
    PCI_DRIVER_FACTORY(rtl8168_driver));

} // namespace drivers
