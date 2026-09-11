#ifndef DHCPC_CLIENT_HPP
#define DHCPC_CLIENT_HPP

#include "lease.hpp"
#include "message.hpp"

#include <cstdint>

enum class dhcp_state {
    init,
    selecting,
    requesting,
    bound,
    renewing,
    rebinding,
};

/* One interface's walk through RFC 2131, driven by the owner's poll loop */
class dhcp_client {
public:
    dhcp_client() = default;
    dhcp_client(const dhcp_client&) = delete;
    dhcp_client& operator=(const dhcp_client&) = delete;
    ~dhcp_client();

    int open(const char* iface, const uint8_t* mac, bool verbose, uint32_t lease_cap_s);

    int fd() const { return m_fd; }
    uint64_t deadline_ns() const { return m_deadline_ns; }

    void on_timeout(uint64_t now_ns);
    void on_readable(uint64_t now_ns);

    /* Hands the address back to the server and clears the interface */
    void release();

private:
    bool holds_lease() const;

    void begin_acquisition(uint64_t now_ns);
    void send_discover(uint64_t now_ns);
    void send_request(uint64_t now_ns);
    void add_common_options(dhcp_message& msg) const;
    void transmit(const dhcp_message& msg, in_addr dest, const char* what);
    void arm_retransmit(uint64_t now_ns);

    void enter_renewing(uint64_t now_ns);
    void enter_rebinding(uint64_t now_ns);
    void send_renewal(uint64_t now_ns, bool broadcast);
    void expire(uint64_t now_ns);
    uint64_t retry_before(uint64_t now_ns, uint64_t boundary_ns) const;

    void handle_offer(const dhcp_message& msg, uint64_t now_ns);
    void handle_ack(const dhcp_message& msg, uint64_t now_ns);
    void handle_nak(uint64_t now_ns);
    void start_lease(const dhcp_lease& lease, uint64_t now_ns);
    void drop_lease();
    void restart(uint64_t at_ns);

    uint16_t elapsed_seconds(uint64_t now_ns) const;
    void log(const char* fmt, ...) const __attribute__((format(printf, 2, 3)));

    char       m_iface[16] = {};
    uint8_t    m_mac[DHCP_MAC_LEN] = {};
    int        m_fd = -1;
    bool       m_verbose = false;
    uint32_t   m_lease_cap_s = 0;
    dhcp_state m_state = dhcp_state::init;
    uint32_t   m_xid = 0;
    uint64_t   m_transaction_start_ns = 0;
    uint64_t   m_deadline_ns = 0;
    uint32_t   m_backoff_s = 0;
    uint32_t   m_request_attempts = 0;
    uint64_t   m_renewal_at_ns = 0;
    uint64_t   m_rebinding_at_ns = 0;
    uint64_t   m_expiry_at_ns = 0;
    dhcp_lease m_offer;
    dhcp_lease m_lease;
};

#endif // DHCPC_CLIENT_HPP
