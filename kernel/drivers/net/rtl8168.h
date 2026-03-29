#ifndef STELLUX_DRIVERS_NET_RTL8168_H
#define STELLUX_DRIVERS_NET_RTL8168_H

#include "drivers/pci_driver.h"
#include "drivers/net/rtl8168_regs.h"
#include "net/net.h"
#include "sync/spinlock.h"

namespace drivers {

/**
 * Realtek RTL8111/RTL8168 PCIe Gigabit Ethernet driver.
 * Uses MMIO BAR 2, DMA descriptor rings, and integrated PHY via PHYAR.
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
    uint8_t  reg_read8(uint16_t offset);
    uint16_t reg_read16(uint16_t offset);
    uint32_t reg_read32(uint16_t offset);
    void reg_write8(uint16_t offset, uint8_t value);
    void reg_write16(uint16_t offset, uint16_t value);
    void reg_write32(uint16_t offset, uint32_t value);

    rtl8168::chip_version identify_chip();
    void hw_reset();
    void read_mac_address();
    void config_unlock();
    void config_lock();

    int32_t phy_read(uint8_t reg, uint16_t* out);
    int32_t phy_write(uint8_t reg, uint16_t data);
    int32_t phy_reset();
    int32_t phy_auto_negotiate();
    void phy_update_link();

    int32_t alloc_rings();
    void free_rings();
    void init_tx_ring();
    void init_rx_ring();
    int32_t fill_rx_ring();
    void set_descriptor_addresses();

    static int32_t tx_callback(net::netif* iface,
                               const uint8_t* frame, size_t len);
    void process_tx_completions();
    void process_rx();

    static bool link_callback(net::netif* iface);
    static void poll_callback(net::netif* iface);

    void hw_start();
    void hw_stop();
    void enable_interrupts();
    void disable_interrupts();
    void dump_state();

    uintptr_t m_mmio_va;
    rtl8168::chip_version m_chip_version;

    bool     m_link_up;
    uint16_t m_speed;
    bool     m_full_duplex;

    rtl8168::tx_desc* m_tx_ring;
    uint64_t          m_tx_ring_phys;
    uint32_t          m_tx_prod;
    uint32_t          m_tx_cons;
    uint32_t          m_tx_queued;
    uintptr_t         m_tx_buf_vaddr;
    uint64_t          m_tx_buf_phys;

    rtl8168::rx_desc* m_rx_ring;
    uint64_t          m_rx_ring_phys;
    uint32_t          m_rx_cur;
    uintptr_t         m_rx_buf_vaddr;
    uint64_t          m_rx_buf_phys;

    sync::spinlock m_lock;
    net::netif m_netif;
    bool m_has_msi;
    uint16_t m_imr;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_NET_RTL8168_H
