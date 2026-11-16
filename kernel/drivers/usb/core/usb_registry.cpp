#include "drivers/usb/core/usb_registry.h"
#include "drivers/usb/core/usb_driver.h"
#include "drivers/usb/core/usb_identity.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "fs/devfs/devfs.h"
#include "mm/heap.h"

namespace usb::core {

using devfs::append_str;
using devfs::append_u64;
using registered_devices = list::head<usb::device, &usb::device::registry_link>;

static const char HEX_DIGITS[] = "0123456789abcdef";
static constexpr size_t MAX_HEX_DIGITS = 4;

static const char MISSING_VALUE[] = "-";

static sync::spinlock            g_registry_lock = sync::SPINLOCK_INIT;
static registered_devices        g_devices;
static devfs::generation_counter g_generation; // goes up by one as a device is listed or unlisted

// By controller, then port by port from the root port, so a hub comes right before the devices behind it
static bool is_located_before(const usb::device* a, const usb::device* b) {
    if (a->controller_number != b->controller_number) {
        return a->controller_number < b->controller_number;
    }

    uint8_t shared_length = a->port_path_length < b->port_path_length ? a->port_path_length : b->port_path_length;
    for (uint8_t i = 0; i < shared_length; i++) {
        if (a->port_path[i] != b->port_path[i]) {
            return a->port_path[i] < b->port_path[i];
        }
    }

    return a->port_path_length < b->port_path_length;
}

static size_t append_hex(char* buf, size_t cap, size_t pos, uint32_t value, size_t digits) {
    char text[MAX_HEX_DIGITS + 1];
    for (size_t i = 0; i < digits; i++) {
        text[i] = HEX_DIGITS[(value >> (4 * (digits - 1 - i))) & 0xF];
    }

    text[digits] = '\0';
    return append_str(buf, cap, pos, text);
}

// A class, subclass and protocol, such as 02/02/01 for a CDC-ACM control interface
static size_t append_class(char* buf, size_t cap, size_t pos, uint8_t usb_class, uint8_t subclass, uint8_t protocol) {
    pos = append_hex(buf, cap, pos, usb_class, 2);
    pos = append_str(buf, cap, pos, "/");
    pos = append_hex(buf, cap, pos, subclass, 2);
    pos = append_str(buf, cap, pos, "/");
    return append_hex(buf, cap, pos, protocol, 2);
}

// Each interface as its number, class and driver, such as 0:03/01/01:usb-hid for a keyboard, with a dash for an
// interface no driver took
static size_t append_interfaces(char* buf, size_t cap, size_t pos, const usb::device& dev) {
    if (dev.num_interfaces == 0) {
        return append_str(buf, cap, pos, MISSING_VALUE);
    }

    for (uint8_t i = 0; i < dev.num_interfaces; i++) {
        if (i > 0) {
            pos = append_str(buf, cap, pos, ",");
        }

        const usb::interface& iface = dev.interfaces[i];
        pos = append_u64(buf, cap, pos, iface.interface_number);
        pos = append_str(buf, cap, pos, ":");
        pos = append_class(buf, cap, pos, iface.interface_class, iface.interface_subclass, iface.interface_protocol);

        const usb::class_driver* drv = dev.bound_drivers[i];
        pos = append_str(buf, cap, pos, ":");
        pos = append_str(buf, cap, pos, drv ? drv->name() : MISSING_VALUE);
    }

    return pos;
}

static size_t append_string_field(char* buf, size_t cap, size_t pos, const char* text) {
    return append_str(buf, cap, pos, text[0] == '\0' ? MISSING_VALUE : text);
}

// Fields are only ever added at the end of a line, since readers skip the fields past those they know
static size_t append_device_line(char* buf, size_t cap, size_t pos, const usb::device& dev) {
    char location[LOCATION_TEXT_BYTES];
    format_location(dev, location);
    pos = append_str(buf, cap, pos, location);

    pos = append_str(buf, cap, pos, "\t");
    pos = append_hex(buf, cap, pos, dev.vid, 4);
    pos = append_str(buf, cap, pos, ":");
    pos = append_hex(buf, cap, pos, dev.pid, 4);

    pos = append_str(buf, cap, pos, "\t");
    pos = append_class(buf, cap, pos, dev.device_class, dev.device_subclass, dev.device_protocol);
    pos = append_str(buf, cap, pos, "\t");
    pos = append_interfaces(buf, cap, pos, dev);

    pos = append_str(buf, cap, pos, "\t");
    pos = append_string_field(buf, cap, pos, dev.serial_number);
    pos = append_str(buf, cap, pos, "\t");
    pos = append_string_field(buf, cap, pos, dev.manufacturer);
    pos = append_str(buf, cap, pos, "\t");
    pos = append_string_field(buf, cap, pos, dev.product);

    pos = append_str(buf, cap, pos, "\t");
    pos = append_u64(buf, cap, pos, dev.arrival);
    return append_str(buf, cap, pos, "\n");
}

/**
 * Writes a line per listed device. The registry's lock keeps each listed device allocated while its line is written,
 * and the device's lifetime lock keeps its drivers from being freed while their names are copied.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static size_t generate_device_listing(char* buf, size_t cap) {
    size_t pos = 0;
    sync::irq_state irq = sync::spin_lock_irqsave(g_registry_lock);
    for (usb::device& dev : g_devices) {
        sync::irq_state device_irq = sync::spin_lock_irqsave(dev.lifetime_lock);
        pos = append_device_line(buf, cap, pos, dev);
        sync::spin_unlock_irqrestore(dev.lifetime_lock, device_irq);
    }

    sync::spin_unlock_irqrestore(g_registry_lock, irq);

    return pos;
}

__PRIVILEGED_CODE int32_t init_registry() {
    g_devices.init();
    g_generation.init();

    fs::node* dir = devfs::ensure_dir("sysinfo");
    if (!dir) {
        log::error("usb: failed to create /dev/sysinfo");
        return ERR;
    }

    auto* node = heap::kalloc_new<devfs::text_snapshot_node>("usb", generate_device_listing, &g_generation);
    if (!node) {
        log::error("usb: failed to allocate /dev/sysinfo/usb");
        return ERR;
    }

    if (devfs::add_char_device_at(dir, node) != devfs::OK) {
        log::error("usb: failed to add /dev/sysinfo/usb");
        heap::kfree_delete(node);
        return ERR;
    }

    return OK;
}

void list_device(usb::device* dev) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_registry_lock);
        g_devices.insert_sorted(dev, is_located_before);
        dev->arrival = g_generation.advance();
        sync::spin_unlock_irqrestore(g_registry_lock, irq);
    });
}

void unlist_device(usb::device* dev) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(g_registry_lock);
        g_devices.remove(dev);
        (void)g_generation.advance();
        sync::spin_unlock_irqrestore(g_registry_lock, irq);
    });
}

} // namespace usb::core
