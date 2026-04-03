#ifndef STELLUX_RANDOM_RANDOM_H
#define STELLUX_RANDOM_RANDOM_H

#include "common/types.h"

namespace random {

constexpr int32_t OK        = 0;
constexpr int32_t ERR_NOSRC = -1;

/**
 * Initialize the kernel RNG subsystem: detect hardware source
 * and register /dev/urandom + /dev/random char devices.
 * Must be called after fs::init() (devfs must be mounted).
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

/**
 * Fill buf with len bytes of random data from hardware RNG.
 * @return OK on success, ERR_NOSRC if no hardware source is available.
 */
int32_t fill(void* buf, size_t len);

} // namespace random

#endif // STELLUX_RANDOM_RANDOM_H
