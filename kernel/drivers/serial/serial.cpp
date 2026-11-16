#include "drivers/serial/serial.h"
#include "drivers/serial/port.h"
#include "fs/devfs/devfs.h"
#include "fs/node.h"
#include "mm/heap.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

namespace serial {

namespace {

// The names registered ports hold, so that no two ports hold the same one
struct registry {
    sync::spinlock            lock;
    held_name_list            names;
    port_list                 ports;      // the ports the listing shows
    devfs::generation_counter generation; // goes up by one as a port is listed or unlisted
};

} // namespace

static registry g_registry;
__PRIVILEGED_BSS static fs::node* g_serial_dir;

static constexpr char        FIELD_SEPARATOR = '\t';
static constexpr const char* MISSING_VALUE = "-";
static constexpr const char* HEX_DIGITS = "0123456789abcdef";

// A device's control characters are listed as spaces, so its strings cannot end a field or a line
static constexpr uint8_t FIRST_PRINTABLE_CHARACTER = 0x20;
static constexpr uint8_t DELETE_CHARACTER = 0x7F;

// The 20 digits of the largest uint64_t and the NUL
static constexpr size_t NUMBER_TEXT_BYTES = 21;

// "vvvv:pppp" and the NUL
static constexpr size_t USB_IDS_TEXT_BYTES = 10;

// A channel's letter and the NUL
static constexpr size_t CHANNEL_TEXT_BYTES = 2;

/**
 * Names `p` and adds its name to the registry under one hold of the lock, false when no name is free.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool reserve_name(port* p) {
    sync::irq_state irq = sync::spin_lock_irqsave(g_registry.lock);
    bool is_named = choose_port_name(p->id(), g_registry.names, p->name_record());
    if (is_named) {
        g_registry.names.push_back(&p->name_record());
    }

    sync::spin_unlock_irqrestore(g_registry.lock, irq);

    return is_named;
}

/**
 * Frees the name `p` holds, so the next board may take it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void release_name(port* p) {
    sync::irq_state irq = sync::spin_lock_irqsave(g_registry.lock);
    g_registry.names.remove(&p->name_record());
    sync::spin_unlock_irqrestore(g_registry.lock, irq);
}

/**
 * Adds `p` to the listing, wakes the listing's pollers and records the new generation as the port's arrival.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void list_port(port* p) {
    sync::irq_state irq = sync::spin_lock_irqsave(g_registry.lock);
    g_registry.ports.push_back(p);
    p->set_arrival(g_registry.generation.advance());
    sync::spin_unlock_irqrestore(g_registry.lock, irq);
}

/**
 * Takes `p` out of the listing and wakes the listing's pollers.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void unlist_port(port* p) {
    sync::irq_state irq = sync::spin_lock_irqsave(g_registry.lock);
    g_registry.ports.remove(p);
    (void)g_registry.generation.advance();
    sync::spin_unlock_irqrestore(g_registry.lock, irq);
}

/**
 * Drops the registry's reference to the port's node, which frees the port with the node once no file holds it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void release_registry_reference(port* p) {
    fs::node* node = p->node();
    if (node->release()) {
        fs::node::ref_destroy(node);
    }
}

static size_t append_char(char* buf, size_t cap, size_t pos, char c) {
    if (pos < cap) {
        buf[pos] = c;
    }

    return pos + 1;
}

// Appends `text` as a field, or MISSING_VALUE when it is empty
static size_t append_field(char* buf, size_t cap, size_t pos, const char* text) {
    if (text[0] == '\0') {
        return devfs::append_str(buf, cap, pos, MISSING_VALUE);
    }

    for (const char* c = text; *c != '\0'; c++) {
        auto byte = static_cast<uint8_t>(*c);
        bool is_control = byte < FIRST_PRINTABLE_CHARACTER || byte == DELETE_CHARACTER;
        pos = append_char(buf, cap, pos, is_control ? ' ' : *c);
    }

    return pos;
}

static size_t append_hex16(char* buf, size_t cap, size_t pos, uint16_t value) {
    for (int shift = 12; shift >= 0; shift -= 4) {
        pos = append_char(buf, cap, pos, HEX_DIGITS[(value >> shift) & 0xF]);
    }

    return pos;
}

static void format_number(uint64_t value, char (&text)[NUMBER_TEXT_BYTES]) {
    size_t length = string::format_u64(text, sizeof(text) - 1, value);
    text[length] = '\0';
}

// The USB vendor and product IDs, such as "0403:6001", or "" for a port not on USB
static void format_usb_ids(const identity& id, char (&text)[USB_IDS_TEXT_BYTES]) {
    size_t length = 0;
    if (id.vid != 0) {
        length = append_hex16(text, sizeof(text), length, id.vid);
        length = append_char(text, sizeof(text), length, ':');
        length = append_hex16(text, sizeof(text), length, id.pid);
    }

    text[length] = '\0';
}

// The letter the port's name ends in, or "" on a device with one port
static void format_channel(int8_t channel, char (&text)[CHANNEL_TEXT_BYTES]) {
    text[0] = channel < 0 ? '\0' : static_cast<char>('a' + channel);
    text[1] = '\0';
}

static size_t append_port_line(char* buf, size_t cap, size_t pos, const port& p) {
    char usb_ids[USB_IDS_TEXT_BYTES];
    char arrival[NUMBER_TEXT_BYTES];
    char received[NUMBER_TEXT_BYTES];
    char dropped[NUMBER_TEXT_BYTES];
    char channel[CHANNEL_TEXT_BYTES];

    const identity& id = p.id();
    format_usb_ids(id, usb_ids);
    format_number(p.arrival(), arrival);
    format_channel(id.channel, channel);

    // Dropped is read before received, so a line never shows more bytes dropped than received
    format_number(p.dropped_bytes(), dropped);
    format_number(p.received_bytes(), received);

    // Fields are only ever added at the end of a line, since readers skip the fields past those they know
    const char* fields[] = {
        p.name(), id.bus, id.driver, usb_ids, id.location, id.serial_number, id.manufacturer, id.product, arrival,
        received, dropped, channel,
    };

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (i > 0) {
            pos = append_char(buf, cap, pos, FIELD_SEPARATOR);
        }

        pos = append_field(buf, cap, pos, fields[i]);
    }

    return append_char(buf, cap, pos, '\n');
}

/**
 * Writes a line per listed port under the registry's lock, which also keeps each listed port alive meanwhile.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static size_t generate_listing(char* buf, size_t cap) {
    size_t pos = 0;
    sync::irq_state irq = sync::spin_lock_irqsave(g_registry.lock);
    for (const port& p : g_registry.ports) {
        pos = append_port_line(buf, cap, pos, p);
    }

    sync::spin_unlock_irqrestore(g_registry.lock, irq);

    return pos;
}

__PRIVILEGED_CODE int32_t init() {
    g_registry.lock = sync::SPINLOCK_INIT;
    g_registry.names.init();
    g_registry.ports.init();
    g_registry.generation.init();

    g_serial_dir = devfs::ensure_dir("serial");
    if (!g_serial_dir) {
        return ERR_NO_MEMORY;
    }

    fs::node* sysinfo_dir = devfs::ensure_dir("sysinfo");
    auto* listing = heap::kalloc_new<devfs::text_snapshot_node>("serial", generate_listing, &g_registry.generation);
    if (!sysinfo_dir || !listing || devfs::add_char_device_at(sysinfo_dir, listing) != devfs::OK) {
        log::error("serial: could not add /dev/sysinfo/serial");
        heap::kfree_delete(listing);
    }

    return OK;
}

port* register_port(const identity& id) {
    port* p = nullptr;
    bool is_named = false;
    RUN_ELEVATED({
        p = g_serial_dir ? port::create(id) : nullptr;
        is_named = p && reserve_name(p);
    });

    if (!p) {
        log::error("serial: could not create a port for chip %s", id.chip);
        return nullptr;
    }

    if (!is_named) {
        log::error("serial: no free name fits a port for chip %s", id.chip);
        RUN_ELEVATED(release_registry_reference(p));
        return nullptr;
    }

    // A name is held before its node is added, since devfs adds a node without checking whether its name is taken
    bool is_added = false;
    RUN_ELEVATED({
        fs::node* node = p->node();
        node->set_name(p->name(), string::strlen(p->name()));
        is_added = devfs::add_char_device_at(g_serial_dir, node) == devfs::OK;
    });

    if (!is_added) {
        log::error("serial: could not add /dev/serial/%s", p->name());
        RUN_ELEVATED({
            release_name(p);
            release_registry_reference(p);
        });
        return nullptr;
    }

    // Listed once its file exists, so a program the listing wakes finds the file
    RUN_ELEVATED(list_port(p));
    log::info("serial: added /dev/serial/%s", p->name());
    return p;
}

void receive(port* p, const uint8_t* data, size_t len) {
    RUN_ELEVATED(p->queue_received(data, len));
}

void receive_failed(port* p) {
    RUN_ELEVATED(p->mark_failed());
}

void detach(port* p) {
    RUN_ELEVATED({
        p->mark_gone();
        unlist_port(p);
        (void)devfs::remove_char_device(p->node());
        release_name(p);
    });
}

void unregister_port(port* p) {
    log::info("serial: unregistered the port that held /dev/serial/%s", p->name());
    RUN_ELEVATED(release_registry_reference(p));
}

} // namespace serial
