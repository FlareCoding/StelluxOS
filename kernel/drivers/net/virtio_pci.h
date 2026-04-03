#ifndef STELLUX_DRIVERS_NET_VIRTIO_PCI_H
#define STELLUX_DRIVERS_NET_VIRTIO_PCI_H

#include "common/types.h"

namespace drivers::virtio {

// Virtio PCI vendor ID
constexpr uint16_t VIRTIO_VENDOR_ID = 0x1AF4;

// Virtio device IDs (transitional range: 0x1000-0x103F)
constexpr uint16_t VIRTIO_DEV_NET_TRANSITIONAL = 0x1000;
// Modern device IDs (0x1040+subsystem_device_id)
constexpr uint16_t VIRTIO_DEV_NET_MODERN = 0x1041;

// Virtio PCI capability types (vendor-specific cap, cap_vndr = 0x09)
constexpr uint8_t VIRTIO_PCI_CAP_COMMON_CFG  = 1;
constexpr uint8_t VIRTIO_PCI_CAP_NOTIFY_CFG  = 2;
constexpr uint8_t VIRTIO_PCI_CAP_ISR_CFG     = 3;
constexpr uint8_t VIRTIO_PCI_CAP_DEVICE_CFG  = 4;
constexpr uint8_t VIRTIO_PCI_CAP_PCI_CFG     = 5;

// Virtio PCI capability structure (in PCI config space)
struct virtio_pci_cap {
    uint8_t  cap_vndr;   // 0x09
    uint8_t  cap_next;
    uint8_t  cap_len;
    uint8_t  cfg_type;   // VIRTIO_PCI_CAP_*
    uint8_t  bar;
    uint8_t  id;
    uint8_t  padding[2];
    uint32_t offset;
    uint32_t length;
} __attribute__((packed));

// Virtio common configuration structure (mapped via BAR MMIO)
// Offsets from the base of the common config region
struct virtio_pci_common_cfg {
    uint32_t device_feature_select;  // 0x00
    uint32_t device_feature;         // 0x04
    uint32_t driver_feature_select;  // 0x08
    uint32_t driver_feature;         // 0x0C
    uint16_t msix_config;            // 0x10
    uint16_t num_queues;             // 0x12
    uint8_t  device_status;          // 0x14
    uint8_t  config_generation;      // 0x15
    uint16_t queue_select;           // 0x16
    uint16_t queue_size;             // 0x18
    uint16_t queue_msix_vector;      // 0x1A
    uint16_t queue_enable;           // 0x1C
    uint16_t queue_notify_off;       // 0x1E
    uint64_t queue_desc;             // 0x20
    uint64_t queue_avail;            // 0x28 (driver)
    uint64_t queue_used;             // 0x30 (device)
} __attribute__((packed));

// Device status bits
constexpr uint8_t VIRTIO_STATUS_ACKNOWLEDGE = 1;
constexpr uint8_t VIRTIO_STATUS_DRIVER      = 2;
constexpr uint8_t VIRTIO_STATUS_DRIVER_OK   = 4;
constexpr uint8_t VIRTIO_STATUS_FEATURES_OK = 8;
constexpr uint8_t VIRTIO_STATUS_FAILED      = 128;

// Feature bits
constexpr uint64_t VIRTIO_F_VERSION_1       = (1ULL << 32);
constexpr uint64_t VIRTIO_NET_F_MAC         = (1ULL << 5);
constexpr uint64_t VIRTIO_NET_F_STATUS      = (1ULL << 16);
constexpr uint64_t VIRTIO_NET_F_MRG_RXBUF   = (1ULL << 15);

// MSI-X vector constants
constexpr uint16_t VIRTIO_MSI_NO_VECTOR = 0xFFFF;

// Virtio-net device config (mapped via BAR at device config offset)
struct virtio_net_config {
    uint8_t  mac[6];
    uint16_t status;
} __attribute__((packed));

// Virtio-net header prepended to every packet (legacy format, 10 bytes;
// modern with VIRTIO_F_VERSION_1 uses 12 bytes with num_buffers)
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers; // only present with VIRTIO_F_VERSION_1
} __attribute__((packed));

constexpr uint8_t VIRTIO_NET_HDR_F_NEEDS_CSUM = 1;
constexpr uint8_t VIRTIO_NET_HDR_GSO_NONE     = 0;

// Virtio-net queue indices
constexpr uint16_t VIRTIO_NET_QUEUE_RX = 0;
constexpr uint16_t VIRTIO_NET_QUEUE_TX = 1;

// Parsed virtio PCI capability locations
struct virtio_pci_config {
    uint8_t  common_bar;
    uint32_t common_offset;
    uint32_t common_length;

    uint8_t  notify_bar;
    uint32_t notify_offset;
    uint32_t notify_length;
    uint32_t notify_off_multiplier;

    uint8_t  isr_bar;
    uint32_t isr_offset;

    uint8_t  device_bar;
    uint32_t device_offset;
    uint32_t device_length;

    bool     has_common;
    bool     has_notify;
    bool     has_isr;
    bool     has_device;
};

} // namespace drivers::virtio

#endif // STELLUX_DRIVERS_NET_VIRTIO_PCI_H
