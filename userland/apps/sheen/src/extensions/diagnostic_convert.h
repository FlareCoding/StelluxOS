#ifndef SHEEN_EXTENSIONS_DIAGNOSTIC_CONVERT_H
#define SHEEN_EXTENSIONS_DIAGNOSTIC_CONVERT_H

#include "extensions/extension_host.h"
#include "extensions/workspace_paths.h"

#include <QList>

#include <vector>

namespace sheen {

QList<diagnostic_item> to_host_diagnostics(const std::vector<script_diagnostic>& parsed);

} // namespace sheen

#endif
