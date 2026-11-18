#include "extensions/diagnostic_convert.h"

namespace sheen {

QList<diagnostic_item> to_host_diagnostics(const std::vector<script_diagnostic>& parsed) {
    QList<diagnostic_item> converted;
    converted.reserve(static_cast<qsizetype>(parsed.size()));

    for (const script_diagnostic& entry : parsed) {
        diagnostic_severity level = entry.severity == script_diagnostic_severity::WARNING
                                  ? diagnostic_severity::WARNING
                                  : diagnostic_severity::ERROR;
        diagnostic_item item{
            QString::fromStdString(entry.file_path),
            entry.line,
            entry.column,
            level,
            QString::fromStdString(entry.message),
            {},
        };

        converted.append(item);
    }

    return converted;
}

} // namespace sheen
