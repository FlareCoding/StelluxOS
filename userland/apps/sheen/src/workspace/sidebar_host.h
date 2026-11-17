#ifndef SHEEN_WORKSPACE_SIDEBAR_HOST_H
#define SHEEN_WORKSPACE_SIDEBAR_HOST_H

#include "extensions/extension_host.h"

#include <QList>
#include <QString>
#include <QWidget>

#include <functional>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace sheen {

struct owned_sidebar_section {
    QString extension_id;
    sidebar_section_spec spec;
    QWidget* header_widget = nullptr;
    QLabel* header_label = nullptr;
};

/* Workspace sidebar hosting the folder file tree and scoped per-extension sidebar sections. */
class sidebar_host : public QWidget {
public:
    explicit sidebar_host(const QString& root_path, QWidget* parent = nullptr);

    void set_sidebar_width(int width);
    void set_root_path(const QString& root_path);
    void refresh_files();
    void highlight_active_file(const QString& file_path);

    void set_extension_sections(const QString& extension_id, const QList<sidebar_section_spec>& sections);
    void remove_extension_sections(const QString& extension_id);

    std::function<void(const QString& file_path)> on_file_activated;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void populate_directory(const QString& dir_path, QTreeWidgetItem* parent_item, int depth);
    void rebuild_sections_layout();

    QString m_root_path;
    QTreeWidget* m_file_tree = nullptr;
    QWidget* m_sections_container = nullptr;
    QVBoxLayout* m_sections_layout = nullptr;
    QList<owned_sidebar_section> m_sections;
};

} // namespace sheen

#endif
