#ifndef STELLUX_EVENTFD_EVENTFD_H
#define STELLUX_EVENTFD_EVENTFD_H

#include "common/types.h"

namespace resource { struct resource_object; }

/**
 * An eventfd is a 64-bit count behind a handle, which lets one thread wake another. A write adds to
 * the count and a read takes all of it, or a single unit in semaphore mode. Reads wait for a nonzero
 * count and writes wait for room below COUNTER_MAX.
 */
namespace eventfd {

// The all-ones value is reserved, so the count tops out one below it
constexpr uint64_t COUNTER_MAX = 0xFFFFFFFFFFFFFFFEULL;

/**
 * @brief Creates an eventfd whose count starts at `initial_value`.
 * @param semaphore Makes each read take a single unit instead of the whole count.
 * @param out Receives the created object, which holds one reference.
 * @return resource::OK, resource::ERR_INVAL when `initial_value` is above COUNTER_MAX, or resource::ERR_NOMEM.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create(uint64_t initial_value, bool semaphore, resource::resource_object** out);

} // namespace eventfd

#endif // STELLUX_EVENTFD_EVENTFD_H
