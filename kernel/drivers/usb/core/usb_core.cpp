#include "drivers/usb/core/usb_core.h"
#include "drivers/usb/core/usb_driver.h"
#include "drivers/usb/xhci/xhci.h"
#include "drivers/usb/xhci/xhci_device.h"
#include "drivers/usb/xhci/xhci_endpoint.h"
#include "common/logging.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "dynpriv/dynpriv.h"

extern "C" const usb::class_driver_entry __usb_class_drivers_start[];
extern "C" const usb::class_driver_entry __usb_class_drivers_end[];

namespace usb::core {

// Tracks which usb::device is associated with each xhci_device
static constexpr uint16_t MAX_USB_DEVICES = 256;
static usb::device* g_devices[MAX_USB_DEVICES] = {};
static usb::class_driver* g_bound_drivers[MAX_USB_DEVICES * 16] = {};

static bool match_interface(const class_match& match, const usb::interface* iface) {
    if (match.interface_class != USB_MATCH_ANY &&
        match.interface_class != iface->interface_class) {
        return false;
    }
    if (match.interface_subclass != USB_MATCH_ANY &&
        match.interface_subclass != iface->interface_subclass) {
        return false;
    }
    if (match.interface_protocol != USB_MATCH_ANY &&
        match.interface_protocol != iface->interface_protocol) {
        return false;
    }
    return true;
}

static void class_driver_task_entry(void* arg) {
    auto* drv = static_cast<usb::class_driver*>(arg);
    drv->run();
    sched::exit(0);
}

// Build a usb::device from the xHCI device's state
static usb::device* build_usb_device(
    drivers::xhci_hcd* hcd,
    drivers::xhci::xhci_device* xdev,
    const usb::usb_device_descriptor& desc
) {
    auto* dev = heap::ualloc_new<usb::device>();
    if (!dev) {
        return nullptr;
    }

    dev->vid = desc.idVendor;
    dev->pid = desc.idProduct;
    dev->bcd_usb = desc.bcdUsb;
    dev->device_class = desc.bDeviceClass;
    dev->device_subclass = desc.bDeviceSubClass;
    dev->device_protocol = desc.bDeviceProtocol;
    dev->speed = xdev->speed();
    dev->config_value = 0;
    dev->num_interfaces = xdev->num_interfaces();
    dev->is_hub = xdev->is_hub();
    dev->hub_num_ports = xdev->hub_num_ports();
    dev->hcd = hcd;
    dev->hcd_device = xdev;

    for (uint8_t i = 0; i < xdev->num_interfaces(); i++) {
        const auto& xi = xdev->interface_info(i);
        auto& iface = dev->interfaces[i];
        iface.interface_number = xi.interface_number;
        iface.alternate_setting = xi.alternate_setting;
        iface.interface_class = xi.interface_class;
        iface.interface_subclass = xi.interface_subclass;
        iface.interface_protocol = xi.interface_protocol;
        iface.num_endpoints = xi.num_endpoints;

        for (uint8_t j = 0; j < xi.num_endpoints && j < 16; j++) {
            auto* xep = xdev->endpoint(xi.endpoint_dcis[j]);
            if (!xep) {
                continue;
            }
            auto& ep = iface.endpoints[j];
            ep.address = xep->endpoint_addr();
            ep.transfer_type = xep->transfer_type();
            ep.max_packet_size = xep->max_packet_size();
            ep.interval = xep->interval();
        }
    }

    return dev;
}

void device_configured(drivers::xhci_hcd* hcd,
                       drivers::xhci::xhci_device* xdev,
                       const usb::usb_device_descriptor& desc) {
    uint8_t slot_id = xdev->slot_id();

    auto* dev = build_usb_device(hcd, xdev, desc);
    if (!dev) {
        log::error("usb: failed to allocate usb::device for slot %u", slot_id);
        return;
    }

    g_devices[slot_id] = dev;

    RUN_ELEVATED({
        const class_driver_entry* reg_start = __usb_class_drivers_start;
        const class_driver_entry* reg_end = __usb_class_drivers_end;

        for (uint8_t i = 0; i < dev->num_interfaces; i++) {
            auto* iface = &dev->interfaces[i];

            for (const class_driver_entry* reg = reg_start; reg < reg_end; reg++) {
                if (!match_interface(reg->match, iface)) {
                    continue;
                }

                auto* drv = reg->create(dev, iface);
                if (!drv) {
                    continue;
                }

                if (drv->probe(dev, iface) != 0) {
                    heap::ufree_delete(drv);
                    continue;
                }

                sched::task* t = sched::create_kernel_task(
                    class_driver_task_entry, drv, drv->name());
                if (!t) {
                    log::error("usb: failed to create task for %s", drv->name());
                    heap::ufree_delete(drv);
                    continue;
                }

                drv->m_task = t;
                sched::enqueue(t);

                log::info("usb: bound %s to interface %u (class=0x%02x)",
                           drv->name(), iface->interface_number, iface->interface_class);

                uint16_t drv_idx = static_cast<uint16_t>(slot_id) * 16 + i;
                if (drv_idx < MAX_USB_DEVICES * 16) {
                    g_bound_drivers[drv_idx] = drv;
                }

                break;
            }
        }
    });
}

void device_disconnected(drivers::xhci_hcd*,
                         drivers::xhci::xhci_device* xdev) {
    uint8_t slot_id = xdev->slot_id();
    auto* dev = g_devices[slot_id];
    if (!dev) {
        return;
    }

    // Notify bound class drivers
    for (uint8_t i = 0; i < dev->num_interfaces; i++) {
        uint16_t drv_idx = static_cast<uint16_t>(slot_id) * 16 + i;
        if (drv_idx >= MAX_USB_DEVICES * 16) {
            continue;
        }

        auto* drv = g_bound_drivers[drv_idx];
        if (!drv) {
            continue;
        }

        drv->disconnect();
        g_bound_drivers[drv_idx] = nullptr;
    }

    g_devices[slot_id] = nullptr;
    heap::ufree_delete(dev);
}

} // namespace usb::core
