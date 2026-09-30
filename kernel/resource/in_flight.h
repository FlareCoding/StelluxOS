#ifndef STELLUX_RESOURCE_IN_FLIGHT_H
#define STELLUX_RESOURCE_IN_FLIGHT_H

#include "resource/resource.h"
#include "resource/handle_batch.h"
#include "sync/mutex.h"

namespace resource {

// A walk that reaches more holders than this reports its target as reachable
constexpr uint32_t MAX_IN_FLIGHT_VISITS = 1024;

// The holders one walk has reached, each kept alive by a reference until the walk ends
struct in_flight_walk {
    resource_object** visited;
    uint32_t count;
    bool over_limit;
    uint64_t generation;
};

using is_target_fn = bool (*)(resource_object* obj, void* context);

/**
 * @brief Sets up the lock and scratch space the walks share. Called once at boot.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void in_flight_init();

/**
 * @brief The lock every operation that makes one object hold another takes across its
 * in_flight_reaches check and the commit that follows, so two operations cannot each add half
 * of a loop. Waiting for room while holding it is not allowed.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE sync::mutex& in_flight_lock();

/**
 * @brief Whether an object `is_target` accepts is one of `roots` or is held, directly or through
 * other holders, by one of them. Only holders are visited, since nothing else can be part of a
 * loop, and a walk past MAX_IN_FLIGHT_VISITS of them gives up and reports true. The caller holds
 * in_flight_lock(). The references the walk took are dropped before it returns.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool in_flight_reaches(const passed_handle* roots, uint32_t root_count,
                                         is_target_fn is_target, void* context);

/**
 * @brief Reports `held` to the walk, for a holder's visit_held to call on each object it keeps
 * alive. Runs under the holder's locks, which keep `held` alive until the walk references it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void in_flight_visit(in_flight_walk& walk, resource_object* held);

} // namespace resource

#endif // STELLUX_RESOURCE_IN_FLIGHT_H
