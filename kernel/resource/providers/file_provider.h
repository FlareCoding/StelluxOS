#ifndef STELLUX_RESOURCE_PROVIDERS_FILE_PROVIDER_H
#define STELLUX_RESOURCE_PROVIDERS_FILE_PROVIDER_H

#include "resource/resource.h"

namespace resource::file_provider {

/**
 * @brief Open a FILE resource from a path.
 * On success returns a resource object with refcount=1.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE int32_t open_file_resource(
    const char* path,
    uint32_t flags,
    resource_object** out_obj
);

} // namespace resource::file_provider

#endif // STELLUX_RESOURCE_PROVIDERS_FILE_PROVIDER_H
