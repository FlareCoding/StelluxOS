#ifndef STELLUX_RESOURCE_PROVIDERS_SHMEM_RESOURCE_PROVIDER_H
#define STELLUX_RESOURCE_PROVIDERS_SHMEM_RESOURCE_PROVIDER_H

#include "resource/resource.h"
#include "mm/shmem.h"

namespace resource::shmem_resource_provider {

/**
 * @brief Create a SHMEM resource backed by a new shmem (size 0).
 * The returned resource_object has one owned reference.
 */
int32_t create_shmem_resource(
    uint32_t flags,
    resource_object** out_obj
);

/**
 * @brief Create a SHMEM resource backed by an existing shmem.
 * Adds a reference to the backing. Used by shm_provider.
 */
int32_t create_shmem_resource_with_backing(
    mm::shmem* backing,
    uint32_t flags,
    resource_object** out_obj
);

/**
 * @brief Get the shmem backing from a SHMEM resource_object.
 * Returns nullptr if obj is not SHMEM or has no impl.
 */
[[nodiscard]] mm::shmem* get_shmem_backing(resource_object* obj);

} // namespace resource::shmem_resource_provider

#endif // STELLUX_RESOURCE_PROVIDERS_SHMEM_RESOURCE_PROVIDER_H
