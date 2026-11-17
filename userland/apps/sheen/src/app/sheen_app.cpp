#include "app/sheen_app.h"
#include "core/editor_core.h"
#include "core/kiln.h"
#include "workspace/dependency_installer.h"
#include "workspace/sheen_window.h"

#include <QApplication>
#include <QFileInfo>
#include <QPointer>

#include <stlx/proc.h>

#include <cerrno>
#include <poll.h>

namespace sheen {

static constexpr int RECENT_FILE_LIMIT = 20;
static constexpr int ORPHAN_POLL_MS = 25;
static sheen_app* s_active_app = nullptr;

static void reap_handle_blocking(int proc_handle) {
    int wait_status = 0;
    while (proc_wait(proc_handle, &wait_status) < 0 && errno == EINTR) {
    }
}

sheen_app::sheen_app() {
    s_active_app = this;
    m_settings_problems = read_settings(QString::fromLatin1(SETTINGS_PATH), &m_settings);
    set_text_size(m_settings.font_size);

    m_orphan_timer.setInterval(ORPHAN_POLL_MS);
    QObject::connect(&m_orphan_timer, &QTimer::timeout, [this] { poll_orphan_processes(); });

    m_installer = std::make_unique<dependency_installer>();
    m_installer->on_dependencies_installed = [this] { notify_dependencies_changed(); };

    for (const std::string& path : m_store.load_recent_paths()) {
        QString decoded = QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size()));
        m_recent_files.append(decoded);
        m_recent_opened.insert(decoded, QFileInfo(decoded).lastModified());
    }

    m_instance.on_paths = [this](const QStringList& handed) { receive_paths(handed); };
    m_instance.listen();
}

sheen_app::~sheen_app() {
    m_installer.reset();
    if (s_active_app == this) {
        s_active_app = nullptr;
    }

    m_orphan_timer.stop();
    for (const orphan_process& entry : m_orphan_processes) {
        if (!entry.kill_sent) {
            proc_kill(entry.proc_handle);
        }

        reap_handle_blocking(entry.proc_handle);
    }

    m_orphan_processes.clear();
}

sheen_window* sheen_app::create_window() {
    auto owned = std::make_unique<sheen_window>(*this);
    sheen_window* window = owned.get();
    m_windows.push_back(std::move(owned));
    m_recent_windows.prepend(window);

    return window;
}

sheen_window* sheen_app::open_new_window() {
    sheen_window* window = create_window();
    window->open_paths({});
    window->show();
    window->raise();
    window->activateWindow();

    return window;
}

sheen_window* sheen_app::find_workspace_window(const QString& path_or_child) const {
    QString normalized = QString::fromStdString(
        workspace_store::normalize_workspace_path(path_or_child.toStdString()));

    for (sheen_window* window : m_recent_windows) {
        if (!window->is_workspace()) {
            continue;
        }

        const QString& ws_root = window->workspace_root();
        if (normalized == ws_root || normalized.startsWith(ws_root + QLatin1Char('/'))) {
            return window;
        }
    }

    return nullptr;
}

sheen_window* sheen_app::open_workspace(const QString& root_path, sheen_window* requester) {
    QString normalized = QString::fromStdString(workspace_store::normalize_workspace_path(root_path.toStdString()));
    if (!QFileInfo(normalized).isDir()) {
        return nullptr;
    }

    remember_recent_file(normalized);

    for (sheen_window* existing : m_recent_windows) {
        if (existing->is_workspace() && existing->workspace_root() == normalized) {
            if (requester && requester != existing) {
                QList<migrated_tab> moved = requester->editor().extract_tabs_under(normalized, &existing->editor());
                existing->editor().adopt_migrated_tabs(moved);
            }

            if (existing->isMinimized()) {
                existing->showNormal();
            }

            existing->raise();
            existing->activateWindow();
            return existing;
        }
    }

    if (requester && !requester->is_workspace() && requester->editor().can_promote_in_place_for(normalized)) {
        requester->promote_to_workspace(normalized);
        requester->show();
        requester->raise();
        requester->activateWindow();
        return requester;
    }

    sheen_window* created = create_window();
    if (requester) {
        QList<migrated_tab> moved = requester->editor().extract_tabs_under(normalized, &created->editor());
        created->editor().adopt_migrated_tabs(moved);
    }

    created->promote_to_workspace(normalized);
    created->show();
    created->raise();
    created->activateWindow();

    return created;
}

void sheen_app::note_window_activated(sheen_window* window) {
    m_recent_windows.removeAll(window);
    m_recent_windows.prepend(window);
}

void sheen_app::remove_window(sheen_window* window) {
    m_recent_windows.removeAll(window);
    for (auto it = m_windows.begin(); it != m_windows.end(); ++it) {
        if (it->get() == window) {
            it->release()->deleteLater();
            m_windows.erase(it);
            break;
        }
    }

    if (m_windows.empty()) {
        QApplication::quit();
    }
}

bool sheen_app::quit_all() {
    QList<QPointer<sheen_window>> snapshot;
    for (sheen_window* window : m_recent_windows) {
        snapshot.append(window);
    }

    for (const QPointer<sheen_window>& window : snapshot) {
        if (window && !window->confirm_close(true)) {
            for (const QPointer<sheen_window>& confirmed : snapshot) {
                if (confirmed) {
                    confirmed->reset_close_confirmation();
                }
            }

            return false;
        }
    }

    for (const QPointer<sheen_window>& window : snapshot) {
        if (window) {
            window->close();
        }
    }

    return true;
}

bool sheen_app::focus_open_document(const QString& path) {
    for (sheen_window* window : m_recent_windows) {
        if (!window->editor().focus_if_open(path)) {
            continue;
        }

        if (window->isMinimized()) {
            window->showNormal();
        }

        window->raise();
        window->activateWindow();
        return true;
    }

    return false;
}

