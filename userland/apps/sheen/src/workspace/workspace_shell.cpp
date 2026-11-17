#include "workspace/workspace_shell.h"
#include "app/sheen_app.h"
#include "core/editor_core.h"
#include "extensions/registry.h"
#include "ui/chrome.h"
#include "ui/menu_style.h"
#include "ui/tab_strip.h"
#include "ui/ui_kit.h"
#include "workspace/action_strip.h"
#include "workspace/dock_host.h"
#include "workspace/extension_setup_page.h"
#include "workspace/extensions_popover.h"
#include "workspace/sidebar_host.h"

#include <QAbstractButton>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace sheen {

static constexpr int WORKSPACE_BAR_HEIGHT = 34;
static constexpr int OFFER_BAR_HEIGHT = 36;
static constexpr int BAR_INSET_X = 8;

static QString resolve_in_workspace(const QString& path, const QString& root_path) {
    if (path.isEmpty()) {
        return {};
    }

    QFileInfo info(path);
    if (info.isAbsolute() || root_path.isEmpty()) {
        return info.absoluteFilePath();
    }

    return QDir(root_path).absoluteFilePath(path);
}

namespace {

/* Scoped host implementation isolating one extension session's UI, callbacks, and editor state by extension_id. */
class extension_host_adapter final : public extension_host {
public:
    extension_host_adapter(workspace_shell& shell, const QString& extension_id, const QString& extension_name,
                           std::shared_ptr<bool> alive)
        : m_shell(shell), m_extension_id(extension_id), m_extension_name(extension_name),
          m_alive(std::move(alive)) {
    }

    QString workspace_root() const override {
        return m_shell.root_path();
    }

    QString active_file_path() const override {
        return m_shell.editor().active_file_path();
    }

    void set_strip(const strip_spec& strip) override {
        if (!*m_alive) {
            return;
        }

        strip_spec guarded = strip;
        std::shared_ptr<bool> alive = m_alive;

        for (picker_spec& picker : guarded.pickers) {
            if (picker.list_choices) {
                auto list_fn = std::move(picker.list_choices);
                picker.list_choices = [alive, list_fn] {
                    return *alive ? list_fn() : QList<picker_choice>{};
                };
            }

            if (picker.on_selected) {
                auto select_fn = std::move(picker.on_selected);
                picker.on_selected = [alive, select_fn](const QString& choice_id) {
                    if (*alive) {
                        select_fn(choice_id);
                    }
                };
            }
        }

        for (button_spec& button : guarded.buttons) {
            if (button.on_clicked) {
                auto click_fn = std::move(button.on_clicked);
                button.on_clicked = [alive, click_fn] {
                    if (*alive) {
                        click_fn();
                    }
                };
            }
        }

        m_shell.strip().set_extension_strip(m_extension_id, m_extension_name, guarded);
    }

    void set_sidebar_sections(const QList<sidebar_section_spec>& sections) override {
        if (*m_alive) {
            m_shell.sidebar().set_extension_sections(m_extension_id, sections);
        }
    }

    void set_dock_panels(const QList<dock_panel_spec>& panels) override {
        if (*m_alive) {
            m_shell.dock().set_extension_panels(m_extension_id, panels);
        }
    }

    void update_dock_state(const QString& panel_id, bool has_activity_dot, int error_count) override {
        if (*m_alive) {
            m_shell.dock().update_panel_state(m_extension_id, panel_id, has_activity_dot, error_count);
        }
    }

    void reveal_dock_panel(const QString& panel_id) override {
        if (*m_alive) {
            m_shell.dock().reveal_panel(m_extension_id, panel_id);
        }
    }

    void open_custom_tab(const custom_tab_spec& tab) override {
        if (*m_alive) {
            m_shell.editor().open_custom_tab(m_extension_id, tab);
        }
    }

    void close_custom_tabs() override {
        if (*m_alive) {
            m_shell.editor().close_custom_tabs(m_extension_id);
        }
    }

    void set_commands(const QList<extension_command_spec>& commands) override {
        if (!*m_alive) {
            return;
        }

        QList<extension_command_spec> guarded = commands;
        std::shared_ptr<bool> alive = m_alive;
        for (extension_command_spec& cmd : guarded) {
            if (cmd.is_enabled) {
                auto enabled_fn = std::move(cmd.is_enabled);
                cmd.is_enabled = [alive, enabled_fn] {
                    return *alive && enabled_fn();
                };
            } else {
                cmd.is_enabled = [alive] { return *alive; };
            }

            if (cmd.on_triggered) {
                auto trigger_fn = std::move(cmd.on_triggered);
                cmd.on_triggered = [alive, trigger_fn] {
                    if (*alive) {
                        trigger_fn();
                    }
                };
            }
        }

        m_shell.editor().set_extension_commands(m_extension_id, guarded);
    }

