#ifndef STELLUX_NET_INTERFACE_H
#define STELLUX_NET_INTERFACE_H

#include "common/types.h"

namespace net {

class packet;

constexpr int32_t OK            = 0;
constexpr int32_t ERR_INVALID   = -1; // null packet or empty frame
constexpr int32_t ERR_BUSY      = -2; // no transmit slot is free
constexpr int32_t ERR_TOO_LARGE = -3; // frame does not fit in one link transmission

constexpr size_t MAC_ADDR_LEN   = 6;
constexpr size_t IFACE_NAME_MAX = 16;

struct iface_counters {
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t drops;  // frames discarded by policy, such as a full ring
    uint64_t errors; // frames the hardware or the stack could not process
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
     * @return OK when the frame was accepted, ERR_INVALID for a null packet.
     */
    int32_t receive(packet* pkt);

protected:
    uint64_t        m_id;      // Nonzero and unique for the life of the kernel, 0 means no interface
    bool            m_enabled; // Administratively up, checked by the stack before frames move either way
    char            m_name[IFACE_NAME_MAX];
    iface_counters  m_counters;

    // Link layer identity, filled in by the driver once the hardware reports it
    uint8_t         m_mac[MAC_ADDR_LEN];
    uint16_t        m_mtu; // Largest payload carried in one frame, excluding the link header
};

} // namespace net

#endif // STELLUX_NET_INTERFACE_H
