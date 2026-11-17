#include "extensions/registry.h"
#include "extensions/embedded/embedded_extension.h"

namespace sheen {

static const extension_descriptor EXTENSIONS[] = {
    embedded_extension(),
};

std::span<const extension_descriptor> registered_extensions() {
    return EXTENSIONS;
}

} // namespace sheen
