#ifndef STELLUX_NET_NETEVENTS_H
#define STELLUX_NET_NETEVENTS_H

#include "net/net.h"

namespace net {
namespace netevents {

/**
 * @brief Registers /dev/net/events, through which userland learns that the
 * network status changed. A read reports the current status generation once
 * and then nothing until it moves again, poll wakes when it moves.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

} // namespace netevents
} // namespace net

#endif // STELLUX_NET_NETEVENTS_H
