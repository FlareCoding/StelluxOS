#include "workspace/dock_host.h"
#include "ui/chrome.h"
#include "ui/ui_kit.h"

#include <QAbstractButton>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>

namespace sheen {

static constexpr int COLLAPSED_HEIGHT = 32;
static constexpr int TAB_BAR_INSET_X = 8;
static constexpr int TAB_BUTTON_PADDING_X = 10;
static constexpr int TAB_DOT_SIZE = 6;
static constexpr int TAB_GAP = 6;
static constexpr int PROBLEMS_PADDING_X = TAB_BAR_INSET_X + TAB_BUTTON_PADDING_X;
static constexpr int PROBLEMS_PADDING_Y = 8;
static constexpr int BADGE_PADDING_X = 8;
static constexpr int BADGE_HEIGHT = 16;

namespace {

class dock_tab_button : public QAbstractButton {
public:
    dock_tab_button(const QString& title, bool is_selected, bool has_dot, int error_count, bool is_popped,
                    QWidget* parent)
        : QAbstractButton(parent), m_is_selected(is_selected), m_has_dot(has_dot), m_error_count(error_count),
          m_is_popped(is_popped) {
        setText(title);
        setFixedHeight(COLLAPSED_HEIGHT - 1);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
    }

    QSize sizeHint() const override {
        QFont font = ui_font(STLXTHEME_BODY_SIZE, m_is_selected ? QFont::DemiBold : QFont::Medium);
        int width = 2 * TAB_BUTTON_PADDING_X + qCeil(QFontMetricsF(font).horizontalAdvance(text()));
        if (m_has_dot) {
            width += TAB_DOT_SIZE + TAB_GAP;
        }

        if (m_error_count > 0) {
            QString count_text = QString::number(m_error_count);
            int count_advance = qCeil(QFontMetricsF(mono_font(STLXTHEME_CAPTION_SIZE)).horizontalAdvance(count_text));
            width += TAB_GAP + count_advance + BADGE_PADDING_X;
        } else if (m_is_popped) {
            width += TAB_GAP + qCeil(QFontMetricsF(mono_font(STLXTHEME_CAPTION_SIZE)).horizontalAdvance(
                                   QStringLiteral("Window")));
        }

        return QSize(width, COLLAPSED_HEIGHT - 1);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        if (underMouse() && !m_is_selected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(token(STLXTHEME_SURFACE_HOVER));
            painter.drawRoundedRect(rect().adjusted(2, 3, -2, -3), STLXTHEME_INNER_RADIUS, STLXTHEME_INNER_RADIUS);
        }

        if (m_is_selected) {
            painter.fillRect(QRect(4, 0, width() - 8, STLXTHEME_FOCUS_RULE_WIDTH), token(STLXTHEME_ACCENT));
        }

        int x = TAB_BUTTON_PADDING_X;
        if (m_has_dot) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(token(STLXTHEME_ACCENT));
            painter.drawEllipse(QPointF(x + TAB_DOT_SIZE / 2.0, height() / 2.0), TAB_DOT_SIZE / 2.0, TAB_DOT_SIZE / 2.0);
            x += TAB_DOT_SIZE + TAB_GAP;
        }

        QFont font = ui_font(STLXTHEME_BODY_SIZE, m_is_selected ? QFont::DemiBold : QFont::Medium);
        painter.setFont(font);
        painter.setPen(token(m_is_selected ? STLXTHEME_TEXT : underMouse() ? STLXTHEME_TEXT_SECONDARY : STLXTHEME_TEXT_DIM));
        painter.drawText(QRect(x, 0, width() - x - TAB_BUTTON_PADDING_X, height()),
                         Qt::AlignLeft | Qt::AlignVCenter, text());
        x += painter.fontMetrics().horizontalAdvance(text()) + TAB_GAP;

        if (m_error_count > 0) {
            QString badge = QString::number(m_error_count);
            QFont mono = mono_font(STLXTHEME_CAPTION_SIZE);
            int badge_width = QFontMetrics(mono).horizontalAdvance(badge) + BADGE_PADDING_X;
            QRect pill(x, (height() - BADGE_HEIGHT) / 2, badge_width, BADGE_HEIGHT);

            painter.setPen(Qt::NoPen);
            painter.setBrush(token(STLXTHEME_DANGER));
            painter.drawRoundedRect(pill, STLXTHEME_CHIP_RADIUS, STLXTHEME_CHIP_RADIUS);
            painter.setFont(mono);
            painter.setPen(token(STLXTHEME_DANGER_TEXT));
            painter.drawText(pill, Qt::AlignCenter, badge);
        } else if (m_is_popped) {
            painter.setFont(mono_font(STLXTHEME_CAPTION_SIZE));
            painter.setPen(token(STLXTHEME_TEXT_DIM));
            painter.drawText(QRect(x, 0, width() - x, height()), Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("Window"));
        }
    }

private:
    bool m_is_selected;
    bool m_has_dot;
    int m_error_count;
    bool m_is_popped;
};

} // namespace

