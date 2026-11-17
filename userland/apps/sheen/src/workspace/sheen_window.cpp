#include "workspace/sheen_window.h"
#include "app/sheen_app.h"
#include "core/editor_core.h"
#include "ui/status_bar.h"

#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
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
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    setWindowTitle(QStringLiteral("Sheen"));
    m_editor = new editor_core(this, m_app, central);
    m_editor->on_title_changed = [this](const QString& title) {
        setWindowTitle(title.isEmpty() ? QStringLiteral("Sheen") : title);
    };

    layout->addWidget(m_editor, 1);
    layout->addWidget(m_editor->status_widget());
    setCentralWidget(central);

    setAcceptDrops(true);
    resize(QSize(WINDOW_W, WINDOW_H).boundedTo(screen()->availableGeometry().size()));
    setMinimumSize(WINDOW_MIN_W, WINDOW_MIN_H);
}

sheen_window::~sheen_window() = default;

void sheen_window::open_paths(const QStringList& paths) {
    m_editor->open_paths(paths);
}

// Raised before the files open, since the desktop drops an activation that arrives too long after the user's press
void sheen_window::receive_paths(const QStringList& paths) {
    if (isMinimized()) {
        showNormal();
    }

    raise();
    activateWindow();
    m_editor->open_paths(paths);
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
}

QString sheen_window::window_label() const {
    QString title = windowTitle();
    return title.isEmpty() ? QStringLiteral("Sheen") : title;
}

QString sheen_window::window_note() const {
    int count = m_editor->document_count();
    if (count == 0) {
        return QStringLiteral("No open files");
    }

    return count == 1 ? QStringLiteral("1 tab") : QStringLiteral("%1 tabs").arg(count);
}

void sheen_window::closeEvent(QCloseEvent* event) {
    if (!confirm_close(false)) {
        event->ignore();
        return;
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
