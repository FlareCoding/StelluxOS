#ifndef STELLUX_NET_NET_H
#define STELLUX_NET_NET_H

#include "common/types.h"

namespace net {

// Result codes shared by every layer of the stack
constexpr int32_t OK            = 0;
constexpr int32_t ERR_INVALID   = -1; // null packet or empty frame
constexpr int32_t ERR_BUSY      = -2; // no transmit slot is free
constexpr int32_t ERR_TOO_LARGE = -3; // frame does not fit in one link transmission
constexpr int32_t ERR_DOWN      = -4; // interface is administratively disabled
constexpr int32_t ERR_NO_MEMORY = -5; // an allocation or task creation failed

/**
 * Initialize the network stack and start its daemon/bookkeeping task.
 * @return OK on success, negative error code on failure.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

} // namespace net

#endif // STELLUX_NET_NET_H
