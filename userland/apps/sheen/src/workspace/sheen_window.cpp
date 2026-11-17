#include "workspace/sheen_window.h"
#include "app/sheen_app.h"
#include "core/editor_core.h"
#include "ui/status_bar.h"
#include "workspace/workspace_shell.h"

#include <QCloseEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QScreen>
#include <QVBoxLayout>
#include <QWidget>

namespace sheen {

static constexpr int WINDOW_W = 1520;
static constexpr int WINDOW_H = 920;
static constexpr int WINDOW_MIN_W = 720;
static constexpr int WINDOW_MIN_H = 480;

sheen_window::sheen_window(sheen_app& app) : m_app(app) {
    auto* central = new QWidget(this);
    m_central_layout = new QVBoxLayout(central);
    m_central_layout->setContentsMargins(0, 0, 0, 0);
    m_central_layout->setSpacing(0);

    setWindowTitle(QStringLiteral("Sheen"));
    m_editor = new editor_core(this, m_app, central);
    m_editor->on_title_changed = [this](const QString& title) { update_window_title(title); };
    m_editor->on_open_folder_requested = [this](const QString& root_path) {
        m_app.open_workspace(root_path, this);
    };

    m_central_layout->addWidget(m_editor, 1);
    m_central_layout->addWidget(m_editor->status_widget());
    setCentralWidget(central);

    setAcceptDrops(true);
    resize(QSize(WINDOW_W, WINDOW_H).boundedTo(screen()->availableGeometry().size()));
    setMinimumSize(WINDOW_MIN_W, WINDOW_MIN_H);
}

sheen_window::~sheen_window() = default;

QString sheen_window::workspace_root() const {
    return m_workspace ? m_workspace->root_path() : QString();
}

void sheen_window::promote_to_workspace(const QString& root_path) {
    if (m_workspace) {
        return;
    }

    m_editor->drop_pristine_untitled_tabs();
    m_central_layout->removeWidget(m_editor);

    m_workspace = std::make_unique<workspace_shell>(m_app, *m_editor, root_path, centralWidget());
    m_central_layout->insertWidget(0, m_workspace.get(), 1);

    if (m_editor->document_count() == 0) {
        QDir dir(m_workspace->root_path());
        QString sketch_ino = dir.filePath(m_workspace->folder_name() + QStringLiteral(".ino"));
        QString readme = dir.filePath(QStringLiteral("README.md"));
        if (QFileInfo(sketch_ino).isFile()) {
            m_editor->open_paths({ sketch_ino });
        } else if (QFileInfo(readme).isFile()) {
            m_editor->open_paths({ readme });
        } else {
            m_editor->show_welcome_state();
        }
    }

    update_window_title(m_editor->active_tab_title());
    m_editor->focus_active_surface();
}

void sheen_window::update_window_title(const QString& active_doc_title) {
    if (m_workspace) {
        if (active_doc_title.isEmpty() || active_doc_title == QLatin1String("Sheen")) {
            setWindowTitle(m_workspace->folder_name());
        } else {
            setWindowTitle(QStringLiteral("%1 - %2").arg(active_doc_title, m_workspace->folder_name()));
        }

        return;
    }

    setWindowTitle(active_doc_title.isEmpty() ? QStringLiteral("Sheen") : active_doc_title);
}

void sheen_window::open_paths(const QStringList& paths) {
    if (paths.size() == 1 && QFileInfo(paths.first()).isDir() && !m_workspace && m_editor->document_count() == 0) {
        m_app.open_workspace(QFileInfo(paths.first()).absoluteFilePath(), this);
        return;
    }

    m_editor->open_paths(paths);
}

// Raised before the files open, since the desktop drops an activation that arrives too long after the user's press
void sheen_window::receive_paths(const QStringList& paths) {
    if (isMinimized()) {
        showNormal();
    }

    raise();
    activateWindow();
    open_paths(paths);
}

bool sheen_window::confirm_close(bool is_quitting_app) {
    if (m_close_confirmed) {
        return true;
    }

    if (m_editor->has_open_sheet()) {
        return false;
    }

    QString purpose = (is_quitting_app || m_app.window_count() <= 1) ? QStringLiteral("quitting")
                                                                     : QStringLiteral("closing");
    if (!m_editor->confirm_close(purpose)) {
        return false;
    }

    m_close_confirmed = true;

    return true;
}

void sheen_window::apply_settings(const sheen_settings& settings, const QStringList& problems) {
    m_editor->apply_settings(settings, problems);
}

void sheen_window::refresh_empty_state() {
    m_editor->refresh_empty_state_if_shown();
}

void sheen_window::on_dependencies_changed() {
    if (m_workspace) {
        m_workspace->on_dependencies_changed();
    }
}

QString sheen_window::window_label() const {
    if (m_workspace) {
        return m_workspace->folder_name();
    }

    QString title = windowTitle();
    return title.isEmpty() ? QStringLiteral("Sheen") : title;
}

QString sheen_window::window_note() const {
    if (m_workspace) {
        return m_workspace->root_path();
    }

    int count = m_editor->document_count();
    if (count == 0) {
        return QStringLiteral("Welcome");
    }

    return count == 1 ? QStringLiteral("1 tab") : QStringLiteral("%1 tabs").arg(count);
}

void sheen_window::closeEvent(QCloseEvent* event) {
    if (!confirm_close(false)) {
        event->ignore();
        return;
    }

    if (m_workspace) {
        m_workspace->shutdown_for_close();
    }

    event->accept();
    m_app.remove_window(this);
}

void sheen_window::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        m_app.note_window_activated(this);
        m_editor->handle_window_activated();
    }
}

void sheen_window::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier && m_editor->handle_escape_key()) {
        return;
    }

    QMainWindow::keyPressEvent(event);
}

void sheen_window::dragEnterEvent(QDragEnterEvent* event) {
    if (!m_editor->take_file_drag(event)) {
        QMainWindow::dragEnterEvent(event);
    }
}

void sheen_window::dragMoveEvent(QDragMoveEvent* event) {
    if (!m_editor->take_file_drag(event)) {
        QMainWindow::dragMoveEvent(event);
    }
}

void sheen_window::dropEvent(QDropEvent* event) {
    if (!m_editor->take_file_drag(event)) {
        QMainWindow::dropEvent(event);
    }
}

} // namespace sheen