static QWidget* make_popped_notice(const QString& title, const std::function<void()>& on_dock_back, QWidget* parent) {
    auto* box = new QWidget(parent);
    auto* layout = new QVBoxLayout(box);
    layout->addStretch(1);

    auto* label = new QLabel(QStringLiteral("%1 is open in a separate window.").arg(title), box);
    label->setFont(ui_font(STLXTHEME_BODY_SIZE));
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                             .arg(token(STLXTHEME_TEXT_DIM).name()));
    layout->addWidget(label);

    auto* button = new ui::ui_pill_button(QStringLiteral("Dock Back"), box);
    QObject::connect(button, &QAbstractButton::clicked, box, [on_dock_back] {
        if (on_dock_back) {
            on_dock_back();
        }
    });
    layout->addWidget(button, 0, Qt::AlignHCenter);
    layout->addStretch(1);

    return box;
}

dock_host::dock_host(QWidget* parent) : QWidget(parent), m_expanded_height(DEFAULT_DOCK_HEIGHT) {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    m_tab_bar = new QWidget(this);
    m_tab_bar->setFixedHeight(COLLAPSED_HEIGHT);
    auto* bar_layout = new QHBoxLayout(m_tab_bar);
    bar_layout->setContentsMargins(TAB_BAR_INSET_X, 1, TAB_BAR_INSET_X, 0);
    bar_layout->setSpacing(4);

    m_tab_layout = new QHBoxLayout;
    m_tab_layout->setContentsMargins(0, 0, 0, 0);
    m_tab_layout->setSpacing(2);
    bar_layout->addLayout(m_tab_layout);
    bar_layout->addStretch(1);

    m_pop_button = new ui::ui_pill_button(QStringLiteral("Pop Out"), m_tab_bar);
    bar_layout->addWidget(m_pop_button, 0, Qt::AlignVCenter);

    m_collapse_button = new ui::ui_pill_button(QStringLiteral("Hide"), m_tab_bar);
    connect(m_collapse_button, &QAbstractButton::clicked, this, [this] {
        set_collapsed(!m_is_collapsed);
    });
    bar_layout->addWidget(m_collapse_button, 0, Qt::AlignVCenter);

    root_layout->addWidget(m_tab_bar);

    m_stack = new QStackedWidget(this);
    root_layout->addWidget(m_stack, 1);

    auto* scroll = new QScrollArea(m_stack);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: %1; border: none; }")
                              .arg(token(STLXTHEME_OPAQUE_POCKET).name()));

    m_problems_widget = new QWidget(scroll);
    m_problems_widget->setStyleSheet(QStringLiteral("background: %1;").arg(token(STLXTHEME_OPAQUE_POCKET).name()));
    m_problems_layout = new QVBoxLayout(m_problems_widget);
    m_problems_layout->setContentsMargins(PROBLEMS_PADDING_X, PROBLEMS_PADDING_Y,
                                          PROBLEMS_PADDING_X, PROBLEMS_PADDING_Y);
    m_problems_layout->setSpacing(4);
    scroll->setWidget(m_problems_widget);

    QString problems_key = make_key(QStringLiteral("__core__"), QStringLiteral("problems"));
    m_stack->addWidget(scroll);
    m_panels.append({ problems_key, QStringLiteral("__core__"),
                      { QStringLiteral("problems"), QStringLiteral("Problems"), scroll, false, 0 }, nullptr });
    m_active_key = problems_key;

    set_problems({}, nullptr);
    rebuild_tab_buttons();
    sync_geometry_and_controls();
}

dock_host::~dock_host() {
    shutdown_for_close();
}

QString dock_host::make_key(const QString& owner_id, const QString& panel_id) {
    return owner_id + QLatin1Char(':') + panel_id;
}

int dock_host::find_panel_index(const QString& key) const {
    for (int i = 0; i < m_panels.size(); i++) {
        if (m_panels.at(i).key == key) {
            return i;
        }
    }

    return -1;
}

