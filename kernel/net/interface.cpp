#include "net/interface.h"
#include "net/packet.h"
#include "sync/atomic.h"
#include "common/logging.h"

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
        m_counters.drops++;
        packet::free(pkt);
        return ERR_DOWN;
    }

    m_counters.frames_in++;
    m_counters.bytes_in += pkt->length();

    // Debug logging
    {
        static const char HEX[] = "0123456789abcdef";
        const uint8_t* bytes = pkt->data();
        size_t len = pkt->length();

        log::info("%s: received %lu bytes", m_name, len);

        for (size_t off = 0; off < len; off += 16) {
            char line[3 * 16 + 1];
            size_t pos = 0;

            for (size_t i = off; i < off + 16 && i < len; i++) {
                line[pos++] = HEX[bytes[i] >> 4];
                line[pos++] = HEX[bytes[i] & 0x0F];
                line[pos++] = ' ';
            }

            line[pos] = '\0';
            log::info("  %04lx: %s", off, line);
        }
    }

    // At this point, the network stack is done
    // processing the packet so we can safely free it.
    packet::free(pkt);

    return OK;
}

} // namespace net
