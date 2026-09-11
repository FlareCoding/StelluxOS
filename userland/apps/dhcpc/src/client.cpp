#include "client.hpp"

#include <arpa/inet.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

/* RFC 2131 retransmission: 4 s doubling to 64 s, each spread by up to a second either way */
constexpr uint32_t FIRST_BACKOFF_S      = 4;
constexpr uint32_t MAX_BACKOFF_S        = 64;
constexpr uint32_t MAX_REQUEST_ATTEMPTS = 4;
constexpr uint64_t NS_PER_S             = 1000000000ull;
constexpr uint64_t RESTART_HOLDOFF_NS   = 10 * NS_PER_S;
constexpr uint64_t MIN_RENEWAL_RETRY_NS = 60 * NS_PER_S;

constexpr uint8_t PARAMETER_LIST[] = {
    OPT_SUBNET_MASK, OPT_ROUTER, OPT_DNS, OPT_LEASE_TIME, OPT_RENEWAL_TIME, OPT_REBINDING_TIME,
};

struct address_text {
    char text[INET_ADDRSTRLEN] = {};

    explicit address_text(in_addr address) {
        inet_ntop(AF_INET, &address, text, sizeof(text));
    }
};

static const in_addr LIMITED_BROADCAST = {htonl(INADDR_BROADCAST)};

static uint32_t random_u32() {
    uint32_t value = 0;
    (void)getrandom(&value, sizeof(value), 0);
    return value;
}

static const char* type_name(dhcp_type type) {
    switch (type) {
    case dhcp_type::discover: return "DISCOVER";
    case dhcp_type::offer:    return "OFFER";
    case dhcp_type::request:  return "REQUEST";
    case dhcp_type::decline:  return "DECLINE";
    case dhcp_type::ack:      return "ACK";
    case dhcp_type::nak:      return "NAK";
    case dhcp_type::release:  return "RELEASE";
    }

    return "?";
}

dhcp_client::~dhcp_client() {
    if (m_fd >= 0) {
        ::close(m_fd);
    }
}