bool dock_host::is_popped_out(const QString& key) const {
    return m_popped_windows.find(key.toStdString()) != m_popped_windows.end();
}

bool dock_host::has_any_docked_panel() const {
    for (const dock_panel_entry& entry : m_panels) {
        if (!is_popped_out(entry.key)) {
            return true;
        }
    }

    return false;
}

void dock_host::set_expanded_height(int height) {
    m_expanded_height = std::clamp(height, MIN_DOCK_HEIGHT, MAX_DOCK_HEIGHT);
    sync_geometry_and_controls();
}

void dock_host::set_collapsed(bool collapsed) {
    if (!collapsed && !has_any_docked_panel()) {
        return;
    }

    if (m_is_collapsed == collapsed) {
        return;
    }

    m_is_collapsed = collapsed;
    rebuild_tab_buttons();
    sync_geometry_and_controls();
    if (on_layout_changed) {
        on_layout_changed();
    }
}

void dock_host::restore_active_panel_key(const QString& key) {
    if (find_panel_index(key) < 0) {
        return;
    }

    m_active_key = key;
    rebuild_tab_buttons();
    sync_geometry_and_controls();
}

void dock_host::select_panel_by_key(const QString& key) {
    int index = find_panel_index(key);
    if (index < 0) {
        return;
    }

    m_active_key = key;
    if (is_popped_out(key)) {
        auto it = m_popped_windows.find(key.toStdString());
        if (it != m_popped_windows.end() && it->second) {
            it->second->raise();
            it->second->activateWindow();
        }
    } else if (m_is_collapsed) {
        m_is_collapsed = false;
    }

    rebuild_tab_buttons();
    sync_geometry_and_controls();
    if (on_layout_changed) {
        on_layout_changed();
    }
}

void dock_host::set_problems(const QList<diagnostic_item>& problems,
                             const std::function<void(const QString& path, int line)>& on_open_location) {
    while (QLayoutItem* item = m_problems_layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }

        delete item;
    }

    int index = find_panel_index(make_key(QStringLiteral("__core__"), QStringLiteral("problems")));
    if (index >= 0) {
        m_panels[index].spec.error_badge_count = static_cast<int>(problems.size());
    }

    if (problems.isEmpty()) {
        auto* empty = new QLabel(QStringLiteral("No problems in workspace."), m_problems_widget);
        empty->setFont(ui_font(STLXTHEME_BODY_SIZE));
        empty->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                 .arg(token(STLXTHEME_TEXT_DIM).name()));

        m_problems_layout->addWidget(empty);
        m_problems_layout->addStretch(1);
        rebuild_tab_buttons();
        return;
    }

    for (const diagnostic_item& item : problems) {
        auto* row_box = new QWidget(m_problems_widget);
        auto* row_layout = new QHBoxLayout(row_box);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(6);

        auto* bar = new ui::ui_inline_diagnostic(item, row_box);
        row_layout->addWidget(bar, 1);

        if (!item.file_path.isEmpty() && on_open_location) {
            QString loc = QStringLiteral("%1:%2").arg(QFileInfo(item.file_path).fileName()).arg(item.line);
            auto* jump = new ui::ui_pill_button(loc, row_box);
            QString path = item.file_path;
            int line = item.line;
            connect(jump, &QAbstractButton::clicked, row_box, [on_open_location, path, line] {
                on_open_location(path, line);
            });
            row_layout->addWidget(jump, 0, Qt::AlignVCenter);
        }

        m_problems_layout->addWidget(row_box);
    }

    m_problems_layout->addStretch(1);
    rebuild_tab_buttons();
}

void dock_host::tear_down_popped_window(const QString& key) {
    auto it = m_popped_windows.find(key.toStdString());
    if (it == m_popped_windows.end()) {
        return;
    }

    std::unique_ptr<dock_window> floating = std::move(it->second);
    m_popped_windows.erase(it);

    floating->set_closing_from_host(true);
    floating->on_dock_back_requested = nullptr;
    if (QWidget* panel_widget = floating->release_panel_widget()) {
        panel_widget->hide();
        panel_widget->setParent(nullptr);
    }

    floating->close();
    floating.release()->deleteLater();
}

