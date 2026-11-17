#ifndef SHEEN_WORKSPACE_WORKSPACE_SHELL_H
#define SHEEN_WORKSPACE_WORKSPACE_SHELL_H

#include "app/workspace_store.h"
#include "extensions/extension.h"
#include "extensions/extension_host.h"

#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QWidget>

#include <memory>
#include <unordered_map>

class QLabel;

namespace sheen {

class action_strip;
class dock_host;
class editor_core;
class extension_setup_page;
class extensions_popover;
class sheen_app;
class sidebar_host;

namespace ui {
class ui_pill_button;
}

struct running_extension {
    std::shared_ptr<bool> alive;
    std::unique_ptr<extension_host> adapter;
    std::unique_ptr<extension_session> session;
};

/* Composable folder workspace shell wrapping editor_core with sidebar, action strip, dock, and extension sessions. */
class workspace_shell : public QWidget {
public:
    workspace_shell(sheen_app& app, editor_core& editor, const QString& root_path, QWidget* parent = nullptr);
    ~workspace_shell() override;

    const QString& root_path() const { return m_root_path; }
    const QString& folder_name() const { return m_files.folder_name; }

    sheen_app& app() { return m_app; }
    editor_core& editor() { return m_editor; }
    action_strip& strip() { return *m_action_strip; }
    sidebar_host& sidebar() { return *m_sidebar; }
    dock_host& dock() { return *m_dock; }

    void set_extension_enabled(const QString& extension_id, bool enabled);
    bool is_extension_enabled(const QString& extension_id) const;

    void open_extensions_popover();
    void open_extension_setup(const QString& focus_extension_id = QString());
    void on_dependencies_changed();
    void sync_problems_dock();
    void shutdown_for_close();

private:
    void scan_workspace_files();
    void restore_persisted_state();
    void persist_state();
    void refresh_header_and_offer();
    void dismiss_offer(const QString& extension_id);

    sheen_app& m_app;
    editor_core& m_editor;
    QString m_root_path;
    workspace_files m_files;
    workspace_record m_record;

    ui::ui_pill_button* m_extensions_button = nullptr;
    QPointer<extensions_popover> m_open_popover;
    QPointer<extension_setup_page> m_setup_page;
    QWidget* m_offer_bar = nullptr;
    QLabel* m_offer_label = nullptr;
    ui::ui_pill_button* m_offer_enable_button = nullptr;
    QString m_offered_extension_id;

    action_strip* m_action_strip = nullptr;
    sidebar_host* m_sidebar = nullptr;
    dock_host* m_dock = nullptr;

    std::unordered_map<std::string, running_extension> m_sessions;
};

} // namespace sheen

#endif
