#ifndef SHEEN_APP_SHEEN_APP_H
#define SHEEN_APP_SHEEN_APP_H

#include "app/instance.h"
#include "app/settings.h"
#include "app/shared_service.h"
#include "app/workspace_store.h"
#include "ui/file_sheet.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>
#include <vector>

namespace sheen {

class sheen_window;

struct orphan_process {
    int proc_handle = -1;
    int pid = -1;
    qint64 kill_deadline_ms = 0;
    bool kill_sent = false;
};

/* Process-wide coordinator owning settings, workspace_store, /tmp/sheen.sock, open windows, and shared services. */
class sheen_app {
public:
    sheen_app();
    ~sheen_app();

    sheen_app(const sheen_app&) = delete;
    sheen_app& operator=(const sheen_app&) = delete;

    const sheen_settings& settings() const { return m_settings; }
    const QStringList& settings_problems() const { return m_settings_problems; }
    workspace_store& store() { return m_store; }
    const workspace_store& store() const { return m_store; }

    sheen_window* create_window();
    sheen_window* open_new_window();
    sheen_window* open_workspace(const QString& root_path, sheen_window* requester = nullptr);
    sheen_window* find_workspace_window(const QString& path_or_child) const;

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

    static void reap_orphan_process(int proc_handle, int pid, int grace_ms);

private:
    sheen_window* target_plain_or_active_window();
    void enqueue_orphan_process(int proc_handle, int pid, int grace_ms);
    void poll_orphan_processes();

    sheen_settings m_settings;
    QStringList m_settings_problems;
    workspace_store m_store;
    instance_server m_instance;
    std::vector<std::unique_ptr<sheen_window>> m_windows;
    QList<sheen_window*> m_recent_windows;
    QStringList m_recent_files;
    QHash<QString, QDateTime> m_recent_opened;
    QHash<QString, std::weak_ptr<shared_service>> m_services;
    QList<orphan_process> m_orphan_processes;
    QTimer m_orphan_timer;
};

} // namespace sheen

#endif