    void show_status_note(const QString& note) override {
        if (*m_alive) {
            m_shell.editor().show_extension_status_note(note);
        }
    }

    bool save_modified_files() override {
        return *m_alive && m_shell.editor().save_modified_files();
    }

    void open_file_at_line(const QString& path, int line, int column) override {
        if (*m_alive) {
            m_shell.editor().open_file_at_line(resolve_in_workspace(path, m_shell.root_path()), line, column);
        }
    }

    void set_diagnostics(const QList<diagnostic_item>& diagnostics) override {
        if (!*m_alive) {
            return;
        }

        QList<diagnostic_item> guarded = diagnostics;
        std::shared_ptr<bool> alive = m_alive;
        for (diagnostic_item& item : guarded) {
            item.file_path = resolve_in_workspace(item.file_path, m_shell.root_path());
            if (item.fix.apply) {
                auto apply_fn = std::move(item.fix.apply);
                item.fix.apply = [alive, apply_fn] {
                    if (*alive) {
                        apply_fn();
                    }
                };
            }
        }

        m_shell.editor().set_diagnostics(m_extension_id, guarded);
    }

    void clear_diagnostics() override {
        if (*m_alive) {
            m_shell.editor().clear_diagnostics(m_extension_id);
        }
    }

    void set_line_badges(const QList<line_badge_item>& badges) override {
        if (!*m_alive) {
            return;
        }

        QList<line_badge_item> resolved = badges;
        for (line_badge_item& badge : resolved) {
            badge.file_path = resolve_in_workspace(badge.file_path, m_shell.root_path());
        }

        m_shell.editor().set_line_badges(m_extension_id, resolved);
    }

    void clear_line_badges() override {
        if (*m_alive) {
            m_shell.editor().clear_line_badges(m_extension_id);
        }
    }

    std::shared_ptr<shared_service> acquire_service(
        const QString& key,
        const std::function<std::shared_ptr<shared_service>()>& factory) override {
        return m_shell.app().acquire_service(key, factory);
    }

private:
    workspace_shell& m_shell;
    QString m_extension_id;
    QString m_extension_name;
    std::shared_ptr<bool> m_alive;
};

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

class offer_banner_bar : public QWidget {
public:
    explicit offer_banner_bar(QWidget* parent) : QWidget(parent) {
        setFixedHeight(OFFER_BAR_HEIGHT);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QColor tint = token(STLXTHEME_ACCENT);
        tint.setAlphaF(0.08);
        painter.fillRect(rect(), token(STLXTHEME_OPAQUE_POCKET));
        painter.fillRect(rect(), tint);
        painter.fillRect(QRect(0, height() - 1, width(), 1), token(STLXTHEME_LINE));

        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(token(STLXTHEME_ACCENT));
        painter.drawEllipse(QPointF(16, height() / 2.0), 3.0, 3.0);
    }
};

} // namespace

