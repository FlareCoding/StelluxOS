#ifndef STELLUX_RESOURCE_PROVIDERS_SHM_PROVIDER_H
#define STELLUX_RESOURCE_PROVIDERS_SHM_PROVIDER_H

#include "resource/resource.h"

namespace resource::shm_provider {

/**
 * @brief Check if a path is a /dev/shm path.
 * @return true if path starts with "/dev/shm/" or equals "/dev/shm".
 */
bool is_shm_path(const char* path);

/**
 * @brief Open or create a named shared memory resource.
 * Path must be "/dev/shm/<name>"; name is extracted and used as key.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t open_shm_resource(
    const char* path,
    uint32_t flags,
    resource_object** out_obj
);

/**
 * @brief Unlink a named shared memory object.
 * Removes the name from the registry; backing persists until
 * all fds are closed and all mappings are unmapped.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t unlink_shm(const char* path);

} // namespace resource::shm_provider

#endif // STELLUX_RESOURCE_PROVIDERS_SHM_PROVIDER_H
