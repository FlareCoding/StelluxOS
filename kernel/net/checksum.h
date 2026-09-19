#ifndef STELLUX_NET_CHECKSUM_H
#define STELLUX_NET_CHECKSUM_H

#include "common/types.h"

namespace net {

/*
 * Adds `len` bytes to a running RFC 1071 one's complement sum, so a header
 * and its pseudo-header can be summed in separate calls.
 */
uint32_t checksum_accumulate(uint32_t sum, const void* data, size_t len);

/*
 * Folds the carries and complements the sum. Store the result with `htons`.
 */
uint16_t checksum_finish(uint32_t sum);

/*
 * Checksum of one buffer. Over a header whose checksum field is filled in,
 * the result is zero when the header is intact.
 */
uint16_t checksum(const void* data, size_t len);

} // namespace net

#endif // STELLUX_NET_CHECKSUM_H
