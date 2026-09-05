#ifndef STELLUX_NET_PACKET_H
#define STELLUX_NET_PACKET_H

#include "common/types.h"

namespace net {

class interface;

constexpr size_t PACKET_OBJECT_SIZE   = 2048;
constexpr size_t PACKET_METADATA_SIZE = 24;
constexpr size_t PACKET_CAPACITY      = PACKET_OBJECT_SIZE - PACKET_METADATA_SIZE;

/**
 * A packet is the single buffer that carries one frame through the network
 * stack.
 * Headers are added in front of the payload on the way down and stripped from
 * the front on the way up, so the class keeps a window `[data, tail)` inside a
 * fixed buffer and every layer works only on that window.
 * A packet has exactly one owner at any moment. It must be created with `alloc`
 * and released with `free`, and it is never copied.
 * Protocol layers own their header layouts and the helpers that read and write
 * them, the packet itself only knows bytes.
 */
class packet {
public:
    /**
     * @brief Allocate an empty zeroed packet from the unprivileged heap.
     * @return The packet, or nullptr when memory is exhausted.
     */
    [[nodiscard]] static packet* alloc();

    /**
     * @brief Release a packet.
     */
    static void free(packet* pkt);

    packet(const packet&) = delete;
    packet& operator=(const packet&) = delete;

    /**
     * @brief Leave `n` bytes of headroom in front of the window.
     * Only allowed while the packet is empty, so it must be called before the
     * first `put`.
     * @return true on success, false when the packet is not empty or `n` does
     *         not fit.
     */
    bool reserve(size_t n);

    /**
     * @brief Grow the window at the end by `n` bytes.
     * @return Pointer to the first new byte, or nullptr when `n` exceeds the
     *         tailroom.
     */
    [[nodiscard]] uint8_t* put(size_t n);

    /**
     * @brief Grow the window at the front by `n` bytes, making room for a header.
     * @return Pointer to the new start of the window, or nullptr when `n` exceeds
     *         the headroom.
     */
    [[nodiscard]] uint8_t* push(size_t n);

    /**
     * @brief Shrink the window at the front by `n` bytes, stepping past a header.
     * The bytes stay in the buffer and remain reachable through the header marks.
     * @return Pointer to the new start of the window, or nullptr when `n` exceeds
     *         the window length.
     */
    [[nodiscard]] uint8_t* pull(size_t n);

    /**
     * @brief Shrink the window at the end to exactly `len` bytes, dropping link
     * padding once the header has told us the true length.
     * @return true on success, false when `len` exceeds the window length.
     */
    bool trim(size_t len);

    uint8_t* data() { return m_buffer + m_data; }
    const uint8_t* data() const { return m_buffer + m_data; }

    size_t length() const { return m_tail - m_data; }
    size_t headroom() const { return m_data; }
    size_t tailroom() const { return PACKET_CAPACITY - m_tail; }

    // Each layer marks where its header starts before pulling past it, so higher
    // layers can still reach lower headers, such as UDP reading IP addresses for
    // its checksum.
    void mark_link_header() { m_link_header = m_data; }
    void mark_network_header() { m_network_header = m_data; }
    void mark_transport_header() { m_transport_header = m_data; }

    uint8_t* link_header() { return header_at(m_link_header); }
    uint8_t* network_header() { return header_at(m_network_header); }
    uint8_t* transport_header() { return header_at(m_transport_header); }

    const uint8_t* link_header() const { return header_at(m_link_header); }
    const uint8_t* network_header() const { return header_at(m_network_header); }
    const uint8_t* transport_header() const { return header_at(m_transport_header); }

    // Interface the frame arrived on or will be transmitted through
    interface* iface() const { return m_iface; }
    void set_iface(interface* iface) { m_iface = iface; }

private:
    static constexpr uint16_t HEADER_UNSET = 0xFFFF;

    packet();
    ~packet() = default;

    uint8_t* header_at(uint16_t offset) {
        return offset == HEADER_UNSET ? nullptr : m_buffer + offset;
    }

    const uint8_t* header_at(uint16_t offset) const {
        return offset == HEADER_UNSET ? nullptr : m_buffer + offset;
    }

    interface* m_iface;

    // Window bounds and header marks are offsets into m_buffer
    uint16_t m_data;
    uint16_t m_tail;
    uint16_t m_link_header;
    uint16_t m_network_header;
    uint16_t m_transport_header;

    alignas(8) uint8_t m_buffer[PACKET_CAPACITY];
};

static_assert(sizeof(packet) == PACKET_OBJECT_SIZE);

} // namespace net

#endif // STELLUX_NET_PACKET_H