void dock_host::set_extension_panels(const QString& extension_id, const QList<dock_panel_spec>& panels) {
    QList<QString> incoming_keys;
    QList<QWidget*> incoming_widgets;
    for (const dock_panel_spec& spec : panels) {
        if (spec.widget && !spec.id.isEmpty()) {
            incoming_keys.append(make_key(extension_id, spec.id));
            incoming_widgets.append(spec.widget);
        }
    }

    for (qsizetype i = m_panels.size() - 1; i >= 0; i--) {
        if (m_panels.at(i).owner_id != extension_id) {
            continue;
        }

        dock_panel_entry& existing = m_panels[i];
        if (!incoming_keys.contains(existing.key) || !incoming_widgets.contains(existing.spec.widget)) {
            tear_down_popped_window(existing.key);
            if (existing.popped_placeholder) {
                m_stack->removeWidget(existing.popped_placeholder);
                existing.popped_placeholder->deleteLater();
            }

            if (existing.spec.widget && !incoming_widgets.contains(existing.spec.widget)) {
                m_stack->removeWidget(existing.spec.widget);
                existing.spec.widget->hide();
                existing.spec.widget->setParent(nullptr);
                existing.spec.widget->deleteLater();
            }

            m_panels.removeAt(i);
        }
    }

    bool added_first_ext_panel = (m_panels.size() == 1);
    for (const dock_panel_spec& spec : panels) {
        if (!spec.widget || spec.id.isEmpty()) {
            continue;
        }

        QString key = make_key(extension_id, spec.id);
        int idx = find_panel_index(key);
        if (idx >= 0) {
            m_panels[idx].spec = spec;
            auto it = m_popped_windows.find(key.toStdString());
            if (it != m_popped_windows.end() && it->second) {
                it->second->setWindowTitle(spec.title);
            }
        } else {
            m_stack->addWidget(spec.widget);
            m_panels.append({ key, extension_id, spec, nullptr });
        }
    }

    if (added_first_ext_panel && m_panels.size() > 1
        && m_active_key == make_key(QStringLiteral("__core__"), QStringLiteral("problems"))) {
        m_active_key = m_panels.at(1).key;
    }

    if (find_panel_index(m_active_key) < 0 && !m_panels.isEmpty()) {
        m_active_key = m_panels.first().key;
    }

    rebuild_tab_buttons();
    sync_geometry_and_controls();
}

void dock_host::remove_extension_panels(const QString& extension_id) {
    set_extension_panels(extension_id, {});
}

void dock_host::update_panel_state(const QString& extension_id, const QString& panel_id, bool has_activity_dot,
                                   int error_count) {
    int index = find_panel_index(make_key(extension_id, panel_id));
    if (index < 0) {
        return;
    }

    m_panels[index].spec.has_activity_dot = has_activity_dot;
    m_panels[index].spec.error_badge_count = error_count;
    rebuild_tab_buttons();
}

void dock_host::reveal_panel(const QString& extension_id, const QString& panel_id) {
    select_panel_by_key(make_key(extension_id, panel_id));
}

void dock_host::pop_out_active_panel() {
    pop_out(m_active_key);
}

void dock_host::pop_out(const QString& key) {
    int index = find_panel_index(key);
    if (index < 0 || is_popped_out(key)) {
        return;
    }

    dock_panel_entry& entry = m_panels[index];
    QWidget* panel_widget = entry.spec.widget;
    if (!panel_widget) {
        return;
    }

    m_stack->removeWidget(panel_widget);
    if (!entry.popped_placeholder) {
        entry.popped_placeholder = make_popped_notice(
            entry.spec.title, [this, key] { dock_back(key, false); }, m_stack);
        m_stack->addWidget(entry.popped_placeholder);
    }

    auto floating = std::make_unique<dock_window>(key, entry.spec.title);
    floating->on_dock_back_requested = [this, key](bool from_close) { dock_back(key, from_close); };
    floating->take_panel_widget(panel_widget);
    dock_window* raw = floating.get();
    m_popped_windows[key.toStdString()] = std::move(floating);

    bool switched_to_docked = false;
    for (const dock_panel_entry& candidate : m_panels) {
        bool has_useful_content = candidate.owner_id != QLatin1String("__core__")
                               || candidate.spec.error_badge_count > 0;
        if (!is_popped_out(candidate.key) && has_useful_content) {
            m_active_key = candidate.key;
            switched_to_docked = true;
            break;
        }
    }

    if (!switched_to_docked) {
        m_is_collapsed = true;
    }

    rebuild_tab_buttons();
    sync_geometry_and_controls();
    raw->show();
    raw->raise();
    raw->activateWindow();

    if (on_layout_changed) {
        on_layout_changed();
    }
}

