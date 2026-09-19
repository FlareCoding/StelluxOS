#include "net/packet.h"
#include "mm/heap.h"

namespace net {

packet* packet::alloc() {
    void* mem = heap::uzalloc(sizeof(packet));
    if (!mem) {
        return nullptr;
    }

    return new (mem) packet();
}

void packet::free(packet* pkt) {
    if (!pkt) {
        return;
    }

    pkt->~packet();
    heap::ufree(pkt);
}

packet::packet()
    : m_iface(nullptr)
    , m_data(0)
    , m_tail(0)
    , m_link_header(HEADER_UNSET)
    , m_network_header(HEADER_UNSET)
    , m_transport_header(HEADER_UNSET) {}

bool packet::reserve(size_t n) {
    if (m_data != m_tail || n > tailroom()) {
        return false;
    }

    m_data += static_cast<uint16_t>(n);
    m_tail = m_data;
    return true;
}

uint8_t* packet::put(size_t n) {
    if (n > tailroom()) {
        return nullptr;
    }

    uint8_t* start = m_buffer + m_tail;
    m_tail += static_cast<uint16_t>(n);

    return start;
}

uint8_t* packet::push(size_t n) {
    if (n > headroom()) {
        return nullptr;
    }

    m_data -= static_cast<uint16_t>(n);
    return m_buffer + m_data;
}

uint8_t* packet::pull(size_t n) {
    if (n > length()) {
        return nullptr;
    }

    m_data += static_cast<uint16_t>(n);
    return m_buffer + m_data;
}

bool packet::trim(size_t len) {
    if (len > length()) {
        return false;
    }

    m_tail = static_cast<uint16_t>(m_data + len);
    return true;
}

} // namespace net
