#include "extensions/registry.h"

#include "extensions/c_run/c_run_extension.h"
#include "extensions/embedded/embedded_extension.h"
#include "extensions/perf_binaries/perf_binaries_extension.h"
#include "extensions/python/python_extension.h"

namespace sheen {

static const extension_descriptor EXTENSIONS[] = {
    embedded_extension(),
    python_extension(),
    c_run_extension(),
    perf_binaries_extension(),
};

std::span<const extension_descriptor> registered_extensions() {
    return EXTENSIONS;
}

} // namespace sheen
