#include "net/interface.h"
#include "net/packet.h"
#include "net/eth.h"
#include "sync/atomic.h"
#include "sync/spinlock.h"
#include "common/string.h"

namespace net {

// IDs start at 1 since 0 means "no interface"
static sync::atomic<uint64_t> g_next_interface_id {1};

// Slots are filled once and never removed. The count is published with a
// release store so readers can index below it without taking the lock.
static sync::spinlock g_registry_lock = sync::SPINLOCK_INIT;
static interface* g_interfaces[MAX_INTERFACES];
static sync::atomic<size_t> g_interface_count {0};

static uint64_t generate_interface_id() {
    return g_next_interface_id.fetch_add_relaxed(1);
}

// Writes `value` in decimal at `pos`, returns the new position or `cap` when
// the buffer is too small.
static size_t append_decimal(char* buf, size_t cap, size_t pos, size_t value) {
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);

    if (pos + count >= cap) {
        return cap;
    }

    while (count > 0) {
        buf[pos++] = digits[--count];
    }

    return pos;
}

// Interfaces already registered under `prefix`, locked by the caller
static size_t count_prefix(const char* prefix, size_t prefix_len) {
    size_t matches = 0;
    size_t total = g_interface_count.load_relaxed();
    for (size_t i = 0; i < total; i++) {
        if (string::strncmp(g_interfaces[i]->name(), prefix, prefix_len) == 0) {
            matches++;
        }
    }
    return matches;
}

interface::interface()
    : m_id(generate_interface_id())
    , m_enabled(false)
    , m_loopback(false)
    , m_name{}
    , m_counters{}
    , m_mac{}
    , m_mtu(0)
    , m_ipv4_conf{} {}

int32_t interface::receive(packet* pkt) {
    if (!pkt) {
        return ERR_INVALID;
    }

    if (!m_enabled) {
        record_packet_dropped();
        packet::free(pkt);
        return ERR_DOWN;
    }

    m_counters.frames_in++;
    m_counters.bytes_in += pkt->length();

    // Every interface is an Ethernet interface, so the link layer
    // above is always Ethernet and it takes ownership from here.
    return eth::input(pkt);
}

void interface::set_name(const char* name) {
    size_t len = string::strlen(name);
    if (len >= IFACE_NAME_MAX) {
        len = IFACE_NAME_MAX - 1;
    }

    string::memcpy(m_name, name, len);
    m_name[len] = '\0';
}

int32_t register_interface(interface* iface, const char* prefix) {
    if (!iface || !prefix) {
        return ERR_INVALID;
    }

    size_t prefix_len = string::strlen(prefix);
    if (prefix_len == 0 || prefix_len >= IFACE_NAME_MAX) {
        return ERR_INVALID;
    }

    sync::lock_guard guard(g_registry_lock);

    size_t count = g_interface_count.load_relaxed();
    if (count >= MAX_INTERFACES) {
        return ERR_FULL;
    }

    char name[IFACE_NAME_MAX];
    string::memcpy(name, prefix, prefix_len);

    size_t end = append_decimal(name, IFACE_NAME_MAX, prefix_len, count_prefix(prefix, prefix_len));
    if (end >= IFACE_NAME_MAX) {
        return ERR_INVALID;
    }

    name[end] = '\0';
    iface->set_name(name);

    g_interfaces[count] = iface;
    g_interface_count.store_release(count + 1);
    return OK;
}

size_t interface_count() {
    return g_interface_count.load_acquire();
}

interface* interface_at(size_t index) {
    if (index >= g_interface_count.load_acquire()) {
        return nullptr;
    }

    return g_interfaces[index];
}

interface* find_interface_by_address(const ipv4::ipv4_addr& addr) {
    size_t count = g_interface_count.load_acquire();
    for (size_t i = 0; i < count; i++) {
        ipv4::ipv4_config conf = g_interfaces[i]->ipv4_conf();

        if (conf.configured() && conf.address == addr) {
            return g_interfaces[i];
        }
    }

    return nullptr;
}

interface* find_loopback_interface() {
    size_t count = g_interface_count.load_acquire();
    for (size_t i = 0; i < count; i++) {
        if (g_interfaces[i]->is_loopback()) {
            return g_interfaces[i];
        }
    }

    return nullptr;
}

} // namespace net
