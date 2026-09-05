#include "net/interface.h"
#include "net/packet.h"
#include "net/eth.h"
#include "sync/atomic.h"

namespace net {

// IDs start at 1 since 0 means "no interface"
static sync::atomic<uint64_t> g_next_interface_id {1};

static uint64_t generate_interface_id() {
    return g_next_interface_id.fetch_add_relaxed(1);
}

interface::interface()
    : m_id(generate_interface_id())
    , m_enabled(false)
    , m_name{}
    , m_counters{}
    , m_mac{}
    , m_mtu(0) {}

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

} // namespace net
