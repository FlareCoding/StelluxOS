#include "drivers/virtio/virtio_pci_driver.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "hw/cpu.h"
#include "hw/mmio.h"
#include "sync/atomic.h"

namespace drivers {

using namespace virtio;

// Vendor-specific capabilities carry the virtio config regions
constexpr uint8_t PCI_CAP_VENDOR = 0x09;

// Offsets of the virtio fields inside a vendor capability
constexpr uint8_t CAP_CFG_TYPE = 3;
constexpr uint8_t CAP_BAR = 4;
constexpr uint8_t CAP_OFFSET = 8;
constexpr uint8_t CAP_LENGTH = 12;
constexpr uint8_t CAP_NOTIFY_MULTIPLIER = 16;

// Bounds the capability walk against a malformed, looping list
constexpr uint32_t MAX_CAPS = 48;
constexpr uint8_t CAP_PTR_MASK = 0xFC;

int32_t virtio_pci_driver::parse_caps() {
    string::memset(&m_pci_cfg, 0, sizeof(m_pci_cfg));

    uint16_t status = 0;
    RUN_ELEVATED(status = m_dev->config_read16(pci::CFG_STATUS));
    if (!(status & pci::STS_CAPABILITIES)) {
        log::error("%s: device has no PCI capabilities", m_log_name);
        return -1;
    }

    uint8_t cap_ptr = 0;
    RUN_ELEVATED(cap_ptr = m_dev->config_read8(pci::CFG_CAP_PTR));
    cap_ptr &= CAP_PTR_MASK;

    uint32_t visited = 0;
    while (cap_ptr != 0 && visited < MAX_CAPS) {
        uint8_t cap_id = 0;
        RUN_ELEVATED(cap_id = m_dev->config_read8(cap_ptr));

        if (cap_id == PCI_CAP_VENDOR) {
            uint8_t cfg_type = 0;
            uint8_t bar = 0;
            uint32_t offset = 0;
            uint32_t length = 0;
            RUN_ELEVATED({
                cfg_type = m_dev->config_read8(cap_ptr + CAP_CFG_TYPE);
                bar = m_dev->config_read8(cap_ptr + CAP_BAR);
                offset = m_dev->config_read32(cap_ptr + CAP_OFFSET);
                length = m_dev->config_read32(cap_ptr + CAP_LENGTH);
            });

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                m_pci_cfg.common_bar = bar;
                m_pci_cfg.common_offset = offset;
                m_pci_cfg.common_length = length;
                m_pci_cfg.has_common = true;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                m_pci_cfg.notify_bar = bar;
                m_pci_cfg.notify_offset = offset;
                m_pci_cfg.notify_length = length;
                m_pci_cfg.has_notify = true;
                RUN_ELEVATED(
                    m_pci_cfg.notify_off_multiplier = m_dev->config_read32(cap_ptr + CAP_NOTIFY_MULTIPLIER)
                );
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                m_pci_cfg.isr_bar = bar;
                m_pci_cfg.isr_offset = offset;
                m_pci_cfg.has_isr = true;
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                m_pci_cfg.device_bar = bar;
                m_pci_cfg.device_offset = offset;
                m_pci_cfg.device_length = length;
                m_pci_cfg.has_device = true;
                break;
            }
        }

        uint8_t next = 0;
        RUN_ELEVATED(next = m_dev->config_read8(cap_ptr + 1));
        cap_ptr = next & CAP_PTR_MASK;
        visited++;
    }

    if (!m_pci_cfg.has_common) {
        log::error("%s: missing common config capability", m_log_name);
        return -1;
    }

    return 0;
}

// map_bar() is idempotent, so regions sharing a BAR share its mapping
int32_t virtio_pci_driver::map_region(uint8_t bar, uint32_t offset, uintptr_t* out_addr) {
    uintptr_t bar_va = 0;
    int32_t rc = map_bar(bar, bar_va, paging::PAGE_USER);
    if (rc != 0) {
        log::error("%s: failed to map BAR %u", m_log_name, bar);
        return rc;
    }

    *out_addr = bar_va + offset;

    return 0;
}