static const extension_descriptor* find_descriptor(const QString& extension_id) {
    for (const extension_descriptor& desc : registered_extensions()) {
        if (extension_id == QLatin1String(desc.id)) {
            return &desc;
        }
    }

    return nullptr;
}

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

    m_extensions_button = new ui::ui_pill_button(QStringLiteral("Extensions"), top_bar);
    m_extensions_button->set_has_chevron(true);
    connect(m_extensions_button, &QAbstractButton::clicked, this, [this] { open_extensions_popover(); });
    top_layout->addWidget(m_extensions_button, 0, Qt::AlignVCenter);
    root_layout->addWidget(top_bar);

    m_action_strip = new action_strip(this);
    m_action_strip->on_pin_changed = [this](const QString& pinned) {
        m_record.pinned_strip_extension = pinned.toStdString();
        persist_state();
    };

    root_layout->addWidget(m_action_strip);

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

    m_offer_bar = new offer_banner_bar(right_column);
    auto* offer_layout = new QHBoxLayout(m_offer_bar);
    offer_layout->setContentsMargins(28, 0, 12, 1);
    offer_layout->setSpacing(8);

    m_offer_label = new QLabel(m_offer_bar);
    m_offer_label->setFont(ui_font(STLXTHEME_BODY_SIZE));
    m_offer_label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                     .arg(token(STLXTHEME_TEXT).name()));
    offer_layout->addWidget(m_offer_label, 1);

    m_offer_enable_button = new ui::ui_pill_button(QStringLiteral("Enable"), m_offer_bar);
    connect(m_offer_enable_button, &QAbstractButton::clicked, this, [this] {
        if (!m_offered_extension_id.isEmpty()) {
            set_extension_enabled(m_offered_extension_id, true);
        }
    });
    offer_layout->addWidget(m_offer_enable_button, 0, Qt::AlignVCenter);

    auto* dismiss_button = new ui::ui_pill_button(QStringLiteral("Dismiss"), m_offer_bar);
    connect(dismiss_button, &QAbstractButton::clicked, this, [this] {
        if (!m_offered_extension_id.isEmpty()) {
            dismiss_offer(m_offered_extension_id);
        }
    });
    offer_layout->addWidget(dismiss_button, 0, Qt::AlignVCenter);
    m_offer_bar->hide();

    m_editor.setParent(right_column);
    m_editor.set_workspace_root(m_root_path);
    right_layout->addWidget(m_offer_bar);
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
        m_action_strip->on_active_file_changed(active_path);

        QList<std::pair<std::shared_ptr<bool>, extension_session*>> snapshot;
        for (auto& [id, running] : m_sessions) {
            if (running.session && running.alive) {
                snapshot.append({ running.alive, running.session.get() });
            }
        }

        for (const auto& [alive, session] : snapshot) {
            if (*alive) {
                session->on_active_file_changed(active_path);
            }
        }
    };

    m_editor.on_diagnostics_changed = [this] {
        sync_problems_dock();
    };

    m_editor.on_file_saved = [this](const QString& saved_path) {
        if (saved_path == m_root_path || saved_path.startsWith(m_root_path + QLatin1Char('/'))) {
            scan_workspace_files();
            m_sidebar->refresh_files();
            m_sidebar->highlight_active_file(m_editor.active_file_path());
            refresh_header_and_offer();
        }
    };

    restore_persisted_state();
    sync_problems_dock();
    m_sidebar->highlight_active_file(m_editor.active_file_path());
    refresh_header_and_offer();
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

    if (!m_record.pinned_strip_extension.empty()) {
        m_action_strip->set_pinned_extension(QString::fromStdString(m_record.pinned_strip_extension));
    }

    std::vector<std::string> enabled_copy = m_record.enabled_extensions;
    for (const std::string& id_str : enabled_copy) {
        QString ext_id = QString::fromStdString(id_str);
        const extension_descriptor* desc = find_descriptor(ext_id);
        if (desc && are_dependencies_installed(*desc)) {
            set_extension_enabled(ext_id, true);
        }
    }

    if (!m_record.active_dock_panel.empty()) {
        m_dock->restore_active_panel_key(QString::fromStdString(m_record.active_dock_panel));
    }

    m_dock->set_collapsed(m_record.is_dock_collapsed);
}

void workspace_shell::persist_state() {
    m_record.root_path = m_root_path.toStdString();
    m_record.enabled_extensions.clear();
    for (const auto& [id, running] : m_sessions) {
        if (running.session) {
            m_record.enabled_extensions.push_back(id);
        }
    }

    std::sort(m_record.enabled_extensions.begin(), m_record.enabled_extensions.end());
    m_app.store().save_workspace(m_record);
}

bool workspace_shell::is_extension_enabled(const QString& extension_id) const {
    return m_sessions.find(extension_id.toStdString()) != m_sessions.end();
}

void workspace_shell::set_extension_enabled(const QString& extension_id, bool enabled) {
    std::string key = extension_id.toStdString();
    if (enabled) {
        if (m_sessions.find(key) != m_sessions.end()) {
            return;
        }

        const extension_descriptor* desc = find_descriptor(extension_id);
        if (!desc || !desc->create_session) {
            return;
        }

        if (!are_dependencies_installed(*desc)) {
            open_extension_setup(extension_id);
            return;
        }

        auto alive = std::make_shared<bool>(true);
        auto adapter = std::make_unique<extension_host_adapter>(
            *this, extension_id, QString::fromLatin1(desc->name), alive);
        std::unique_ptr<extension_session> session = desc->create_session(*adapter);
        if (!session) {
            *alive = false;
            return;
        }

        extension_session* session_ptr = session.get();
        m_sessions[key] = { alive, std::move(adapter), std::move(session) };

        QString active_path = m_editor.active_file_path();
        if (!active_path.isEmpty() && *alive) {
            session_ptr->on_active_file_changed(active_path);
        }

        persist_state();
        refresh_header_and_offer();
        return;
    }

    auto it = m_sessions.find(key);
    if (it == m_sessions.end()) {
        return;
    }

    running_extension extracted = std::move(it->second);
    m_sessions.erase(it);

    if (extracted.alive) {
        *extracted.alive = false;
    }

    if (extracted.session) {
        extracted.session->shutdown();
    }

    m_action_strip->remove_extension_strip(extension_id);
    m_sidebar->remove_extension_sections(extension_id);
    m_dock->remove_extension_panels(extension_id);
    m_editor.close_custom_tabs(extension_id);

    m_editor.clear_extension_commands(extension_id);
    m_editor.clear_diagnostics(extension_id);
    m_editor.clear_line_badges(extension_id);

    std::shared_ptr<running_extension> deferred = std::make_shared<running_extension>(std::move(extracted));
    QTimer::singleShot(0, [deferred]() mutable { deferred.reset(); });

    persist_state();
    refresh_header_and_offer();
}

