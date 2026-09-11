#ifndef DHCPC_MESSAGE_HPP
#define DHCPC_MESSAGE_HPP

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

constexpr uint16_t DHCP_SERVER_PORT = 67;
constexpr uint16_t DHCP_CLIENT_PORT = 68;
constexpr size_t   DHCP_MAX_MESSAGE = 576;
constexpr size_t   DHCP_MAC_LEN     = 6;

constexpr uint8_t OPT_PAD            = 0;
constexpr uint8_t OPT_SUBNET_MASK    = 1;
constexpr uint8_t OPT_ROUTER         = 3;
constexpr uint8_t OPT_DNS            = 6;
constexpr uint8_t OPT_HOSTNAME       = 12;
constexpr uint8_t OPT_REQUESTED_ADDR = 50;
constexpr uint8_t OPT_LEASE_TIME     = 51;
constexpr uint8_t OPT_MESSAGE_TYPE   = 53;
constexpr uint8_t OPT_SERVER_ID      = 54;
constexpr uint8_t OPT_PARAM_LIST     = 55;
constexpr uint8_t OPT_RENEWAL_TIME   = 58;
constexpr uint8_t OPT_REBINDING_TIME = 59;
constexpr uint8_t OPT_END            = 255;

enum class dhcp_type : uint8_t {
    discover = 1,
    offer    = 2,
    request  = 3,
    decline  = 4,
    ack      = 5,
    nak      = 6,
    release  = 7,
};

/* One DHCP message in a fixed buffer, built option by option or parsed off the wire */
class dhcp_message {
public:
    void start(dhcp_type type, uint32_t xid, uint16_t secs, const uint8_t* mac, bool broadcast);
    bool add_option(uint8_t code, std::span<const uint8_t> value);
    bool add_address(uint8_t code, in_addr address);
    void finish();

    /* Accepts only a well-formed reply carrying a message type */
    bool parse(const uint8_t* bytes, size_t len);
    std::optional<std::span<const uint8_t>> find(uint8_t code) const;

    std::optional<dhcp_type> type() const;
    uint32_t xid() const;
    in_addr yiaddr() const;
    bool addressed_to(const uint8_t* mac) const;

    const uint8_t* data() const { return m_bytes; }
    size_t size() const { return m_len; }

private:
    uint8_t m_bytes[DHCP_MAX_MESSAGE] = {};
    size_t  m_len = 0;
};

#endif // DHCPC_MESSAGE_HPP