int32_t virtio_pci_driver::map_regions() {
    uintptr_t common = 0;
    int32_t rc = map_region(m_pci_cfg.common_bar, m_pci_cfg.common_offset, &common);
    if (rc != 0) {
        return rc;
    }

    m_common_cfg = reinterpret_cast<volatile virtio_pci_common_cfg*>(common);

    if (m_pci_cfg.has_notify) {
        rc = map_region(m_pci_cfg.notify_bar, m_pci_cfg.notify_offset, &m_notify_base);
        if (rc != 0) {
            return rc;
        }

        m_notify_off_multiplier = m_pci_cfg.notify_off_multiplier;
    }

    if (m_pci_cfg.has_isr) {
        rc = map_region(m_pci_cfg.isr_bar, m_pci_cfg.isr_offset, &m_isr_addr);
        if (rc != 0) {
            return rc;
        }
    }

    if (m_pci_cfg.has_device) {
        rc = map_region(m_pci_cfg.device_bar, m_pci_cfg.device_offset, &m_device_cfg);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

void virtio_pci_driver::write_status(uint8_t status) {
    m_common_cfg->device_status = status;

    // Fence orders the status write ahead of later device config accesses
    sync::atomic_fence_seq_cst();
}

uint8_t virtio_pci_driver::read_status() {
    return m_common_cfg->device_status;
}

int32_t virtio_pci_driver::negotiate_features(uint64_t required, uint64_t optional, uint64_t* out_features) {
    // The 64-bit feature word is exposed through a 32-bit select window
    m_common_cfg->device_feature_select = 0;
    sync::atomic_fence_seq_cst();
    uint32_t features_lo = m_common_cfg->device_feature;

    m_common_cfg->device_feature_select = 1;
    sync::atomic_fence_seq_cst();
    uint32_t features_hi = m_common_cfg->device_feature;

    uint64_t device_features = (static_cast<uint64_t>(features_hi) << 32) | features_lo;
    if ((device_features & required) != required) {
        log::error("%s: device lacks required features 0x%lx", m_log_name, required & ~device_features);
        return -1;
    }

    uint64_t driver_features = required | (device_features & optional);
    log::info("%s: device features=0x%lx, driver features=0x%lx", m_log_name, device_features, driver_features);

    m_common_cfg->driver_feature_select = 0;
    sync::atomic_fence_seq_cst();
    m_common_cfg->driver_feature = static_cast<uint32_t>(driver_features & 0xFFFFFFFF);

    m_common_cfg->driver_feature_select = 1;
    sync::atomic_fence_seq_cst();
    m_common_cfg->driver_feature = static_cast<uint32_t>(driver_features >> 32);

    *out_features = driver_features;

    return 0;
}

int32_t virtio_pci_driver::start_transport(uint64_t required, uint64_t optional, uint64_t* out_features) {
    RUN_ELEVATED({
        m_dev->enable();
        m_dev->enable_bus_mastering();
    });

    int32_t rc = parse_caps();
    if (rc != 0) {
        return rc;
    }

    rc = map_regions();
    if (rc != 0) {
        return rc;
    }

    reset_device();
    write_status(VIRTIO_STATUS_ACKNOWLEDGE);
    write_status(read_status() | VIRTIO_STATUS_DRIVER);

    rc = negotiate_features(required, optional, out_features);
    if (rc != 0) {
        set_failed();
        return rc;
    }

    write_status(read_status() | VIRTIO_STATUS_FEATURES_OK);
    if (!(read_status() & VIRTIO_STATUS_FEATURES_OK)) {
        log::error("%s: device did not accept features", m_log_name);
        set_failed();
        return -1;
    }

    return 0;
}

int32_t virtio_pci_driver::setup_queue(uint16_t index, virtqueue& queue, uintptr_t* out_notify) {
    m_common_cfg->queue_select = index;
    sync::atomic_fence_seq_cst();
    uint16_t size = m_common_cfg->queue_size;
    if (size == 0) {
        log::error("%s: queue %u does not exist", m_log_name, index);
        return -1;
    }

    if (size > VIRTQ_MAX_SIZE) {
        size = VIRTQ_MAX_SIZE;
    }

    m_common_cfg->queue_size = size;

    int32_t rc = queue.init(size, index);
    if (rc != 0) {
        log::error("%s: queue %u init failed", m_log_name, index);
        return rc;
    }

    m_common_cfg->queue_desc = queue.desc_phys();
    m_common_cfg->queue_avail = queue.avail_phys();
    m_common_cfg->queue_used = queue.used_phys();
    m_common_cfg->queue_enable = 1;
    sync::atomic_fence_seq_cst();

    uint16_t notify_off = m_common_cfg->queue_notify_off;
    *out_notify = m_notify_base + static_cast<uintptr_t>(notify_off) * m_notify_off_multiplier;

    return 0;
}

void virtio_pci_driver::route_queue_msix(uint16_t index, uint16_t vector) {
    m_common_cfg->queue_select = index;
    sync::atomic_fence_seq_cst();
    m_common_cfg->queue_msix_vector = vector;
    sync::atomic_fence_seq_cst();
}

void virtio_pci_driver::route_config_msix(uint16_t vector) {
    m_common_cfg->msix_config = vector;
    sync::atomic_fence_seq_cst();
}

void virtio_pci_driver::set_driver_ok() {
    write_status(read_status() | VIRTIO_STATUS_DRIVER_OK);
}

void virtio_pci_driver::set_failed() {
    write_status(read_status() | VIRTIO_STATUS_FAILED);
}

void virtio_pci_driver::reset_device() {
    write_status(0);
    while (read_status() != 0) {
        cpu::relax();
    }
}

uint8_t virtio_pci_driver::ack_interrupt() {
    return m_isr_addr ? mmio::read8(m_isr_addr) : 0;
}

} // namespace drivers
