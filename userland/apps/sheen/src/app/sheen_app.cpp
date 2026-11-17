#include "app/sheen_app.h"
#include "core/editor_core.h"
#include "core/kiln.h"
#include "workspace/sheen_window.h"

#include <QApplication>
#include <QFileInfo>
#include <QPointer>

namespace sheen {

static constexpr int RECENT_FILE_LIMIT = 20;

sheen_app::sheen_app() {
    m_settings_problems = read_settings(QString::fromLatin1(SETTINGS_PATH), &m_settings);
    set_text_size(m_settings.font_size);

    m_instance.on_paths = [this](const QStringList& handed) { receive_paths(handed); };
    m_instance.listen();
}

sheen_app::~sheen_app() = default;

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

sheen_window* sheen_app::target_window() {
    if (!m_recent_windows.isEmpty()) {
        return m_recent_windows.first();
    }

    sheen_window* window = create_window();
    window->show();

    return window;
}

void sheen_app::receive_paths(const QStringList& paths) {
    if (paths.isEmpty()) {
        target_window()->receive_paths({});
        return;
    }

    QStringList unopened;
    for (const QString& path : paths) {
        QFileInfo info(path);
        if (!info.isDir() && focus_open_document(info.absoluteFilePath())) {
            continue;
        }

        unopened.append(path);
    }

    if (!unopened.isEmpty()) {
        target_window()->receive_paths(unopened);
    }
}

void sheen_app::remember_recent_file(const QString& path) {
    m_recent_opened.insert(path, QDateTime::currentDateTime());
    m_recent_files.removeAll(path);
    m_recent_files.prepend(path);
    while (m_recent_files.size() > RECENT_FILE_LIMIT) {
        m_recent_files.removeLast();
    }
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

} // namespace sheen
