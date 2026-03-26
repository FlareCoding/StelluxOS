#ifndef STELLUX_NET_BYTEORDER_H
#define STELLUX_NET_BYTEORDER_H

#include "common/types.h"

namespace net {

inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

} // namespace net

#endif // STELLUX_NET_BYTEORDER_H
