#ifndef STELLUX_NET_LOOPBACK_H
#define STELLUX_NET_LOOPBACK_H

#include "net/net.h"

namespace net {

/**
 * Initialize and register the loopback network interface ("lo").
 * Creates a virtual interface that loops transmitted frames back to the
 * receive path. Automatically configured with 127.0.0.1/255.0.0.0.
 *
 * Called from net::init(), before any hardware drivers register.
 * @return net::OK on success.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t loopback_init();

/**
 * Get the loopback network interface.
 * Returns nullptr if loopback has not been initialized.
 */
netif* get_loopback_netif();

} // namespace net

#endif // STELLUX_NET_LOOPBACK_H
