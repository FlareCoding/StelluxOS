#ifndef STELLUX_NET_INTERFACE_H
#define STELLUX_NET_INTERFACE_H

#include "net/net.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "sync/atomic.h"
#include "sync/seqlock.h"

namespace sync { struct poll_table; }

namespace net {

class packet;

constexpr size_t IFACE_NAME_MAX = 16;
constexpr size_t MAX_INTERFACES = 8;

struct iface_counters {
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
    sync::atomic<uint64_t> drops;  // frames discarded by policy, such as a full ring
    sync::atomic<uint64_t> errors; // frames the hardware or the stack could not process
};

/*
 * Every change to what the status query reports, an interface appearing, its
 * carrier or its IPv4 identity, moves the status generation and wakes watchers.
 */
int32_t init_status_watch();
uint64_t status_generation();

/**
 * @brief Moves the status generation and wakes every watcher.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void note_status_change();

/**
 * @brief Subscribes `pt` to the next status change.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void watch_status(sync::poll_table& pt);

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
    const char* name() const { return m_name; }

    bool enabled() const { return m_enabled; }
    bool link_up() const { return m_link_up.load_acquire(); }
    bool is_loopback() const { return m_loopback; }

    const eth::mac_addr& mac() const { return m_mac; }
    uint16_t mtu() const { return m_mtu; }
    ipv4::ipv4_config ipv4_conf() const { return m_ipv4_conf.read(); }

    /**
     * @brief Gives the interface a new IPv4 identity and forgets the neighbors
     * learned under the old one. Loopback addresses belong to loopback alone.
     * @return OK, or ERR_INVALID when `conf` does not describe a host on a subnet.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE int32_t configure_ipv4(const ipv4::ipv4_config& conf);

    /**
     * @brief Clears the IPv4 identity, the interface then handles no network traffic.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void unconfigure_ipv4();

    /**
     * @brief Records what the driver learned about the carrier, a change is
     * reported to status watchers.
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void set_link_up(bool up);

    // Assigned by the registry, truncated to IFACE_NAME_MAX
    void set_name(const char* name);

    void record_packet_dropped() { m_counters.drops.fetch_add_relaxed(1); }
    void record_iface_error() { m_counters.errors.fetch_add_relaxed(1); }

protected:
    uint64_t        m_id;       // Nonzero and unique for the life of the kernel, 0 means no interface
    bool            m_enabled;  // Administratively up, checked by the stack before frames move either way
    bool            m_loopback; // Frames sent through it come back to this host, so no link or ARP
    sync::atomic<bool> m_link_up; // Carrier as last reported by the driver
    char            m_name[IFACE_NAME_MAX];
    iface_counters  m_counters;

    // Link layer identity, filled in by the driver once the hardware reports it
    eth::mac_addr   m_mac;
    uint16_t        m_mtu; // Largest payload carried in one frame, excluding the link header

    // IPv4 identity, unspecified until configured by hand or through DHCP
    sync::seqlocked<ipv4::ipv4_config> m_ipv4_conf;

    /**
     * @note Privilege: **required**
     */
    __PRIVILEGED_CODE void set_ipv4_conf(const ipv4::ipv4_config& conf);
};

/*
 * Adds `iface` to the stack under the name `<prefix><n>`, such as eth0 or lo0. The
 * registry owns the set and the names, the driver owns the object.
 */
int32_t register_interface(interface* iface, const char* prefix);

size_t interface_count();
interface* interface_at(size_t index);

/*
 * Finds the interface configured with `addr` or returns nullptr.
 */
interface* find_interface_by_address(const ipv4::ipv4_addr& addr);

/*
 * Finds the interface registered as `name` or returns nullptr.
 */
interface* find_interface_by_name(const char* name);

/*
 * Returns the registered loopback interface or nullptr before one exists.
 */
interface* find_loopback_interface();

} // namespace net

#endif // STELLUX_NET_INTERFACE_H
