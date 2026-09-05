#ifndef STELLUX_NET_INTERFACE_H
#define STELLUX_NET_INTERFACE_H

#include "net/net.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "sync/atomic.h"

namespace net {

class packet;

constexpr size_t IFACE_NAME_MAX = 16;

struct iface_counters {
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
    sync::atomic<uint64_t> drops;  // frames discarded by policy, such as a full ring
    sync::atomic<uint64_t> errors; // frames the hardware or the stack could not process
};

/**
 * A network interface is a connection point between a network driver and the rest
 * of the network stack. A network driver would derive from this class.
 * `transmit` must be implemented by the driver and acts as a handoff point from the
 * network stack to the driver.
 * `receive` is already implemented by the interface and serves as the driver's
 * entry point into the network stack and lets the driver push packets into it.
 */
class interface {
public:
    interface();
    virtual ~interface() = default;

    /**
     * @brief Hand a fully framed packet to the driver for transmission.
     * The implementation must copy the frame into device memory before returning,
     * and the caller still owns the packet afterwards.
     * @param pkt Frame to send.
     * @return OK on success, ERR_BUSY when no transmit slot is free, ERR_TOO_LARGE
     *         when the frame exceeds the link limit, ERR_INVALID for a null or
     *         empty packet.
     */
    virtual int32_t transmit(packet* pkt) = 0;

    /**
     * @brief Entry point for the network driver to push a received frame into
     * the network stack. The interface takes ownership of the packet, so the
     * driver must not touch it again after this call.
     * @param pkt Received frame.
     * @return ERR_INVALID for a null packet, ERR_DOWN when the interface is
     *         disabled and the frame was dropped, otherwise the result of the
     *         link layer.
     */
    int32_t receive(packet* pkt);

    uint64_t id() const { return m_id; }
    bool enabled() const { return m_enabled; }

    const eth::mac_addr& mac() const { return m_mac; }
    uint16_t mtu() const { return m_mtu; }
    const ipv4::ipv4_config& ipv4_conf() const { return m_ipv4_conf; }

    void record_packet_dropped() { m_counters.drops.fetch_add_relaxed(1); }
    void record_iface_error() { m_counters.errors.fetch_add_relaxed(1); }

protected:
    uint64_t        m_id;      // Nonzero and unique for the life of the kernel, 0 means no interface
    bool            m_enabled; // Administratively up, checked by the stack before frames move either way
    char            m_name[IFACE_NAME_MAX];
    iface_counters  m_counters;

    // Link layer identity, filled in by the driver once the hardware reports it
    eth::mac_addr   m_mac;
    uint16_t        m_mtu; // Largest payload carried in one frame, excluding the link header

    // IPv4 identity, unspecified until configured by hand or through DHCP
    ipv4::ipv4_config m_ipv4_conf;
};

} // namespace net

#endif // STELLUX_NET_INTERFACE_H
