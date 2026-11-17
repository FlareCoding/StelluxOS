#ifndef SHEEN_EXTENSIONS_REGISTRY_H
#define SHEEN_EXTENSIONS_REGISTRY_H

#include "extensions/extension.h"

#include <span>

namespace sheen {

std::span<const extension_descriptor> registered_extensions();

} // namespace sheen

#endif
