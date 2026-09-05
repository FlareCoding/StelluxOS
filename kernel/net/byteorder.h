#ifndef STELLUX_NET_BYTEORDER_H
#define STELLUX_NET_BYTEORDER_H

#include "common/types.h"

namespace net {

// Multibyte protocol fields travel most significant byte first, which RFC 1700
// calls network byte order. These convert between that and the host's order and
// compile to a byte swap on little-endian hosts and to nothing on big-endian ones.
constexpr bool HOST_IS_LITTLE_ENDIAN = (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

constexpr uint16_t bswap16(uint16_t v) {
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}

constexpr uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}

constexpr uint16_t htons(uint16_t v) { return HOST_IS_LITTLE_ENDIAN ? bswap16(v) : v; }
constexpr uint16_t ntohs(uint16_t v) { return HOST_IS_LITTLE_ENDIAN ? bswap16(v) : v; }
constexpr uint32_t htonl(uint32_t v) { return HOST_IS_LITTLE_ENDIAN ? bswap32(v) : v; }
constexpr uint32_t ntohl(uint32_t v) { return HOST_IS_LITTLE_ENDIAN ? bswap32(v) : v; }

} // namespace net

#endif // STELLUX_NET_BYTEORDER_H
