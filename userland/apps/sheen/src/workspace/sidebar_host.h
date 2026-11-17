#ifndef SHEEN_WORKSPACE_SIDEBAR_HOST_H
#define SHEEN_WORKSPACE_SIDEBAR_HOST_H

#include <QString>
#include <QWidget>

#include <functional>

class QTreeWidget;
class QTreeWidgetItem;

namespace sheen {

/* Workspace sidebar hosting the folder file tree. */
class sidebar_host : public QWidget {
public:
    explicit sidebar_host(const QString& root_path, QWidget* parent = nullptr);

    void set_sidebar_width(int width);
    void set_root_path(const QString& root_path);
    void refresh_files();
    void highlight_active_file(const QString& file_path);

    std::function<void(const QString& file_path)> on_file_activated;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void populate_directory(const QString& dir_path, QTreeWidgetItem* parent_item, int depth);

    QString m_root_path;
    QTreeWidget* m_file_tree = nullptr;
};

} // namespace sheen

#endif
