#ifndef SHEEN_WORKSPACE_WORKSPACE_SHELL_H
#define SHEEN_WORKSPACE_WORKSPACE_SHELL_H

#include "app/workspace_store.h"
#include "extensions/extension.h"

#include <QString>
#include <QWidget>

namespace sheen {

class dock_host;
class editor_core;
class sheen_app;
class sidebar_host;

/* Composable folder workspace shell wrapping editor_core with file sidebar, bottom dock, and persisted layout state. */
class workspace_shell : public QWidget {
public:
    workspace_shell(sheen_app& app, editor_core& editor, const QString& root_path, QWidget* parent = nullptr);
    ~workspace_shell() override;

    const QString& root_path() const { return m_root_path; }
    const QString& folder_name() const { return m_files.folder_name; }

    void on_dependencies_changed();
    void shutdown_for_close();

private:
    void scan_workspace_files();
    void restore_persisted_state();
    void persist_state();
    void sync_problems_dock();

    sheen_app& m_app;
    editor_core& m_editor;
    QString m_root_path;
    workspace_files m_files;
    workspace_record m_record;

    sidebar_host* m_sidebar = nullptr;
    dock_host* m_dock = nullptr;
};

} // namespace sheen

#endif
