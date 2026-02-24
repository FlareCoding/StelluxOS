#ifndef STELLUX_DEBUG_DEBUG_H
#define STELLUX_DEBUG_DEBUG_H

#include "common/types.h"

namespace debug {

constexpr int32_t OK = 0;

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t init();

} // namespace debug

#endif // STELLUX_DEBUG_DEBUG_H
