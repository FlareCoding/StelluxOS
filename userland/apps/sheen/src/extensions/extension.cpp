#include "extensions/extension.h"

#include <unistd.h>

namespace sheen {

bool workspace_files::has_suffix(const QString& suffix) const {
    for (const QString& entry : entry_names) {
        if (entry.endsWith(suffix, Qt::CaseInsensitive)) {
            return true;
        }
    }

    return false;
}

bool is_dependency_group_installed(const dependency_group& group) {
    for (const package_requirement& pkg : group.packages) {
        if (pkg.probe_path && pkg.probe_path[0] != '\0' && ::access(pkg.probe_path, F_OK) != 0) {
            return false;
        }
    }

    if (!group.post_install_argv.empty() && group.post_install_probe_path && group.post_install_probe_path[0] != '\0') {
        if (::access(group.post_install_probe_path, F_OK) != 0) {
            return false;
        }
    }

    return true;
}

bool are_dependencies_installed(const extension_descriptor& descriptor) {
    for (const dependency_group& group : descriptor.dependencies) {
        if (group.is_required && !is_dependency_group_installed(group)) {
            return false;
        }
    }

    return true;
}

} // namespace sheen
