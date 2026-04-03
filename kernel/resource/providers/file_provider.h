#ifndef STELLUX_RESOURCE_PROVIDERS_FILE_PROVIDER_H
#define STELLUX_RESOURCE_PROVIDERS_FILE_PROVIDER_H

#include "resource/resource.h"

namespace fs { class file; }

namespace resource::file_provider {

/**
 * @brief Open a FILE resource from a path.
 * On success returns a resource object with one owned reference.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t open_file_resource(
    const char* path,
    uint32_t flags,
    resource_object** out_obj
);

/**
 * @brief Get the fs::file from a FILE resource_object.
 * Returns nullptr if obj is not a FILE resource or has no impl.
 * @note Privilege: **required**
 */
[[nodiscard]] __PRIVILEGED_CODE fs::file* get_file(resource_object* obj);

} // namespace resource::file_provider

#endif // STELLUX_RESOURCE_PROVIDERS_FILE_PROVIDER_H
