#include "resource/in_flight.h"

namespace resource {

__PRIVILEGED_DATA static sync::mutex g_in_flight_lock;
__PRIVILEGED_DATA static uint64_t g_in_flight_generation;
__PRIVILEGED_DATA static resource_object* g_visited[MAX_IN_FLIGHT_VISITS];

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void in_flight_init() {
    g_in_flight_lock.init();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE sync::mutex& in_flight_lock() {
    return g_in_flight_lock;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE void in_flight_visit(in_flight_walk& walk, resource_object* held) {
    if (!held || !held->ops || !held->ops->visit_held || held->in_flight_generation == walk.generation) {
        return;
    }

    held->in_flight_generation = walk.generation;
    if (walk.count == MAX_IN_FLIGHT_VISITS) {
        walk.over_limit = true;
        return;
    }

    held->add_ref();
    walk.visited[walk.count++] = held;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool in_flight_reaches(const passed_handle* roots, uint32_t root_count,
                                         is_target_fn is_target, void* context) {
    in_flight_walk walk = {g_visited, 0, false, ++g_in_flight_generation};
    for (uint32_t i = 0; i < root_count; i++) {
        in_flight_visit(walk, roots[i].obj);
    }

    bool reaches = false;
    for (uint32_t i = 0; i < walk.count && !reaches && !walk.over_limit; i++) {
        resource_object* holder = walk.visited[i];
        reaches = is_target(holder, context);
        if (!reaches) {
            holder->ops->visit_held(holder, walk);
        }
    }

    for (uint32_t i = 0; i < walk.count; i++) {
        resource_release(walk.visited[i]);
    }

    return reaches || walk.over_limit;
}

} // namespace resource
