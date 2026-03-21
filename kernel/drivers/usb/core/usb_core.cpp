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
static sync::spinlock g_usb_core_lock = sync::SPINLOCK_INIT;

struct finalization_ticket {
    usb::device* dev = nullptr;
    drivers::xhci_hcd* hcd = nullptr;
    drivers::xhci::xhci_device* xdev = nullptr;
};

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

static void publish_usb_device(uint8_t slot_id, usb::device* dev) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_usb_core_lock);
        g_devices[slot_id] = dev;
        sync::spin_unlock_irqrestore(g_usb_core_lock, irq);
    });
}

static void clear_usb_device_if_current(uint8_t slot_id, usb::device* dev) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_usb_core_lock);
        if (g_devices[slot_id] == dev) {
            g_devices[slot_id] = nullptr;
        }
        sync::spin_unlock_irqrestore(g_usb_core_lock, irq);
    });
}

static void publish_bound_driver(uint16_t drv_idx, usb::class_driver* drv) {
    if (drv_idx >= MAX_USB_DEVICES * 16) {
        return;
    }

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_usb_core_lock);
        g_bound_drivers[drv_idx] = drv;
        sync::spin_unlock_irqrestore(g_usb_core_lock, irq);
    });
}

static void clear_bound_driver_if_current(uint16_t drv_idx, usb::class_driver* drv) {
    if (drv_idx >= MAX_USB_DEVICES * 16) {
        return;
    }

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_usb_core_lock);
        if (g_bound_drivers[drv_idx] == drv) {
            g_bound_drivers[drv_idx] = nullptr;
        }
        sync::spin_unlock_irqrestore(g_usb_core_lock, irq);
    });
}

static usb::class_driver* take_bound_driver_for_device(uint16_t drv_idx, usb::device* dev) {
    if (!dev || drv_idx >= MAX_USB_DEVICES * 16) {
        return nullptr;
    }

    usb::class_driver* drv = nullptr;
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_usb_core_lock);
        auto* current = g_bound_drivers[drv_idx];
        if (current && current->bound_device() == dev) {
            drv = current;
            g_bound_drivers[drv_idx] = nullptr;
        }
        sync::spin_unlock_irqrestore(g_usb_core_lock, irq);
    });
    return drv;
}

static finalization_ticket claim_finalization_if_ready_locked(usb::device* dev) {
    finalization_ticket ticket = {};
    if (!dev ||
        !dev->disconnect_pending ||
        !dev->hcd_teardown_complete ||
        dev->active_driver_count != 0 ||
        dev->finalize_started) {
        return ticket;
    }

    dev->finalize_started = true;
    ticket.dev = dev;
    ticket.xdev = static_cast<drivers::xhci::xhci_device*>(dev->hcd_device);
    ticket.hcd = static_cast<drivers::xhci_hcd*>(dev->hcd);
    dev->hcd_device = nullptr;
    dev->hcd = nullptr;
    return ticket;
}

static void finish_claimed_finalization(finalization_ticket ticket) {
    if (!ticket.dev) {
        return;
    }

    uint8_t slot_id = ticket.xdev ? ticket.xdev->slot_id() : 0;
    if (ticket.xdev && ticket.xdev->core_device() == ticket.dev) {
        ticket.xdev->set_core_device(nullptr);
    }
    clear_usb_device_if_current(slot_id, ticket.dev);

    if (ticket.hcd && ticket.xdev) {
        ticket.hcd->release_disconnected_device(ticket.xdev);
    }
    heap::ufree_delete(ticket.dev);
}

static void class_driver_task_entry(void* arg) {
    auto* drv = static_cast<usb::class_driver*>(arg);
    uint8_t slot_id = drv->bound_slot_id();
    uint8_t iface_index = drv->bound_interface_index();
    auto* dev = drv->bound_device();
    drv->run();

    uint16_t drv_idx = static_cast<uint16_t>(slot_id) * 16 + iface_index;
    clear_bound_driver_if_current(drv_idx, drv);

    finalization_ticket ticket = {};
    if (dev) {
        RUN_ELEVATED({
            sync::irq_state irq = sync::spin_lock_irqsave(dev->lifetime_lock);
            if (dev->active_driver_count > 0) {
                dev->active_driver_count--;
            }
            ticket = claim_finalization_if_ready_locked(dev);
            sync::spin_unlock_irqrestore(dev->lifetime_lock, irq);
        });
    }

    heap::ufree_delete(drv);

    finish_claimed_finalization(ticket);

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
    dev->lifetime_lock = sync::SPINLOCK_INIT;
    dev->active_driver_count = 0;
    dev->disconnect_pending = false;
    dev->hcd_teardown_complete = false;
    dev->finalize_started = false;
    xdev->set_core_device(dev);

    for (uint8_t i = 0; i < xdev->num_interfaces(); i++) {
        const auto& xi = xdev->interface_info(i);
        auto& iface = dev->interfaces[i];
        iface.interface_number = xi.interface_number;
        iface.alternate_setting = xi.alternate_setting;
        iface.interface_class = xi.interface_class;
        iface.interface_subclass = xi.interface_subclass;
        iface.interface_protocol = xi.interface_protocol;
        iface.hid_report_desc_length = xi.hid_report_desc_length;
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

    publish_usb_device(slot_id, dev);

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
                drv->m_bound_device = dev;
                drv->m_bound_slot_id = slot_id;
                drv->m_bound_interface_index = i;
                sync::irq_state dev_irq = sync::spin_lock_irqsave(dev->lifetime_lock);
                dev->active_driver_count++;
                sync::spin_unlock_irqrestore(dev->lifetime_lock, dev_irq);

                uint16_t drv_idx = static_cast<uint16_t>(slot_id) * 16 + i;
                publish_bound_driver(drv_idx, drv);
                sched::enqueue(t);

                log::info("usb: bound %s to interface %u (class=0x%02x)",
                           drv->name(), iface->interface_number, iface->interface_class);

                break;
            }
        }
    });
}

void device_disconnected(drivers::xhci_hcd*,
                         drivers::xhci::xhci_device* xdev) {
    if (!xdev) {
        return;
    }

    auto* dev = xdev->core_device();
    if (!dev) {
        return;
    }

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(dev->lifetime_lock);
        dev->disconnect_pending = true;
        sync::spin_unlock_irqrestore(dev->lifetime_lock, irq);
    });

    // Notify bound class drivers
    uint8_t slot_id = xdev->slot_id();
    for (uint8_t i = 0; i < dev->num_interfaces; i++) {
        uint16_t drv_idx = static_cast<uint16_t>(slot_id) * 16 + i;
        auto* drv = take_bound_driver_for_device(drv_idx, dev);
        if (!drv) {
            continue;
        }

        drv->disconnect();
    }
}

void finalize_disconnected_device(drivers::xhci_hcd* hcd,
                                  drivers::xhci::xhci_device* xdev) {
    if (!xdev) {
        return;
    }

    auto* dev = xdev->core_device();
    if (!dev) {
        xdev->set_core_device(nullptr);
        if (hcd) {
            hcd->release_disconnected_device(xdev);
        }
        return;
    }

    finalization_ticket ticket = {};
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(dev->lifetime_lock);
        dev->hcd_teardown_complete = true;
        ticket = claim_finalization_if_ready_locked(dev);
        sync::spin_unlock_irqrestore(dev->lifetime_lock, irq);
    });
    finish_claimed_finalization(ticket);
}

} // namespace usb::core
