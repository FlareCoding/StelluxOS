#include "workspace/workspace_shell.h"
#include "app/sheen_app.h"
#include "core/editor_core.h"
#include "ui/chrome.h"
#include "ui/menu_style.h"
#include "ui/tab_strip.h"
#include "workspace/dock_host.h"
#include "workspace/sidebar_host.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

namespace sheen {

static constexpr int WORKSPACE_BAR_HEIGHT = 34;
static constexpr int BAR_INSET_X = 8;

namespace {

class workspace_header_bar : public QWidget {
public:
    explicit workspace_header_bar(QWidget* parent) : QWidget(parent) {
        setFixedHeight(WORKSPACE_BAR_HEIGHT);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), token(STLXTHEME_SHADE));
        painter.fillRect(QRect(0, height() - 1, width(), 1), token(STLXTHEME_LINE));
    }
};

} // namespace

workspace_shell::workspace_shell(sheen_app& app, editor_core& editor, const QString& root_path, QWidget* parent)
    : QWidget(parent), m_app(app), m_editor(editor),
      m_root_path(QString::fromStdString(workspace_store::normalize_workspace_path(root_path.toStdString()))) {
    scan_workspace_files();

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* top_bar = new workspace_header_bar(this);
    auto* top_layout = new QHBoxLayout(top_bar);
    top_layout->setContentsMargins(BAR_INSET_X, 0, BAR_INSET_X, 1);
    top_layout->setSpacing(8);

    menu_button* editor_menu = m_editor.tab_row_widget()->menu();
    if (!m_app.settings().has_menu_bar) {
        editor_menu->setParent(top_bar);
        editor_menu->show();
        top_layout->addWidget(editor_menu, 0, Qt::AlignVCenter);
    }

    auto* folder_label = new QLabel(m_files.folder_name, top_bar);
    folder_label->setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::DemiBold));
    folder_label->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(token(STLXTHEME_TEXT).name()));
    top_layout->addWidget(folder_label, 0, Qt::AlignVCenter);

    auto* path_label = new QLabel(m_root_path, top_bar);
    path_label->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
    path_label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                  .arg(token(STLXTHEME_TEXT_DIM).name()));
    top_layout->addWidget(path_label, 0, Qt::AlignVCenter);
    top_layout->addStretch(1);
    root_layout->addWidget(top_bar);

    auto* body = new QWidget(this);
    auto* body_layout = new QHBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);

    m_sidebar = new sidebar_host(m_root_path, body);
    m_sidebar->on_file_activated = [this](const QString& path) {
        m_editor.open_paths({ path });
    };

    body_layout->addWidget(m_sidebar);

    auto* right_column = new QWidget(body);
    auto* right_layout = new QVBoxLayout(right_column);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(0);

    m_editor.setParent(right_column);
    m_editor.set_workspace_root(m_root_path);
    right_layout->addWidget(&m_editor, 1);

    m_dock = new dock_host(right_column);
    m_dock->on_layout_changed = [this] {
        m_record.dock_height = m_dock->expanded_height();
        m_record.is_dock_collapsed = m_dock->is_collapsed();
        m_record.active_dock_panel = m_dock->active_panel_key().toStdString();
        persist_state();
    };

    right_layout->addWidget(m_dock);

    body_layout->addWidget(right_column, 1);
    root_layout->addWidget(body, 1);

    m_editor.on_active_file_changed = [this](const QString& active_path) {
        m_sidebar->highlight_active_file(active_path);
    };

    m_editor.on_diagnostics_changed = [this] {
        sync_problems_dock();
    };

    m_editor.on_file_saved = [this](const QString& saved_path) {
        if (saved_path == m_root_path || saved_path.startsWith(m_root_path + QLatin1Char('/'))) {
            scan_workspace_files();
            m_sidebar->refresh_files();
            m_sidebar->highlight_active_file(m_editor.active_file_path());
        }
    };

    restore_persisted_state();
    sync_problems_dock();
    m_sidebar->highlight_active_file(m_editor.active_file_path());
}

workspace_shell::~workspace_shell() {
    shutdown_for_close();
}

void workspace_shell::scan_workspace_files() {
    m_files.root_path = m_root_path;
    QString base = QFileInfo(m_root_path).fileName();
    m_files.folder_name = base.isEmpty() ? m_root_path : base;
    m_files.entry_names.clear();

    QDir dir(m_root_path);
    for (const QString& entry : dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        m_files.entry_names.insert(entry);
    }
}

void workspace_shell::restore_persisted_state() {
    m_record = m_app.store().load_workspace(m_root_path.toStdString());
    m_sidebar->set_sidebar_width(m_record.sidebar_width);
    m_sidebar->setVisible(m_record.is_sidebar_visible);

    m_dock->set_expanded_height(m_record.dock_height);
    if (!m_record.active_dock_panel.empty()) {
        m_dock->restore_active_panel_key(QString::fromStdString(m_record.active_dock_panel));
    }

    m_dock->set_collapsed(m_record.is_dock_collapsed);
}

void workspace_shell::persist_state() {
    m_record.root_path = m_root_path.toStdString();
    m_app.store().save_workspace(m_record);
}

void workspace_shell::on_dependencies_changed() {
}

void workspace_shell::sync_problems_dock() {
    m_dock->set_problems(m_editor.current_diagnostics(), [this](const QString& path, int line) {
        m_editor.open_file_at_line(path, line);
    });
}

void workspace_shell::shutdown_for_close() {
    m_editor.on_active_file_changed = nullptr;
    m_editor.on_diagnostics_changed = nullptr;
    m_editor.on_file_saved = nullptr;
    if (m_dock) {
        m_dock->shutdown_for_close();
    }
}

} // namespace sheen