int sheen_app::next_untitled_number(int preferred) const {
    auto is_taken = [this](int candidate) {
        for (const std::unique_ptr<sheen_window>& window : m_windows) {
            if (window->editor().is_untitled_number_taken(candidate)) {
                return true;
            }
        }

        return false;
    };

    if (preferred >= 1 && !is_taken(preferred)) {
        return preferred;
    }

    int number = 1;
    while (is_taken(number)) {
        number++;
    }

    return number;
}

sheen_window* sheen_app::target_plain_or_active_window() {
    for (sheen_window* window : m_recent_windows) {
        if (!window->is_workspace()) {
            return window;
        }
    }

    if (!m_recent_windows.isEmpty()) {
        return m_recent_windows.first();
    }

    sheen_window* window = create_window();
    window->show();

    return window;
}

void sheen_app::receive_paths(const QStringList& paths) {
    if (paths.isEmpty()) {
        sheen_window* window = target_plain_or_active_window();
        window->receive_paths({});
        return;
    }

    QStringList remaining_files;
    for (const QString& path : paths) {
        QFileInfo info(path);
        if (info.isDir()) {
            open_workspace(info.absoluteFilePath(), nullptr);
            continue;
        }

        if (focus_open_document(info.absoluteFilePath())) {
            continue;
        }

        if (sheen_window* ws = find_workspace_window(info.absoluteFilePath())) {
            ws->receive_paths({ info.absoluteFilePath() });
            continue;
        }

        remaining_files.append(path);
    }

    if (!remaining_files.isEmpty()) {
        sheen_window* window = target_plain_or_active_window();
        window->receive_paths(remaining_files);
    }
}

void sheen_app::remember_recent_file(const QString& path) {
    if (path.isEmpty()) {
        return;
    }

    m_recent_opened.insert(path, QDateTime::currentDateTime());
    m_recent_files.removeAll(path);
    m_recent_files.prepend(path);
    while (m_recent_files.size() > RECENT_FILE_LIMIT) {
        m_recent_files.removeLast();
    }

    m_store.record_recent_path(path.toStdString());
}

QList<recent_file> sheen_app::recent_file_entries() const {
    QList<recent_file> entries;
    for (const QString& path : m_recent_files) {
        entries.append({ path, m_recent_opened.value(path) });
    }

    return entries;
}

void sheen_app::clear_recent_files() {
    m_recent_files.clear();
    m_recent_opened.clear();
    m_store.clear_recent_paths();
    for (const std::unique_ptr<sheen_window>& window : m_windows) {
        window->refresh_empty_state();
    }
}

void sheen_app::reload_settings() {
    sheen_settings saved;
    m_settings_problems = read_settings(QString::fromLatin1(SETTINGS_PATH), &saved);
    m_settings = saved;
    set_text_size(m_settings.font_size);

    for (const std::unique_ptr<sheen_window>& window : m_windows) {
        window->apply_settings(m_settings, m_settings_problems);
    }
}

void sheen_app::notify_dependencies_changed() {
    for (const std::unique_ptr<sheen_window>& window : m_windows) {
        window->on_dependencies_changed();
    }
}

std::shared_ptr<shared_service> sheen_app::acquire_service(
    const QString& key,
    const std::function<std::shared_ptr<shared_service>()>& factory) {
    for (auto it = m_services.begin(); it != m_services.end();) {
        if (it.value().expired()) {
            it = m_services.erase(it);
        } else {
            ++it;
        }
    }

    if (std::shared_ptr<shared_service> existing = m_services.value(key).lock()) {
        return existing;
    }

    std::shared_ptr<shared_service> created = factory ? factory() : nullptr;
    if (created) {
        m_services.insert(key, created);
    }

    return created;
}

void sheen_app::reap_orphan_process(int proc_handle, int pid, int grace_ms) {
    if (proc_handle < 0) {
        return;
    }

    pollfd status_poll = { proc_handle, POLLIN, 0 };
    if (::poll(&status_poll, 1, 0) > 0 && (status_poll.revents & (POLLIN | POLLHUP))) {
        reap_handle_blocking(proc_handle);
        return;
    }

    if (!s_active_app) {
        proc_kill(proc_handle);
        reap_handle_blocking(proc_handle);
        return;
    }

    s_active_app->enqueue_orphan_process(proc_handle, pid, grace_ms);
}

void sheen_app::enqueue_orphan_process(int proc_handle, int pid, int grace_ms) {
    bool send_kill_now = grace_ms <= 0;
    if (send_kill_now) {
        proc_kill(proc_handle);
    }

    qint64 deadline = QDateTime::currentMSecsSinceEpoch() + qMax(0, grace_ms);
    m_orphan_processes.append({ proc_handle, pid, deadline, send_kill_now });
    if (!m_orphan_timer.isActive()) {
        m_orphan_timer.start();
    }
}

void sheen_app::poll_orphan_processes() {
    qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    for (qsizetype i = m_orphan_processes.size() - 1; i >= 0; i--) {
        orphan_process& entry = m_orphan_processes[i];
        pollfd status_poll = { entry.proc_handle, POLLIN, 0 };
        if (::poll(&status_poll, 1, 0) > 0 && (status_poll.revents & (POLLIN | POLLHUP))) {
            int handle = entry.proc_handle;
            m_orphan_processes.removeAt(i);
            reap_handle_blocking(handle);
            continue;
        }

        if (!entry.kill_sent && now_ms >= entry.kill_deadline_ms) {
            proc_kill(entry.proc_handle);
            entry.kill_sent = true;
        }
    }

    if (m_orphan_processes.isEmpty()) {
        m_orphan_timer.stop();
    }
}

} // namespace sheen
