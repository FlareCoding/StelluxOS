#ifndef STELLUX_DRIVERS_VIRTIO_VIRTIO_PCI_DRIVER_H
#define STELLUX_DRIVERS_VIRTIO_VIRTIO_PCI_DRIVER_H

#include "drivers/pci_driver.h"
#include "drivers/virtio/virtio_pci.h"
#include "drivers/virtio/virtqueue.h"

namespace drivers {

/**
 * A PCI driver for a modern virtio device. It owns the virtio PCI transport:
 * the config regions the device advertises as vendor capabilities, the status
 * handshake, feature negotiation, and virtqueue setup. Device drivers derive
 * from it and add their own queues and protocol on top.
 *
 * A driver calls `start_transport()` first, then `setup_queue()` for each
 * queue it uses, and `set_driver_ok()` once it is ready for the device to run.
 */
class virtio_pci_driver : public pci_driver {
public:
    /**
     * @param name Driver name, which also names the driver task.
     * @param log_name Prefix of the transport's log messages, matching the driver's own.
     */
    virtio_pci_driver(const char* name, const char* log_name, pci::device* dev)
        : pci_driver(name, dev), m_log_name(log_name) {}

protected:
    /**
     * @brief Enable the device, map its config regions, reset it, and agree
     * on features. The device must offer every feature in `required`, and
     * the driver takes whichever of `optional` it offers.
     * @param out_features Receives the negotiated feature set.
     * @return 0 on success, negative on failure. A device that rejects the
     * features is marked failed.
     */
    int32_t start_transport(uint64_t required, uint64_t optional, uint64_t* out_features);

    /**
     * @brief Size, allocate, and enable virtqueue `index`, capped at
     * `VIRTQ_MAX_SIZE`.
     * @param out_notify Receives the address that kicks the queue.
     * @return 0, or negative when the device lacks the queue or memory runs out.
     */
    int32_t setup_queue(uint16_t index, virtio::virtqueue& queue, uintptr_t* out_notify);

    // Route a queue's interrupts or config change interrupts to an MSI-X vector.
    // Both return negative when the device cannot use the vector.
    int32_t route_queue_msix(uint16_t index, uint16_t vector);
    int32_t route_config_msix(uint16_t vector);

    // Queues may carry buffers only after the driver reports itself ready
    void set_driver_ok();
    void set_failed();

    // Does nothing until start_transport() maps the device, so a failed attach can still detach
    void reset_device();

    // Reading the ISR status acknowledges a legacy or MSI interrupt
    uint8_t ack_interrupt();

    // The device-specific config region, or null when the device has none
    template <typename T>
    volatile T* device_config() const {
        return reinterpret_cast<volatile T*>(m_device_cfg);
    }

private:
    int32_t parse_caps();
    int32_t map_regions();
    int32_t map_region(uint8_t bar, uint32_t offset, uintptr_t* out_addr);
    int32_t negotiate_features(uint64_t required, uint64_t optional, uint64_t* out_features);
    void write_status(uint8_t status);
    uint8_t read_status();

    const char* m_log_name;
    virtio::virtio_pci_config m_pci_cfg = {};
    volatile virtio::virtio_pci_common_cfg* m_common_cfg = nullptr;
    uintptr_t m_notify_base = 0;
    uint32_t  m_notify_off_multiplier = 0;
    uintptr_t m_isr_addr = 0;
    uintptr_t m_device_cfg = 0;
};

} // namespace drivers

#endif // STELLUX_DRIVERS_VIRTIO_VIRTIO_PCI_DRIVER_H
