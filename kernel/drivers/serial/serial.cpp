#include "drivers/serial/serial.h"
#include "drivers/serial/port.h"
#include "fs/devfs/devfs.h"
#include "fs/node.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

namespace serial {

namespace {

// The names registered ports hold, so that no two ports hold the same one
struct registry {
    sync::spinlock lock;
    held_name_list names;
};

} // namespace

static registry g_registry;
__PRIVILEGED_BSS static fs::node* g_serial_dir;

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
 * Drops the registry's reference to the port's node, which frees the port with the node once no file holds it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void release_registry_reference(port* p) {
    fs::node* node = p->node();
    if (node->release()) {
        fs::node::ref_destroy(node);
    }
}

__PRIVILEGED_CODE int32_t init() {
    g_registry.lock = sync::SPINLOCK_INIT;
    g_registry.names.init();

    g_serial_dir = devfs::ensure_dir("serial");
    return g_serial_dir ? OK : ERR_NO_MEMORY;
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
        (void)devfs::remove_char_device(p->node());
        release_name(p);
    });
}

void unregister_port(port* p) {
    log::info("serial: unregistered the port that held /dev/serial/%s", p->name());
    RUN_ELEVATED(release_registry_reference(p));
}

} // namespace serial
