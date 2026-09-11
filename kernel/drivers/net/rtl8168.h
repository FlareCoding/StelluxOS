#ifndef STELLUX_DRIVERS_NET_RTL8168_H
#define STELLUX_DRIVERS_NET_RTL8168_H

#include "drivers/pci_driver.h"
#include "drivers/net/rtl8168_regs.h"
#include "net/interface.h"
#include "sync/spinlock.h"

namespace drivers {

/**
 * Realtek RTL8111/RTL8168 PCIe Gigabit Ethernet driver.
 * Uses MMIO BAR 2, DMA descriptor rings, and integrated PHY via PHYAR.
 */
class rtl8168_driver : public pci_driver, public net::interface {
public:
    rtl8168_driver(pci::device* dev);

    int32_t attach() override;
    int32_t detach() override;
    void run() override;

    int32_t transmit(net::packet* pkt) override;

    /** @note Privilege: **required** */
    __PRIVILEGED_CODE void on_interrupt(uint32_t vector) override;

private:
    // Finished descriptors taken from the ring under m_lock and held by the
    // host until recycled, so their buffers stay stable while the frames are
    // delivered without the lock. A zero length marks a frame the hardware
    // flagged as bad, which is counted and recycled without delivery.
    static constexpr uint32_t RX_BATCH_MAX = 32;
    static constexpr uint32_t RX_PASSES_PER_WAKEUP = rtl8168::RX_DESC_COUNT / RX_BATCH_MAX;
    struct rx_batch_entry {
        uint16_t idx;
        uint16_t len;
    };

    struct rx_batch {
        rx_batch_entry entries[RX_BATCH_MAX];
        uint32_t count;
    };

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
    void arm_rx_desc(uint32_t idx);
    void set_descriptor_addresses();

    void process_tx_completions();               // requires m_lock
    void drain_rx_locked(rx_batch& batch);       // requires m_lock
    void deliver_rx_batch(const rx_batch& batch); // called without m_lock
    void recycle_rx_locked(const rx_batch& batch); // requires m_lock

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
    bool m_has_msi;
    uint16_t m_imr;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_NET_RTL8168_H