int dhcp_client::open(const char* iface, const uint8_t* mac, bool verbose, uint32_t lease_cap_s, bool link_up) {
    snprintf(m_iface, sizeof(m_iface), "%s", iface);
    memcpy(m_mac, mac, DHCP_MAC_LEN);
    m_verbose = verbose;
    m_lease_cap_s = lease_cap_s;
    m_link_up = link_up;

    m_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (m_fd < 0) {
        return -1;
    }

    int enable = 1;
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(DHCP_CLIENT_PORT);
    if (setsockopt(m_fd, SOL_SOCKET, SO_BINDTODEVICE, m_iface, strlen(m_iface)) != 0 ||
        setsockopt(m_fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) != 0 ||
        bind(m_fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        int saved = errno;
        ::close(m_fd);
        m_fd = -1;
        errno = saved;
        return -1;
    }

    m_deadline_ns = link_up ? 0 : DHCP_NO_DEADLINE;
    if (!link_up) {
        log("waiting for link");
    }

    return 0;
}

bool dhcp_client::holds_lease() const {
    return m_state == dhcp_state::bound || m_state == dhcp_state::renewing || m_state == dhcp_state::rebinding ||
           m_state == dhcp_state::rebooting;
}

void dhcp_client::on_timeout(uint64_t now_ns) {
    switch (m_state) {
    case dhcp_state::init:
        begin_acquisition(now_ns);
        break;
    case dhcp_state::selecting:
        send_discover(now_ns);
        break;
    case dhcp_state::requesting:
        if (m_request_attempts >= MAX_REQUEST_ATTEMPTS) {
            log("no answer to the request, restarting");
            restart(now_ns);
        } else {
            send_request(now_ns);
        }
        break;
    case dhcp_state::bound:
        enter_renewing(now_ns);
        break;
    case dhcp_state::renewing:
        if (now_ns >= m_rebinding_at_ns) {
            enter_rebinding(now_ns);
        } else {
            send_renewal(now_ns, false);
            m_deadline_ns = retry_before(now_ns, m_rebinding_at_ns);
        }
        break;
    case dhcp_state::rebinding:
        if (now_ns >= m_expiry_at_ns) {
            expire(now_ns);
        } else {
            send_renewal(now_ns, true);
            m_deadline_ns = retry_before(now_ns, m_expiry_at_ns);
        }
        break;
    case dhcp_state::rebooting:
        if (m_request_attempts >= MAX_REQUEST_ATTEMPTS) {
            resume_lease(now_ns);
        } else {
            send_reboot_request(now_ns);
        }
        break;
    }
}

/* Nothing moves without carrier, so the timers pause. When it returns a held
 * lease is confirmed with the server and anything else starts over at once */
void dhcp_client::on_link(bool up, uint64_t now_ns) {
    if (up == m_link_up) {
        return;
    }

    m_link_up = up;
    if (!up) {
        log("link down");
        m_deadline_ns = DHCP_NO_DEADLINE;
        return;
    }

    log("link up");
    if (!holds_lease()) {
        restart(now_ns);
    } else if (now_ns >= m_expiry_at_ns) {
        expire(now_ns);
    } else {
        enter_rebooting(now_ns);
    }
}

void dhcp_client::on_readable(uint64_t now_ns) {
    uint8_t buffer[DHCP_MAX_MESSAGE];

    while (true) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        ssize_t len = recvfrom(m_fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (len < 0) {
            if (errno != EAGAIN && errno != EINTR) {
                log("receive failed: %s", strerror(errno));
            }
            return;
        }

        dhcp_message msg;
        if (!msg.parse(buffer, static_cast<size_t>(len)) || msg.xid() != m_xid || !msg.addressed_to(m_mac)) {
            if (m_verbose) {
                log("ignored a message from %s", address_text(from.sin_addr).text);
            }
            continue;
        }

        dhcp_type type = *msg.type();
        if (m_verbose) {
            log("received %s xid=%08x from %s", type_name(type), msg.xid(), address_text(from.sin_addr).text);
        }

        bool awaiting_answer = m_state == dhcp_state::requesting || m_state == dhcp_state::renewing ||
                               m_state == dhcp_state::rebinding || m_state == dhcp_state::rebooting;
        if (type == dhcp_type::offer && m_state == dhcp_state::selecting) {
            handle_offer(msg, now_ns);
        } else if (type == dhcp_type::ack && awaiting_answer) {
            handle_ack(msg, now_ns);
        } else if (type == dhcp_type::nak && awaiting_answer) {
            handle_nak(now_ns);
        }
    }
}

void dhcp_client::release() {
    if (!holds_lease()) {
        return;
    }

    m_xid = random_u32();
    dhcp_message msg;
    msg.start(dhcp_type::release, m_xid, 0, m_mac, false);
    msg.set_ciaddr(m_lease.address);
    msg.add_address(OPT_SERVER_ID, m_lease.server);
    msg.finish();
    transmit(msg, m_lease.server, "RELEASE");

    log("released %s", address_text(m_lease.address).text);
    drop_lease();
    m_state = dhcp_state::init;
}

void dhcp_client::begin_acquisition(uint64_t now_ns) {
    m_xid = random_u32();
    m_transaction_start_ns = now_ns;
    m_backoff_s = 0;
    m_state = dhcp_state::selecting;
    log("discovering");
    send_discover(now_ns);
}

void dhcp_client::send_discover(uint64_t now_ns) {
    dhcp_message msg;
    msg.start(dhcp_type::discover, m_xid, elapsed_seconds(now_ns), m_mac, true);
    add_common_options(msg);
    msg.finish();

    transmit(msg, LIMITED_BROADCAST, "DISCOVER");
    arm_retransmit(now_ns);
}

void dhcp_client::send_request(uint64_t now_ns) {
    dhcp_message msg;
    msg.start(dhcp_type::request, m_xid, elapsed_seconds(now_ns), m_mac, true);
    msg.add_address(OPT_REQUESTED_ADDR, m_offer.address);
    msg.add_address(OPT_SERVER_ID, m_offer.server);
    add_common_options(msg);
    msg.finish();

    m_request_attempts++;
    transmit(msg, LIMITED_BROADCAST, "REQUEST");
    arm_retransmit(now_ns);
}

void dhcp_client::add_common_options(dhcp_message& msg) const {
    msg.add_option(OPT_PARAM_LIST, PARAMETER_LIST);

    utsname name;
    if (uname(&name) == 0 && name.nodename[0] != '\0') {
        msg.add_option(OPT_HOSTNAME, {reinterpret_cast<const uint8_t*>(name.nodename), strlen(name.nodename)});
    }
}

void dhcp_client::transmit(const dhcp_message& msg, in_addr dest, const char* what) {
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(DHCP_SERVER_PORT);
    to.sin_addr = dest;

    if (sendto(m_fd, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0) {
        log("cannot send %s: %s", what, strerror(errno));
    } else if (m_verbose) {
        log("sent %s xid=%08x to %s", what, m_xid, address_text(dest).text);
    }
}

void dhcp_client::arm_retransmit(uint64_t now_ns) {
    if (m_backoff_s == 0) {
        m_backoff_s = FIRST_BACKOFF_S;
    } else if (m_backoff_s < MAX_BACKOFF_S) {
        m_backoff_s *= 2;
    }

    m_deadline_ns = now_ns + m_backoff_s * NS_PER_S - NS_PER_S + random_u32() % (2 * NS_PER_S);
}

/* A renewal is a fresh transaction addressed to the server that granted the lease */
void dhcp_client::enter_renewing(uint64_t now_ns) {
    m_xid = random_u32();
    m_transaction_start_ns = now_ns;
    m_state = dhcp_state::renewing;
    log("renewing");
    send_renewal(now_ns, false);
    m_deadline_ns = retry_before(now_ns, m_rebinding_at_ns);
}

void dhcp_client::enter_rebinding(uint64_t now_ns) {
    m_state = dhcp_state::rebinding;
    log("rebinding");
    send_renewal(now_ns, true);
    m_deadline_ns = retry_before(now_ns, m_expiry_at_ns);
}

void dhcp_client::send_renewal(uint64_t now_ns, bool broadcast) {
    dhcp_message msg;
    msg.start(dhcp_type::request, m_xid, elapsed_seconds(now_ns), m_mac, false);
    msg.set_ciaddr(m_lease.address);
    add_common_options(msg);
    msg.finish();

    transmit(msg, broadcast ? LIMITED_BROADCAST : m_lease.server, "REQUEST");
}

void dhcp_client::enter_rebooting(uint64_t now_ns) {
    m_xid = random_u32();
    m_transaction_start_ns = now_ns;
    m_request_attempts = 0;
    m_backoff_s = 0;
    m_state = dhcp_state::rebooting;
    log("confirming %s", address_text(m_lease.address).text);
    send_reboot_request(now_ns);
}

void dhcp_client::send_reboot_request(uint64_t now_ns) {
    dhcp_message msg;
    msg.start(dhcp_type::request, m_xid, elapsed_seconds(now_ns), m_mac, true);
    msg.add_address(OPT_REQUESTED_ADDR, m_lease.address);
    add_common_options(msg);
    msg.finish();

    m_request_attempts++;
    transmit(msg, LIMITED_BROADCAST, "REQUEST");
    arm_retransmit(now_ns);
}

/* An unanswered confirmation is not a refusal, the lease runs on under its own timers */
void dhcp_client::resume_lease(uint64_t now_ns) {
    log("no answer, keeping %s", address_text(m_lease.address).text);
    m_state = dhcp_state::bound;
    m_deadline_ns = m_renewal_at_ns > now_ns ? m_renewal_at_ns : now_ns;
}

void dhcp_client::expire(uint64_t now_ns) {
    log("lease expired");
    drop_lease();
    begin_acquisition(now_ns);
}

/* RFC 2131: wait half the time left before the boundary, never under a minute, never past it */
uint64_t dhcp_client::retry_before(uint64_t now_ns, uint64_t boundary_ns) const {
    if (now_ns >= boundary_ns) {
        return now_ns;
    }

    uint64_t wait_ns = (boundary_ns - now_ns) / 2;
    if (wait_ns < MIN_RENEWAL_RETRY_NS) {
        wait_ns = MIN_RENEWAL_RETRY_NS;
    }

    return now_ns + wait_ns < boundary_ns ? now_ns + wait_ns : boundary_ns;
}

void dhcp_client::handle_offer(const dhcp_message& msg, uint64_t now_ns) {
    dhcp_lease offer;
    if (!offer.from_message(msg)) {
        log("ignored an unusable offer");
        return;
    }

    m_offer = offer;
    m_state = dhcp_state::requesting;
    m_request_attempts = 0;
    m_backoff_s = 0;
    log("offered %s by %s", address_text(offer.address).text, address_text(offer.server).text);
    send_request(now_ns);
}

void dhcp_client::handle_ack(const dhcp_message& msg, uint64_t now_ns) {
    bool renewal = m_state != dhcp_state::requesting;
    in_addr expected = renewal ? m_lease.address : m_offer.address;

    dhcp_lease lease;
    if (!lease.from_message(msg, renewal ? &m_lease : nullptr) || lease.address.s_addr != expected.s_addr) {
        if (renewal) {
            log("ignored an unusable acknowledgement");
            return;
        }

        log("unusable acknowledgement, restarting");
        restart(now_ns + RESTART_HOLDOFF_NS);
        return;
    }

    lease.cap_to(m_lease_cap_s);
    if ((!renewal || !lease.same_interface_config(m_lease)) && lease.apply(m_iface) != 0) {
        log("cannot apply %s/%u: %s", address_text(lease.address).text, lease.prefix_length(), strerror(errno));
        if (renewal) {
            drop_lease();
        }

        restart(now_ns + RESTART_HOLDOFF_NS);
        return;
    }

    if ((!renewal || !lease.same_dns(m_lease)) && lease.publish_dns() != 0) {
        log("cannot publish the name servers: %s", strerror(errno));
    }

    bool confirmed = m_state == dhcp_state::rebooting;
    start_lease(lease, now_ns);
    if (confirmed) {
        log("confirmed %s, lease %us", address_text(lease.address).text, lease.lease_seconds);
    } else if (renewal) {
        log("renewed %s, lease %us", address_text(lease.address).text, lease.lease_seconds);
    } else if (lease.router.s_addr != 0) {
        log("bound %s/%u via %s, lease %us", address_text(lease.address).text, lease.prefix_length(),
            address_text(lease.router).text, lease.lease_seconds);
    } else {
        log("bound %s/%u, lease %us", address_text(lease.address).text, lease.prefix_length(), lease.lease_seconds);
    }
}

void dhcp_client::handle_nak(uint64_t now_ns) {
    if (m_state == dhcp_state::requesting) {
        log("declined by %s, restarting", address_text(m_offer.server).text);
        restart(now_ns + RESTART_HOLDOFF_NS);
        return;
    }

    if (m_state == dhcp_state::rebooting) {
        log("%s refused here, discovering", address_text(m_lease.address).text);
        drop_lease();
        restart(now_ns);
        return;
    }

    log("lease withdrawn by %s, restarting", address_text(m_lease.server).text);
    drop_lease();
    restart(now_ns + RESTART_HOLDOFF_NS);
}

void dhcp_client::start_lease(const dhcp_lease& lease, uint64_t now_ns) {
    m_lease = lease;
    m_renewal_at_ns = now_ns + lease.renewal_seconds * NS_PER_S;
    m_rebinding_at_ns = now_ns + lease.rebinding_seconds * NS_PER_S;
    m_expiry_at_ns = now_ns + lease.lease_seconds * NS_PER_S;
    m_state = dhcp_state::bound;
    m_deadline_ns = m_renewal_at_ns;
}

void dhcp_client::drop_lease() {
    if (dhcp_lease::clear_interface(m_iface) != 0) {
        log("cannot clear the interface: %s", strerror(errno));
    }

    m_lease = dhcp_lease{};
}

void dhcp_client::restart(uint64_t at_ns) {
    m_state = dhcp_state::init;
    m_deadline_ns = at_ns;
}

uint16_t dhcp_client::elapsed_seconds(uint64_t now_ns) const {
    uint64_t seconds = (now_ns - m_transaction_start_ns) / NS_PER_S;
    return seconds < UINT16_MAX ? static_cast<uint16_t>(seconds) : UINT16_MAX;
}

void dhcp_client::log(const char* fmt, ...) const {
    printf("dhcpc: %s ", m_iface);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\r\n");
}