void dock_host::dock_back(const QString& panel_key, bool from_window_close) {
    if (m_closing_workspace) {
        return;
    }

    auto it = m_popped_windows.find(panel_key.toStdString());
    if (it == m_popped_windows.end()) {
        return;
    }

    std::unique_ptr<dock_window> floating = std::move(it->second);
    m_popped_windows.erase(it);

    floating->set_closing_from_host(true);
    floating->on_dock_back_requested = nullptr;
    QWidget* panel_widget = floating->release_panel_widget();

    int index = find_panel_index(panel_key);
    if (index >= 0 && panel_widget) {
        dock_panel_entry& entry = m_panels[index];
        if (entry.popped_placeholder) {
            m_stack->removeWidget(entry.popped_placeholder);
            entry.popped_placeholder->deleteLater();
            entry.popped_placeholder = nullptr;
        }

        m_stack->addWidget(panel_widget);
        panel_widget->show();
        m_active_key = panel_key;
        m_is_collapsed = false;
    }

    if (!from_window_close) {
        floating->close();
    }

    floating.release()->deleteLater();

    rebuild_tab_buttons();
    sync_geometry_and_controls();
    if (on_layout_changed) {
        on_layout_changed();
    }
}

void dock_host::shutdown_for_close() {
    m_closing_workspace = true;
    for (auto& [key, window] : m_popped_windows) {
        if (!window) {
            continue;
        }

        window->set_closing_from_host(true);
        window->on_dock_back_requested = nullptr;
        if (QWidget* panel = window->release_panel_widget()) {
            panel->setParent(m_stack);
        }

        window->close();
    }

    m_popped_windows.clear();
}

void dock_host::rebuild_tab_buttons() {
    while (QLayoutItem* item = m_tab_layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }

        delete item;
    }

    for (const dock_panel_entry& entry : m_panels) {
        bool selected = entry.key == m_active_key && !m_is_collapsed;
        bool popped = is_popped_out(entry.key);
        auto* button = new dock_tab_button(entry.spec.title, selected, entry.spec.has_activity_dot,
                                           entry.spec.error_badge_count, popped, m_tab_bar);
        QString key = entry.key;
        connect(button, &QAbstractButton::clicked, this, [this, key] {
            if (m_active_key == key && !is_popped_out(key)) {
                set_collapsed(!m_is_collapsed);
            } else {
                select_panel_by_key(key);
            }
        });
        m_tab_layout->addWidget(button);
    }
}

void dock_host::sync_geometry_and_controls() {
    if (!has_any_docked_panel()) {
        m_is_collapsed = true;
    }

    int index = find_panel_index(m_active_key);
    if (index >= 0) {
        const dock_panel_entry& active = m_panels.at(index);
        QWidget* target = is_popped_out(active.key) ? active.popped_placeholder : active.spec.widget;
        if (target) {
            m_stack->setCurrentWidget(target);
        }
    }

    bool active_popped = is_popped_out(m_active_key);
    if (!active_popped && m_popped_windows.size() == 1 && m_is_collapsed) {
        QString only_popped = QString::fromStdString(m_popped_windows.begin()->first);
        m_pop_button->setText(QStringLiteral("Dock Back"));
        m_pop_button->disconnect();
        connect(m_pop_button, &QAbstractButton::clicked, this, [this, only_popped] {
            dock_back(only_popped, false);
        });
    } else {
        m_pop_button->setText(active_popped ? QStringLiteral("Dock Back") : QStringLiteral("Pop Out"));
        m_pop_button->disconnect();
        connect(m_pop_button, &QAbstractButton::clicked, this, [this] {
            if (is_popped_out(m_active_key)) {
                dock_back(m_active_key, false);
            } else {
                pop_out_active_panel();
            }
        });
    }

    bool can_toggle_collapse = has_any_docked_panel() && !(active_popped && m_is_collapsed);
    m_collapse_button->setVisible(can_toggle_collapse);
    m_collapse_button->setText(m_is_collapsed ? QStringLiteral("Show") : QStringLiteral("Hide"));

    m_stack->setVisible(!m_is_collapsed);
    setFixedHeight(m_is_collapsed ? COLLAPSED_HEIGHT : m_expanded_height);
    update();
}

void dock_host::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), token(STLXTHEME_SHADE));
    painter.fillRect(QRect(0, 0, width(), 1), token(STLXTHEME_LINE));
    if (!m_is_collapsed) {
        painter.fillRect(QRect(0, COLLAPSED_HEIGHT - 1, width(), 1), token(STLXTHEME_LINE));
    }
}

} // namespace sheen
