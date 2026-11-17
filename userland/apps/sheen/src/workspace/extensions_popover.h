#ifndef SHEEN_WORKSPACE_EXTENSIONS_POPOVER_H
#define SHEEN_WORKSPACE_EXTENSIONS_POPOVER_H

#include "extensions/extension.h"
#include "ui/ui_kit.h"

#include <QSet>
#include <QString>

#include <functional>

class QVBoxLayout;

namespace sheen {

/* Compact workspace popover listing extensions with Detected badges, Set Up buttons, and live toggle switches. */
class extensions_popover : public ui::ui_popover {
public:
    extensions_popover(QWidget* anchor, const workspace_files& files, const QSet<QString>& enabled_ids);

    void refresh_rows(const QSet<QString>& enabled_ids);

    std::function<void(const QString& extension_id, bool enabled)> on_toggled;
    std::function<void(const QString& extension_id)> on_setup_requested;

private:
    static int compute_popover_height();

    workspace_files m_files;
    QSet<QString> m_enabled_ids;
    QVBoxLayout* m_rows_layout = nullptr;
};

} // namespace sheen

#endif
