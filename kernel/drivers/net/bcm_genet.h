#ifndef STELLUX_DRIVERS_NET_BCM_GENET_H
#define STELLUX_DRIVERS_NET_BCM_GENET_H

#include "drivers/platform_driver.h"
#include "drivers/net/bcm_genet_regs.h"
#include "drivers/net/phy_regs.h"
#include "net/net.h"
#include "sync/spinlock.h"

namespace drivers {

/**
 * BCM GENET v5 Gigabit Ethernet driver for Raspberry Pi 4.
 *
 * Platform device discovered via FDT (compatible "brcm,bcm2711-genet-v5")
 * or ACPI OEM detection. Uses MMIO registers, DMA descriptor rings, MDIO
 * bus for PHY management, and GIC SPI interrupts.
 */
class bcm_genet_driver : public platform_driver {
public:
    bcm_genet_driver(uint64_t reg_phys, uint64_t reg_size,
                     uint32_t irq0, uint32_t irq1);

    int32_t attach() override;
    int32_t detach() override;
    void run() override;

private:
    // Register access
    uint32_t reg_read(uint32_t offset);
    void reg_write(uint32_t offset, uint32_t value);

    // MDIO / PHY
    int32_t mdio_read(uint8_t phy_addr, uint8_t reg, uint16_t* out);
    int32_t mdio_write(uint8_t phy_addr, uint8_t reg, uint16_t data);
    int32_t phy_detect();
    int32_t phy_reset();
    int32_t phy_reset_post_action();
    int32_t phy_auto_negotiate();
    int32_t phy_update_link();
    void phy_configure_mac(phy::phy_speed speed, phy::phy_duplex duplex);

    // Controller init / reset
    void genet_reset();
    void read_mac_address();
    void write_mac_address();
    void set_phy_mode();

    // DMA
    int32_t dma_alloc();
    void dma_free();
    void dma_init_rings();
    int32_t dma_map_rx_descriptors();
    void dma_enable_tx_rx();
    void dma_disable_tx_rx();

    // TX path
    static int32_t tx_callback(net::netif* iface, const uint8_t* frame, size_t len);
    void process_tx_completions();

    // RX path
    void process_rx();
    void rx_remap_descriptor(uint16_t desc_idx);

    // Interrupts
    int32_t setup_interrupts();
    void teardown_interrupts();
    static void isr(uint32_t irq, void* context);
    void enable_interrupts();
    void disable_interrupts();

    // Net interface callbacks
    static bool link_callback(net::netif* iface);
    static void poll_callback(net::netif* iface);

    // MAC filter
    void set_promisc(bool enable);
    void setup_rx_filter();

    // Debug
    void dump_state();

    // PHY state
    uint8_t          m_phy_addr;
    bool             m_link_up;
    phy::phy_speed   m_speed;
    phy::phy_duplex  m_duplex;

    // DMA state
    // RX: one large contiguous allocation, sliced into DMA_DESC_COUNT buffers
    uintptr_t        m_rx_buf_vaddr;
    uint64_t         m_rx_buf_phys;
    uint16_t         m_rx_cons_index;
    uint16_t         m_rx_prod_index;

    // TX: one large contiguous allocation, sliced into DMA_DESC_COUNT buffers
    uintptr_t        m_tx_buf_vaddr;
    uint64_t         m_tx_buf_phys;
    uint16_t         m_tx_cons_index;
    uint16_t         m_tx_prod_index;
    uint16_t         m_tx_queued;

    // Lock protecting DMA state and register access
    sync::spinlock   m_lock;

    // Network interface
    net::netif       m_netif;

    // Whether interrupts were successfully set up
    bool             m_has_irq;
};

/**
 * Factory function called by platform_driver framework.
 * Returns a heap-allocated driver instance, or nullptr on failure.
 */
bcm_genet_driver* create_bcm_genet(uint64_t reg_phys, uint64_t reg_size,
                                    uint32_t irq0, uint32_t irq1);

} // namespace drivers

#endif // STELLUX_DRIVERS_NET_BCM_GENET_H
