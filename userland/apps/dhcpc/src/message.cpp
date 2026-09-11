#include "message.hpp"

#include <arpa/inet.h>

#include <cstring>

/* Fixed header layout of RFC 2131, offsets in bytes */
constexpr size_t HDR_OP      = 0;
constexpr size_t HDR_HTYPE   = 1;
constexpr size_t HDR_HLEN    = 2;
constexpr size_t HDR_XID     = 4;
constexpr size_t HDR_SECS    = 8;
constexpr size_t HDR_FLAGS   = 10;
constexpr size_t HDR_CIADDR  = 12;
constexpr size_t HDR_YIADDR  = 16;
constexpr size_t HDR_CHADDR  = 28;
constexpr size_t HDR_COOKIE  = 236;
constexpr size_t HDR_OPTIONS = 240;

constexpr uint8_t  BOOTREQUEST    = 1;
constexpr uint8_t  BOOTREPLY      = 2;
constexpr uint8_t  HTYPE_ETHERNET = 1;
constexpr uint16_t FLAG_BROADCAST = 0x8000;
constexpr uint8_t  MAGIC_COOKIE[4] = {99, 130, 83, 99};

static void put_u16(uint8_t* at, uint16_t value) {
    uint16_t wire = htons(value);
    memcpy(at, &wire, sizeof(wire));
}

static void put_u32(uint8_t* at, uint32_t value) {
    uint32_t wire = htonl(value);
    memcpy(at, &wire, sizeof(wire));
}

void dhcp_message::start(dhcp_type type, uint32_t xid, uint16_t secs, const uint8_t* mac, bool broadcast) {
    memset(m_bytes, 0, sizeof(m_bytes));
    m_bytes[HDR_OP] = BOOTREQUEST;
    m_bytes[HDR_HTYPE] = HTYPE_ETHERNET;
    m_bytes[HDR_HLEN] = DHCP_MAC_LEN;
    put_u32(m_bytes + HDR_XID, xid);
    put_u16(m_bytes + HDR_SECS, secs);
    put_u16(m_bytes + HDR_FLAGS, broadcast ? FLAG_BROADCAST : 0);
    memcpy(m_bytes + HDR_CHADDR, mac, DHCP_MAC_LEN);
    memcpy(m_bytes + HDR_COOKIE, MAGIC_COOKIE, sizeof(MAGIC_COOKIE));
    m_len = HDR_OPTIONS;

    uint8_t code = static_cast<uint8_t>(type);
    add_option(OPT_MESSAGE_TYPE, {&code, 1});
}

void dhcp_message::set_ciaddr(in_addr address) {
    memcpy(m_bytes + HDR_CIADDR, &address.s_addr, sizeof(address.s_addr));
}

/* Always leaves room for the END marker finish() appends */
bool dhcp_message::add_option(uint8_t code, std::span<const uint8_t> value) {
    if (value.size() > UINT8_MAX || m_len + 2 + value.size() + 1 > sizeof(m_bytes)) {
        return false;
    }

    m_bytes[m_len++] = code;
    m_bytes[m_len++] = static_cast<uint8_t>(value.size());
    memcpy(m_bytes + m_len, value.data(), value.size());
    m_len += value.size();
    return true;
}

bool dhcp_message::add_address(uint8_t code, in_addr address) {
    return add_option(code, {reinterpret_cast<const uint8_t*>(&address.s_addr), sizeof(address.s_addr)});
}

void dhcp_message::finish() {
    m_bytes[m_len++] = OPT_END;
}

bool dhcp_message::parse(const uint8_t* bytes, size_t len) {
    if (len < HDR_OPTIONS || len > sizeof(m_bytes)) {
        return false;
    }

    if (bytes[HDR_OP] != BOOTREPLY || bytes[HDR_HTYPE] != HTYPE_ETHERNET || bytes[HDR_HLEN] != DHCP_MAC_LEN) {
        return false;
    }

    if (memcmp(bytes + HDR_COOKIE, MAGIC_COOKIE, sizeof(MAGIC_COOKIE)) != 0) {
        return false;
    }

    memcpy(m_bytes, bytes, len);
    m_len = len;
    return type().has_value();
}

std::optional<std::span<const uint8_t>> dhcp_message::find(uint8_t code) const {
    size_t pos = HDR_OPTIONS;
    while (pos < m_len && m_bytes[pos] != OPT_END) {
        if (m_bytes[pos] == OPT_PAD) {
            pos++;
            continue;
        }

        if (pos + 2 > m_len) {
            break;
        }

        size_t value_len = m_bytes[pos + 1];
        if (pos + 2 + value_len > m_len) {
            break;
        }

        if (m_bytes[pos] == code) {
            return std::span<const uint8_t>(m_bytes + pos + 2, value_len);
        }

        pos += 2 + value_len;
    }

    return std::nullopt;
}

std::optional<dhcp_type> dhcp_message::type() const {
    auto value = find(OPT_MESSAGE_TYPE);
    if (!value || value->size() != 1) {
        return std::nullopt;
    }

    uint8_t code = (*value)[0];
    if (code < static_cast<uint8_t>(dhcp_type::discover) || code > static_cast<uint8_t>(dhcp_type::release)) {
        return std::nullopt;
    }

    return static_cast<dhcp_type>(code);
}

uint32_t dhcp_message::xid() const {
    uint32_t wire = 0;
    memcpy(&wire, m_bytes + HDR_XID, sizeof(wire));
    return ntohl(wire);
}

in_addr dhcp_message::yiaddr() const {
    in_addr address;
    memcpy(&address.s_addr, m_bytes + HDR_YIADDR, sizeof(address.s_addr));
    return address;
}

bool dhcp_message::addressed_to(const uint8_t* mac) const {
    return memcmp(m_bytes + HDR_CHADDR, mac, DHCP_MAC_LEN) == 0;
}
