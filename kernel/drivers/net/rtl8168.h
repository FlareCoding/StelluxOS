#ifndef STELLUX_DRIVERS_NET_RTL8168_H
#define STELLUX_DRIVERS_NET_RTL8168_H

#include "drivers/pci_driver.h"
#include "drivers/net/rtl8168_regs.h"
#include "net/net.h"
#include "sync/spinlock.h"

namespace drivers {

/**
 * Realtek RTL8111/RTL8168 PCIe Gigabit Ethernet driver.
 *
 * PCI device: 10EC:8168. Discovered via REGISTER_PCI_DRIVER with class
 * 0x02/0x00 (Ethernet controller). Uses a single MMIO BAR (BAR 2),
 * host-memory DMA descriptor rings, and an integrated PHY accessed
 * through the PHYAR register.
 *
 * The driver follows the same patterns as bcm_genet_driver and
 * virtio_net_driver: fill a net::netif with callbacks, register with
 * the stack, deliver RX via net::rx_frame(), drain deferred TX after
 * RX processing.
 */
class rtl8168_driver : public pci_driver {
public:
    rtl8168_driver(pci::device* dev);

    int32_t attach() override;
    int32_t detach() override;
    void run() override;

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void on_interrupt(uint32_t vector) override;

private:
    // ================================================================
    // Register access (through mapped MMIO BAR)
    // ================================================================
    uint8_t  reg_read8(uint16_t offset);
    uint16_t reg_read16(uint16_t offset);
    uint32_t reg_read32(uint16_t offset);
    void reg_write8(uint16_t offset, uint8_t value);
    void reg_write16(uint16_t offset, uint16_t value);
    void reg_write32(uint16_t offset, uint32_t value);

    // ================================================================
    // Chip identification and initialization
    // ================================================================
    rtl8168::chip_version identify_chip();
    void hw_reset();
    void read_mac_address();
    void set_rx_mode();

    // Config register write enable/disable (9346CR lock/unlock)
    void config_unlock();
    void config_lock();

    // ================================================================
    // PHY access via PHYAR indirect register
    // ================================================================
    int32_t phy_read(uint8_t reg, uint16_t* out);
    int32_t phy_write(uint8_t reg, uint16_t data);
    int32_t phy_reset();
    int32_t phy_auto_negotiate();
    void    phy_update_link();

    // ================================================================
    // DMA descriptor ring management
    // ================================================================
    int32_t alloc_rings();
    void free_rings();
    void init_tx_ring();
    void init_rx_ring();
    int32_t fill_rx_ring();
    void set_descriptor_addresses();

    // ================================================================
    // TX path
    // ================================================================
    static int32_t tx_callback(net::netif* iface, const uint8_t* frame, size_t len);
    void process_tx_completions();

    // ================================================================
    // RX path
    // ================================================================
    void process_rx();

    // ================================================================
    // Network interface callbacks
    // ================================================================
    static bool link_callback(net::netif* iface);
    static void poll_callback(net::netif* iface);

    // ================================================================
    // Hardware enable/disable
    // ================================================================
    void hw_start();
    void hw_stop();
    void enable_interrupts();
    void disable_interrupts();

    // ================================================================
    // Debug / diagnostic
    // ================================================================
    void dump_state();

    // ================================================================
    // Member state
    // ================================================================

    // MMIO base virtual address (from BAR mapping)
    uintptr_t m_mmio_va;

    // Chip version detected from TxConfig
    rtl8168::chip_version m_chip_version;

    // PHY / link state
    bool     m_link_up;
    uint16_t m_speed;     // 10, 100, 1000
    bool     m_full_duplex;

    // TX descriptor ring (host memory, DMA-visible)
    rtl8168::tx_desc* m_tx_ring;      // virtual address
    uint64_t          m_tx_ring_phys;  // physical address
    uint32_t          m_tx_prod;       // next descriptor to fill (software)
    uint32_t          m_tx_cons;       // next descriptor to reclaim (software)
    uint32_t          m_tx_queued;     // number of descriptors owned by NIC

    // TX buffer pool: one contiguous DMA region, sliced by descriptor index
    uintptr_t         m_tx_buf_vaddr;
    uint64_t          m_tx_buf_phys;

    // RX descriptor ring (host memory, DMA-visible)
    rtl8168::rx_desc* m_rx_ring;      // virtual address
    uint64_t          m_rx_ring_phys;  // physical address
    uint32_t          m_rx_cur;        // next descriptor to check (software)

    // RX buffer pool: one contiguous DMA region, sliced by descriptor index
    uintptr_t         m_rx_buf_vaddr;
    uint64_t          m_rx_buf_phys;

    // Lock protecting descriptor rings and register access
    sync::spinlock m_lock;

    // Network interface
    net::netif m_netif;

    // Whether MSI was successfully configured
    bool m_has_msi;

    // Saved interrupt mask for enable/disable cycling
    uint16_t m_imr;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_NET_RTL8168_H
