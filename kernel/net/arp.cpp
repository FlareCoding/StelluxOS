#include "net/arp.h"
#include "net/packet.h"
#include "net/interface.h"
#include "clock/clock.h"
#include "common/logging.h"

namespace net {
namespace arp {

static arp_table g_table;

static int32_t drop(interface* iface, packet* pkt, int32_t rc) {
    iface->record_packet_dropped();
    packet::free(pkt);
    return rc;
}

static int32_t reject(interface* iface, packet* pkt, int32_t rc) {
    iface->record_iface_error();
    packet::free(pkt);
    return rc;
}

static void move_all(packet_list& from, packet_list& to) {
    while (packet* pkt = from.pop_front()) {
        to.push_back(pkt);
    }
}

static void drop_all(packet_list& list) {
    while (packet* pkt = list.pop_front()) {
        pkt->iface()->record_packet_dropped();
        packet::free(pkt);
    }
}

// Broadcasts a request for `ip` from `iface`. Requests exist only
// for pending table entries, so this stays private to the module.
static int32_t send_arp_request(interface* iface, const ipv4::ipv4_addr& ip) {
    packet* pkt = packet::alloc();
    if (!pkt) {
        return ERR_NO_MEMORY;
    }

    // Leave room for the link header in front of the payload
    uint8_t* body = pkt->reserve(eth::HEADER_LEN) ? pkt->put(HEADER_LEN) : nullptr;
    if (!body) {
        packet::free(pkt);
        return ERR_INVALID;
    }

    arp_header* hdr = reinterpret_cast<arp_header*>(body);
    hdr->hw_type = htons(HW_TYPE_ETHERNET);
    hdr->proto_type = htons(PROTO_TYPE_IPV4);
    hdr->hw_len = eth::MAC_ADDR_LEN;
    hdr->proto_len = ipv4::ADDR_LEN;
    hdr->opcode = htons(OP_REQUEST);
    hdr->sender_hw_addr = iface->mac();
    hdr->sender_proto_addr = iface->ipv4_conf().address;
    hdr->target_hw_addr = {};
    hdr->target_proto_addr = ip;

    pkt->set_iface(iface);
    return output(pkt, eth::BROADCAST_ADDR);
}

void arp_table::init() {
    m_lock = sync::SPINLOCK_INIT;
    for (size_t i = 0; i < TABLE_SIZE; i++) {
        m_entries[i].queue.init();
        clear_entry(m_entries[i]);
    }
}

void arp_table::clear_entry(arp_entry& entry) {
    entry.state = arp_entry_state::empty;
    entry.iface = nullptr;
    entry.attempts = 0;
    entry.draining = false;
    entry.ip = {};
    entry.mac = {};
    entry.timestamp = 0;
}

arp_entry* arp_table::find_entry(interface* iface, const ipv4::ipv4_addr& ip) {
    for (size_t i = 0; i < TABLE_SIZE; i++) {
        arp_entry& entry = m_entries[i];
        if (entry.state != arp_entry_state::empty && entry.iface == iface && entry.ip == ip) {
            return &entry;
        }
    }
    return nullptr;
}

// An empty slot or the oldest resolved entry, never a pending one, so taking
// it costs no queued packets. Returns nullptr when every entry is pending.
arp_entry* arp_table::take_free_entry() {
    arp_entry* oldest_resolved = nullptr;

    for (size_t i = 0; i < TABLE_SIZE; i++) {
        arp_entry& entry = m_entries[i];
        if (entry.state == arp_entry_state::empty) {
            return &entry;
        }

        if (entry.state == arp_entry_state::resolved &&
            (!oldest_resolved || entry.timestamp < oldest_resolved->timestamp)) {
            oldest_resolved = &entry;
        }
    }

    if (oldest_resolved) {
        clear_entry(*oldest_resolved);
    }

    return oldest_resolved;
}

// Falls back to evicting the oldest pending entry, handing its packets to `dropped`
arp_entry* arp_table::allocate_entry(packet_list& dropped) {
    arp_entry* entry = take_free_entry();
    if (entry) {
        return entry;
    }

    arp_entry* oldest_pending = &m_entries[0];
    for (size_t i = 1; i < TABLE_SIZE; i++) {
        if (m_entries[i].timestamp < oldest_pending->timestamp) {
            oldest_pending = &m_entries[i];
        }
    }

    move_all(oldest_pending->queue, dropped);
    clear_entry(*oldest_pending);
    return oldest_pending;
}

arp_send_action arp_table::resolve(
    interface* iface,
    const ipv4::ipv4_addr& ip,
    packet* pkt,
    uint64_t timestamp,
    eth::mac_addr* out_mac,
    bool* send_request,
    packet_list& dropped
) {
    sync::lock_guard guard(m_lock);
    *send_request = false;

    // If we have a resolved entry for this IP, we can use it to send the packet.
    arp_entry* entry = find_entry(iface, ip);
    if (entry && entry->state == arp_entry_state::resolved) {
        *out_mac = entry->mac;
        return arp_send_action::transmit;
    }

    // Unknown address: open a pending entry and have the caller ask for it.
    // A pending hit means a request is already in flight, the sweep retries it.
    if (!entry) {
        entry = allocate_entry(dropped);
        entry->state = arp_entry_state::pending;
        entry->iface = iface;
        entry->ip = ip;
        entry->attempts = 1;
        entry->timestamp = timestamp;
        *send_request = true;
    }

    // The oldest waiting packet gives way, the newest is the most likely still wanted
    if (pkt) {
        if (entry->queue.size() >= PENDING_QUEUE_DEPTH) {
            dropped.push_back(entry->queue.pop_front());
        }

        entry->queue.push_back(pkt);
    }

    return arp_send_action::queued;
}

void arp_table::sweep(
    uint64_t timestamp,
    packet_list& dropped,
    arp_retry* retries,
    size_t* retry_count
) {
    sync::lock_guard guard(m_lock);
    *retry_count = 0;

    for (size_t i = 0; i < TABLE_SIZE; i++) {
        arp_entry& entry = m_entries[i];
        if (entry.state == arp_entry_state::empty) {
            continue;
        }

        uint64_t age = timestamp - entry.timestamp;
        if (entry.state == arp_entry_state::resolved) {
            if (age >= ENTRY_LIFETIME_NS) {
                clear_entry(entry);
            }

            continue;
        }

        if (age < REQUEST_RETRY_NS) {
            continue;
        }

        // Pending with no reply for a full retry interval: ask again,
        // or give up and release the packets that waited on it.
        if (entry.attempts >= MAX_REQUEST_ATTEMPTS) {
            move_all(entry.queue, dropped);
            clear_entry(entry);
            continue;
        }

        entry.attempts++;
        entry.timestamp = timestamp;
        retries[*retry_count] = { entry.iface, entry.ip };
        
        (*retry_count)++;
    }
}

bool arp_table::update_entry(
    interface* iface,
    const ipv4::ipv4_addr& ip,
    const eth::mac_addr& mac,
    uint64_t timestamp,
    bool create,
    packet_list& flushed
) {
    sync::lock_guard guard(m_lock);

    arp_entry* entry = find_entry(iface, ip);
    if (!entry) {
        if (!create) {
            return false;
        }

        entry = take_free_entry();
        if (!entry) {
            return false;
        }

        entry->iface = iface;
        entry->ip = ip;
    }

    bool completed = entry->state == arp_entry_state::pending;
    entry->state = arp_entry_state::resolved;
    entry->mac = mac;
    entry->attempts = 0;
    entry->timestamp = timestamp;
    move_all(entry->queue, flushed);

    if (entry->draining) {
        clear_entry(*entry);
    }

    return completed;
}

size_t arp_table::snapshot(arp_snapshot_entry* out, size_t max, uint64_t timestamp) {
    sync::lock_guard guard(m_lock);

    size_t count = 0;
    for (size_t i = 0; i < TABLE_SIZE && count < max; i++) {
        const arp_entry& entry = m_entries[i];
        if (entry.state == arp_entry_state::empty) {
            continue;
        }

        out[count++] = { entry.iface, entry.ip, entry.mac, entry.state, timestamp - entry.timestamp };
    }

    return count;
}

void arp_table::forget(interface* iface) {
    sync::lock_guard guard(m_lock);

    for (size_t i = 0; i < TABLE_SIZE; i++) {
        arp_entry& entry = m_entries[i];
        if (entry.state == arp_entry_state::empty || entry.iface != iface) {
            continue;
        }

        if (entry.state == arp_entry_state::pending && !entry.queue.empty()) {
            entry.draining = true;
        } else {
            clear_entry(entry);
        }
    }
}

int32_t init() {
    g_table.init();
    return OK;
}

int32_t input(packet* pkt) {
    if (!pkt) {
        log::warn("arp: input called with no packet");
        return ERR_INVALID;
    }

    interface* iface = pkt->iface();
    if (!iface) {
        log::warn("arp: input called with a packet that has no interface");
        packet::free(pkt);
        return ERR_INVALID;
    }

    if (pkt->length() < HEADER_LEN) {
        return reject(iface, pkt, ERR_INVALID);
    }

    // Only Ethernet over IPv4 is supported
    arp_header* hdr = reinterpret_cast<arp_header*>(pkt->data());
    if (
        ntohs(hdr->hw_type) != HW_TYPE_ETHERNET ||
        ntohs(hdr->proto_type) != PROTO_TYPE_IPV4 ||
        hdr->hw_len != eth::MAC_ADDR_LEN ||
        hdr->proto_len != ipv4::ADDR_LEN
    ) {
        return drop(iface, pkt, OK);
    }

    // Without an address nothing is answered and only pending entries can complete
    ipv4::ipv4_config conf = iface->ipv4_conf();
    bool is_for_local_ip = conf.configured() && hdr->target_proto_addr == conf.address;
    bool is_request = ntohs(hdr->opcode) == OP_REQUEST;

    // Never learn from address probes, group addresses, or our own address
    bool learnable_sender =
        !hdr->sender_proto_addr.is_unspecified() &&
        !hdr->sender_hw_addr.is_multicast() &&
        hdr->sender_proto_addr != conf.address;

    if (learnable_sender) {
        packet_list flushed;
        flushed.init();

        // Only requests addressed to this host may create entries
        bool completed = g_table.update_entry(
            iface,
            hdr->sender_proto_addr,
            hdr->sender_hw_addr,
            clock::now_ns(),
            is_for_local_ip && is_request, // prevent ARP poisoning
            flushed
        );

        if (completed) {
            const uint8_t* ip = hdr->sender_proto_addr.bytes;
            const uint8_t* mac = hdr->sender_hw_addr.bytes;
            log::info("arp: %u.%u.%u.%u is at %02x:%02x:%02x:%02x:%02x:%02x",
                      ip[0], ip[1], ip[2], ip[3], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }

        // Packets that waited on this address can leave now that the lock is released
        while (packet* waiting = flushed.pop_front()) {
            eth::output(waiting, hdr->sender_hw_addr, eth::TYPE_IPV4);
        }
    }

    // Requests for other hosts and replies are normal traffic, not drops
    if (!is_for_local_ip || !is_request) {
        packet::free(pkt);
        return OK;
    }

    // RFC 826: the request becomes the reply in place, sender and target swapped
    hdr->target_hw_addr = hdr->sender_hw_addr;
    hdr->target_proto_addr = hdr->sender_proto_addr;
    hdr->sender_hw_addr = iface->mac();
    hdr->sender_proto_addr = conf.address;
    hdr->opcode = htons(OP_REPLY);

    // Strip link padding so it is not sent back as payload
    pkt->trim(HEADER_LEN);

    return output(pkt, hdr->target_hw_addr);
}

int32_t output(packet* pkt, const eth::mac_addr& dest) {
    return eth::output(pkt, dest, eth::TYPE_ARP);
}

int32_t resolve(interface* iface, const ipv4::ipv4_addr& ip, eth::mac_addr* out) {
    if (!iface || !out) {
        return ERR_INVALID;
    }

    packet_list dropped;
    dropped.init();

    bool request = false;

    arp_send_action action = g_table.resolve(
        iface,
        ip,
        nullptr,
        clock::now_ns(),
        out,
        &request,
        dropped
    );

    drop_all(dropped);

    // A lost request is retried by the sweep, so its result does not change the answer
    if (request) {
        send_arp_request(iface, ip);
    }

    return action == arp_send_action::transmit ? OK : ERR_PENDING;
}

int32_t resolve_and_send(packet* pkt, const ipv4::ipv4_addr& next_hop) {
    if (!pkt) {
        log::warn("arp: resolve_and_send called with no packet");
        return ERR_INVALID;
    }

    interface* iface = pkt->iface();
    if (!iface) {
        log::warn("arp: resolve_and_send called with a packet that has no interface");
        packet::free(pkt);
        return ERR_INVALID;
    }

    packet_list dropped;
    dropped.init();

    eth::mac_addr mac;
    bool request = false;

    arp_send_action action = g_table.resolve(
        iface,
        next_hop,
        pkt,
        clock::now_ns(),
        &mac,
        &request,
        dropped
    );

    drop_all(dropped);

    if (request) {
        send_arp_request(iface, next_hop);
    }

    // A queued packet belongs to the table now and leaves when the reply arrives
    if (action == arp_send_action::queued) {
        return OK;
    }

    return eth::output(pkt, mac, eth::TYPE_IPV4);
}

void sweep(uint64_t ts) {
    packet_list dropped;
    dropped.init();
    arp_retry retries[TABLE_SIZE];
    size_t retry_count = 0;

    g_table.sweep(ts, dropped, retries, &retry_count);
    drop_all(dropped);

    for (size_t i = 0; i < retry_count; i++) {
        send_arp_request(retries[i].iface, retries[i].ip);
    }
}

size_t snapshot(arp_snapshot_entry* out, size_t max) {
    if (!out) {
        return 0;
    }

    return g_table.snapshot(out, max, clock::now_ns());
}

void forget(interface* iface) {
    if (!iface) {
        return;
    }

    g_table.forget(iface);
}

} // namespace arp
} // namespace net
