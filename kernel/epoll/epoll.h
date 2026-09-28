#ifndef STELLUX_EPOLL_EPOLL_H
#define STELLUX_EPOLL_EPOLL_H

#include "common/types.h"
#include "resource/resource_types.h"
#include "sync/poll.h"

namespace resource { struct resource_object; }

/**
 * An epoll is a resource holding interests. Each interest asks to hear about some events on one object,
 * reached through one handle, and names a value to report with them. An interest holds no reference on
 * its object, and ends when it is removed or when the epoll or the object is destroyed.
 */
namespace epoll {

constexpr int32_t OK        = 0;
constexpr int32_t ERR_INVAL = -1;
constexpr int32_t ERR_NOENT = -2;
constexpr int32_t ERR_EXIST = -3;
constexpr int32_t ERR_NOMEM = -4;
constexpr int32_t ERR_PERM  = -5;
constexpr int32_t ERR_NOSPC = -6;

// The events an interest may ask for, which share their values with the poll layer
constexpr uint32_t INTEREST_EVENTS =
    sync::POLL_IN | sync::POLL_PRI | sync::POLL_OUT | sync::POLL_ERR | sync::POLL_HUP | sync::POLL_RDHUP;

constexpr uint32_t MAX_INTERESTS = 4096;

/**
 * @brief Creates an epoll with no interests.
 * @param out Receives the created object, which holds one reference.
 * @return OK, ERR_INVAL for a null `out`, or ERR_NOMEM.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t create(resource::resource_object** out);

/**
 * @brief Registers interest in `events` on `target`, reached through `handle`, to be reported with `data`.
 * The caller holds a reference on `target` for the call.
 * @return OK, ERR_INVAL when `ep` is not an epoll, `target` is one, or `events` has bits outside
 *   INTEREST_EVENTS, ERR_PERM for a target that cannot be polled, ERR_EXIST for an existing interest,
 *   ERR_NOSPC once `ep` holds MAX_INTERESTS, or ERR_NOMEM.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t add_interest(resource::resource_object* ep, resource::handle_t handle,
                                       resource::resource_object* target, uint32_t events, uint64_t data);

/**
 * @brief Replaces the events and data of the interest in `target` through `handle`.
 * @return OK, ERR_INVAL when `ep` is not an epoll or `events` has bits outside INTEREST_EVENTS, or
 *   ERR_NOENT when no such interest exists.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t modify_interest(resource::resource_object* ep, resource::handle_t handle,
                                          resource::resource_object* target, uint32_t events, uint64_t data);

/**
 * @brief Ends the interest in `target` through `handle`.
 * @return OK, ERR_INVAL when `ep` is not an epoll, or ERR_NOENT when no such interest exists.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t remove_interest(resource::resource_object* ep, resource::handle_t handle,
                                          resource::resource_object* target);

/**
 * @brief The number of interests `ep` holds, or 0 when it is not an epoll.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE uint32_t interest_count(resource::resource_object* ep);

} // namespace epoll

#endif // STELLUX_EPOLL_EPOLL_H
