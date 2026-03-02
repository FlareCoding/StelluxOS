#ifndef STELLUX_RESOURCE_PROVIDERS_SOCKET_PROVIDER_H
#define STELLUX_RESOURCE_PROVIDERS_SOCKET_PROVIDER_H

#include "resource/resource.h"
#include "net/unix_stream.h"

namespace resource::socket_provider {

/**
 * @brief Create a new AF_UNIX stream socket resource object.
 * On success returns a resource object with one owned reference.
 */
int32_t create_stream_socket_resource(
    bool nonblocking,
    resource_object** out_obj
);

/**
 * @brief Bind a socket resource to a local path.
 */
int32_t bind(
    resource_object* obj,
    const net::unix_stream::socket_path& path
);

/**
 * @brief Put a bound socket resource into listening mode.
 */
int32_t listen(resource_object* obj, uint32_t backlog);

/**
 * @brief Connect a socket resource to a listening path.
 */
int32_t connect(
    resource_object* obj,
    const net::unix_stream::socket_path& path
);

/**
 * @brief Accept one incoming connection from a listener resource.
 * On success returns a new connected socket resource object with one owned reference.
 */
int32_t accept(
    resource_object* listener_obj,
    resource_object** out_obj
);

/**
 * @brief Query O_NONBLOCK state from a socket resource.
 */
int32_t get_nonblocking(
    resource_object* obj,
    bool* out_nonblocking
);

/**
 * @brief Set O_NONBLOCK state on a socket resource.
 */
int32_t set_nonblocking(
    resource_object* obj,
    bool nonblocking
);

} // namespace resource::socket_provider

#endif // STELLUX_RESOURCE_PROVIDERS_SOCKET_PROVIDER_H
