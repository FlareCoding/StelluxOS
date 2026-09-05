#include "net/checksum.h"

namespace net {

uint32_t checksum_accumulate(uint32_t sum, const void* data, size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    // Words are summed as they appear on the wire, most significant byte first
    while (len >= 2) {
        sum += static_cast<uint32_t>((bytes[0] << 8) | bytes[1]);
        bytes += 2;
        len -= 2;
    }

    // An odd trailing byte is the high half of a word padded with zero
    if (len == 1) {
        sum += static_cast<uint32_t>(bytes[0] << 8);
    }

    return sum;
}

uint16_t checksum_finish(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

uint16_t checksum(const void* data, size_t len) {
    return checksum_finish(checksum_accumulate(0, data, len));
}

} // namespace net