void workspace_shell::open_extensions_popover() {
    QSet<QString> enabled_ids;
    for (const auto& [id, running] : m_sessions) {
        enabled_ids.insert(QString::fromStdString(id));
    }

    auto* popover = new extensions_popover(m_extensions_button, m_files, enabled_ids);
    popover->setAttribute(Qt::WA_DeleteOnClose);
    m_open_popover = popover;
    m_extensions_button->set_is_open(true);

    popover->on_closed = [this] {
        if (m_extensions_button) {
            m_extensions_button->set_is_open(false);
        }
    };

    popover->on_toggled = [this](const QString& ext_id, bool checked) {
        set_extension_enabled(ext_id, checked);
    };

    popover->on_setup_requested = [this](const QString& ext_id) {
        open_extension_setup(ext_id);
    };

    popover->open_below_anchor();
}

void workspace_shell::open_extension_setup(const QString& focus_extension_id) {
    QString owner_id = QStringLiteral("__core_setup__");
    QString tab_id = QStringLiteral("extension_setup");
    if (QWidget* existing = m_editor.focus_custom_tab(owner_id, tab_id)) {
        static_cast<extension_setup_page*>(existing)->focus_extension(focus_extension_id);
        return;
    }

    auto* page = new extension_setup_page(m_app, focus_extension_id);
    page->on_extension_ready = [this](const QString& ready_id) {
        if (!ready_id.isEmpty()) {
            set_extension_enabled(ready_id, true);
        }
    };

    m_setup_page = page;
    m_editor.open_custom_tab(owner_id, { tab_id, QStringLiteral("Extension Setup"), page });
}

void workspace_shell::on_dependencies_changed() {
    if (m_open_popover) {
        QSet<QString> enabled_ids;
        for (const auto& [id, running] : m_sessions) {
            enabled_ids.insert(QString::fromStdString(id));
        }

        m_open_popover->refresh_rows(enabled_ids);
    }

    refresh_header_and_offer();
}

void workspace_shell::dismiss_offer(const QString& extension_id) {
    std::string id = extension_id.toStdString();
    if (std::find(m_record.dismissed_offers.begin(), m_record.dismissed_offers.end(), id)
        == m_record.dismissed_offers.end()) {
        m_record.dismissed_offers.push_back(id);
        persist_state();
    }

    refresh_header_and_offer();
}

void workspace_shell::refresh_header_and_offer() {
    int enabled_count = static_cast<int>(m_sessions.size());
    m_extensions_button->setText(enabled_count > 0 ? QStringLiteral("Extensions %1").arg(enabled_count)
                                                   : QStringLiteral("Extensions"));

    bool any_detected = false;
    m_offered_extension_id.clear();
    const extension_descriptor* offered = nullptr;

    for (const extension_descriptor& desc : registered_extensions()) {
        bool qualifies = desc.qualifies_workspace && desc.qualifies_workspace(m_files);
        any_detected = any_detected || qualifies;

        QString ext_id = QString::fromLatin1(desc.id);
        bool dismissed = std::find(m_record.dismissed_offers.begin(), m_record.dismissed_offers.end(),
                                   ext_id.toStdString())
                      != m_record.dismissed_offers.end();
        if (!offered && qualifies && desc.auto_offer && !is_extension_enabled(ext_id) && !dismissed
            && are_dependencies_installed(desc)) {
            offered = &desc;
            m_offered_extension_id = ext_id;
        }
    }

    m_extensions_button->set_checked_dot(enabled_count > 0 || any_detected);
    m_extensions_button->updateGeometry();

    if (offered) {
        QString notice = offered->detection_notice && offered->detection_notice[0]
                       ? QString::fromLatin1(offered->detection_notice)
                       : QStringLiteral("%1 project detected").arg(QString::fromLatin1(offered->name));
        m_offer_label->setText(notice);
        m_offer_enable_button->setText(QStringLiteral("Enable %1").arg(QString::fromLatin1(offered->name)));
        m_offer_bar->show();
    } else {
        m_offer_bar->hide();
    }
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
    if (m_open_popover) {
        m_open_popover->close();
    }

    std::vector<running_extension> to_stop;
    to_stop.reserve(m_sessions.size());
    for (auto& [id, running] : m_sessions) {
        if (running.alive) {
            *running.alive = false;
        }

        to_stop.push_back(std::move(running));
    }

    m_sessions.clear();
    for (running_extension& running : to_stop) {
        if (running.session) {
            running.session->shutdown();
        }
    }

    to_stop.clear();
    if (m_dock) {
        m_dock->shutdown_for_close();
    }
}

} // namespace sheen
