#ifndef SHEEN_WORKSPACE_ACTION_STRIP_H
#define SHEEN_WORKSPACE_ACTION_STRIP_H

#include "extensions/extension_host.h"

#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>

class QHBoxLayout;

namespace sheen {

namespace ui {
class ui_searchable_picker;
}

struct registered_strip {
    QString extension_id;
    QString extension_name;
    strip_spec spec;
};

/* Compact 36 px workspace bar rendering one active extension strip with a multi-extension switcher pill. */
class action_strip : public QWidget {
public:
    explicit action_strip(QWidget* parent = nullptr);

    void set_extension_strip(const QString& extension_id, const QString& extension_name, const strip_spec& spec);
    void remove_extension_strip(const QString& extension_id);

    void set_pinned_extension(const QString& extension_id);
    const QString& pinned_extension() const { return m_pinned_extension; }
    void on_active_file_changed(const QString& file_path);

    std::function<void(const QString& pinned_extension_id)> on_pin_changed;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int find_strip_index(const QString& extension_id) const;
    void choose_active_strip(const QString& file_path);
    void rebuild_widgets();
    void open_switcher_picker(QWidget* anchor);

    QList<registered_strip> m_strips;
    QString m_active_extension;
    QString m_pinned_extension;
    QString m_last_file_path;
    QHBoxLayout* m_layout = nullptr;
    QPointer<ui::ui_searchable_picker> m_open_picker;
    int m_progress_percent = -1;
};

} // namespace sheen

#endif
