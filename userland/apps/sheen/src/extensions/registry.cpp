#include "extensions/registry.h"

#include "extensions/embedded/embedded_extension.h"
#include "extensions/python/python_extension.h"

namespace sheen {

static const extension_descriptor EXTENSIONS[] = {
    embedded_extension(),
    python_extension(),
};

std::span<const extension_descriptor> registered_extensions() {
    return EXTENSIONS;
}

} // namespace sheen
