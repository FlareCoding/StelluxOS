#ifndef STELLUX_NET_NET_H
#define STELLUX_NET_NET_H

#include "common/types.h"

namespace net {

constexpr int32_t OK       = 0;
constexpr int32_t ERR_INIT = -1;

/**
 * @brief Initialize kernel networking core subsystems.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

} // namespace net

#endif // STELLUX_NET_NET_H
