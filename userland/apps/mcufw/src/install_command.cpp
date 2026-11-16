#include "commands.hpp"

#include "cancel.hpp"
#include "installer.hpp"
#include "layout.hpp"

static bool parse_targets(const arguments& parsed, std::vector<install_target>& targets, reporter& report) {
    for (const std::string& operand : parsed.operands) {
        install_target target;
        std::string error;
        if (!parse_install_target(operand, target, error)) {
            report.failed({ failure_kind::board, error });
            return false;
        }

        targets.push_back(target);
    }

    return true;
}

int run_install(const arguments& parsed, const directory_layout& layout, reporter& report) {
    std::vector<install_target> targets;
    if (!parse_targets(parsed, targets, report)) {
        return 1;
    }

    watch_for_cancel();
    failure failed;
    if (!install_targets(layout, targets, report, failed)) {
        report.failed(failed);
        return 1;
    }

    return 0;
}

// A core or a library without a version, the whole of which a removal deletes
static bool is_removable(const install_target& target) {
    switch (target.kind) {
    case install_kind::core:
    case install_kind::library:
        return target.version.empty();
    case install_kind::board:
        return false;
    }

    return false;
}

int run_remove(const arguments& parsed, const directory_layout& layout, reporter& report) {
    std::vector<install_target> targets;
    if (!parse_targets(parsed, targets, report)) {
        return 1;
    }

    for (const install_target& target : targets) {
        if (!is_removable(target)) {
            failure_kind kind = target.kind == install_kind::library ? failure_kind::library : failure_kind::core;
            report.failed({ kind, "remove takes a core or a library without a version, such as esp32:esp32" });
            return 1;
        }
    }

    // A removal never reads the flag, so this keeps a signal from stopping it partway
    watch_for_cancel();
    failure failed;
    if (!remove_targets(layout, targets, report, failed)) {
        report.failed(failed);
        return 1;
    }

    return 0;
}
