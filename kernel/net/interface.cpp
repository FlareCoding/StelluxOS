#include "net/interface.h"
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

    // TODO: deliver the frame to the layer above
    return OK;
}

} // namespace net
