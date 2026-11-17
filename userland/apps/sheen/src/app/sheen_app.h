#ifndef SHEEN_APP_SHEEN_APP_H
#define SHEEN_APP_SHEEN_APP_H

#include "app/instance.h"
#include "app/settings.h"
#include "app/shared_service.h"
#include "ui/file_sheet.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

namespace sheen {

class sheen_window;

/* Process-wide coordinator owning settings, the single-instance socket, open windows, and shared services. */
class sheen_app {
public:
    sheen_app();
    ~sheen_app();

    sheen_app(const sheen_app&) = delete;
    sheen_app& operator=(const sheen_app&) = delete;

    const sheen_settings& settings() const { return m_settings; }
    const QStringList& settings_problems() const { return m_settings_problems; }

    sheen_window* create_window();
    sheen_window* open_new_window();
    void note_window_activated(sheen_window* window);
    void remove_window(sheen_window* window);
    int window_count() const { return static_cast<int>(m_windows.size()); }
    const QList<sheen_window*>& ordered_windows() const { return m_recent_windows; }
    bool quit_all();

    bool focus_open_document(const QString& path);
    int next_untitled_number(int preferred = 1) const;
    void receive_paths(const QStringList& paths);

    void remember_recent_file(const QString& path);
    const QStringList& recent_files() const { return m_recent_files; }
    QList<recent_file> recent_file_entries() const;
    void clear_recent_files();

    void reload_settings();
    void notify_dependencies_changed();

    std::shared_ptr<shared_service> acquire_service(
        const QString& key,
        const std::function<std::shared_ptr<shared_service>()>& factory);

private:
    sheen_window* target_window();

    sheen_settings m_settings;
    QStringList m_settings_problems;
    instance_server m_instance;
    std::vector<std::unique_ptr<sheen_window>> m_windows;
    QList<sheen_window*> m_recent_windows;
    QStringList m_recent_files;
    QHash<QString, QDateTime> m_recent_opened;
    QHash<QString, std::weak_ptr<shared_service>> m_services;
};

} // namespace sheen

#endif
