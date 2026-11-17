#ifndef SHEEN_WORKSPACE_DOCK_HOST_H
#define SHEEN_WORKSPACE_DOCK_HOST_H

#include "app/workspace_store.h"
#include "extensions/extension_host.h"
#include "workspace/dock_window.h"

#include <QList>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <unordered_map>

class QHBoxLayout;
class QStackedWidget;
class QVBoxLayout;

namespace sheen {

namespace ui {
class ui_pill_button;
}

struct dock_panel_entry {
    QString key;
    QString owner_id;
    dock_panel_spec spec;
    QWidget* popped_placeholder = nullptr;
};

/* Bottom workspace dock (204 px open, 32 px collapsed) with alien-widget Pop-Out and Dock-Back. */
class dock_host : public QWidget {
public:
    explicit dock_host(QWidget* parent = nullptr);
    ~dock_host() override;

    void set_expanded_height(int height);
    int expanded_height() const { return m_expanded_height; }

    void set_collapsed(bool collapsed);
    bool is_collapsed() const { return m_is_collapsed; }

    QString active_panel_key() const { return m_active_key; }
    void restore_active_panel_key(const QString& key);
    void select_panel_by_key(const QString& key);

    void set_problems(const QList<diagnostic_item>& problems,
                      const std::function<void(const QString& path, int line)>& on_open_location);

    void set_extension_panels(const QString& extension_id, const QList<dock_panel_spec>& panels);
    void remove_extension_panels(const QString& extension_id);
    void update_panel_state(const QString& extension_id, const QString& panel_id, bool has_activity_dot,
                            int error_count);
    void reveal_panel(const QString& extension_id, const QString& panel_id);

    void pop_out_active_panel();
    void dock_back(const QString& panel_key, bool from_window_close = false);
    void shutdown_for_close();

    std::function<void()> on_layout_changed;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    static QString make_key(const QString& owner_id, const QString& panel_id);
    int find_panel_index(const QString& key) const;
    bool is_popped_out(const QString& key) const;
    bool has_any_docked_panel() const;
    void pop_out(const QString& key);
    void tear_down_popped_window(const QString& key);
    void sync_geometry_and_controls();
    void rebuild_tab_buttons();

    int m_expanded_height = DEFAULT_DOCK_HEIGHT;
    bool m_is_collapsed = false;
    bool m_closing_workspace = false;
    QString m_active_key;

    QWidget* m_tab_bar = nullptr;
    QHBoxLayout* m_tab_layout = nullptr;
    ui::ui_pill_button* m_pop_button = nullptr;
    ui::ui_pill_button* m_collapse_button = nullptr;
    QStackedWidget* m_stack = nullptr;
    QWidget* m_problems_widget = nullptr;
    QVBoxLayout* m_problems_layout = nullptr;

    QList<dock_panel_entry> m_panels;
    std::unordered_map<std::string, std::unique_ptr<dock_window>> m_popped_windows;
};

} // namespace sheen

#endif
