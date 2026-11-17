#include "workspace/action_strip.h"
#include "ui/chrome.h"
#include "ui/ui_kit.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPointer>

namespace sheen {

static constexpr int STRIP_HEIGHT = 36;
static constexpr int STRIP_INSET_X = 10;
static constexpr int STRIP_GAP = 6;
static constexpr int PROGRESS_HEIGHT = 2;
static constexpr int DIVIDER_HEIGHT = 16;

namespace {

class strip_divider : public QWidget {
public:
    explicit strip_divider(QWidget* parent) : QWidget(parent) {
        setFixedSize(1, DIVIDER_HEIGHT);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), token(STLXTHEME_LINE));
    }
};

} // namespace

action_strip::action_strip(QWidget* parent) : QWidget(parent), m_layout(new QHBoxLayout(this)) {
    setFixedHeight(STRIP_HEIGHT);
    m_layout->setContentsMargins(STRIP_INSET_X, 0, STRIP_INSET_X, 1);
    m_layout->setSpacing(STRIP_GAP);
    hide();
}

int action_strip::find_strip_index(const QString& extension_id) const {
    for (int i = 0; i < m_strips.size(); i++) {
        if (m_strips.at(i).extension_id == extension_id) {
            return i;
        }
    }

    return -1;
}

void action_strip::set_extension_strip(const QString& extension_id, const QString& extension_name,
                                       const strip_spec& spec) {
    bool is_empty = spec.pickers.isEmpty() && spec.buttons.isEmpty() && spec.readouts.isEmpty();
    if (is_empty) {
        remove_extension_strip(extension_id);
        return;
    }

    int existing = find_strip_index(extension_id);
    if (existing >= 0) {
        m_strips[existing].extension_name = extension_name;
        m_strips[existing].spec = spec;
    } else {
        m_strips.append({ extension_id, extension_name, spec });
    }

    choose_active_strip(m_last_file_path);
    rebuild_widgets();
}

void action_strip::remove_extension_strip(const QString& extension_id) {
    int index = find_strip_index(extension_id);
    if (index < 0) {
        return;
    }

    if (m_active_extension == extension_id && m_open_picker) {
        m_open_picker->close();
    }

    m_strips.removeAt(index);
    if (m_pinned_extension == extension_id) {
        m_pinned_extension.clear();
    }

    if (m_active_extension == extension_id) {
        m_active_extension.clear();
    }

    choose_active_strip(m_last_file_path);
    rebuild_widgets();
}

void action_strip::set_pinned_extension(const QString& extension_id) {
    m_pinned_extension = extension_id;
    choose_active_strip(m_last_file_path);
    rebuild_widgets();
}

void action_strip::on_active_file_changed(const QString& file_path) {
    m_last_file_path = file_path;
    QString before = m_active_extension;
    choose_active_strip(file_path);
    if (m_active_extension != before) {
        rebuild_widgets();
    }
}

void action_strip::choose_active_strip(const QString& file_path) {
    if (m_strips.isEmpty()) {
        m_active_extension.clear();
        return;
    }

    if (!m_pinned_extension.isEmpty() && find_strip_index(m_pinned_extension) >= 0) {
        m_active_extension = m_pinned_extension;
        return;
    }

    if (!file_path.isEmpty()) {
        for (const registered_strip& entry : m_strips) {
            for (const QString& suffix : entry.spec.focus_extensions) {
                if (!suffix.isEmpty() && file_path.endsWith(suffix, Qt::CaseInsensitive)) {
                    m_active_extension = entry.extension_id;
                    return;
                }
            }
        }
    }

    if (find_strip_index(m_active_extension) < 0) {
        m_active_extension = m_strips.first().extension_id;
    }
}

void action_strip::rebuild_widgets() {
    while (QLayoutItem* item = m_layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }

        delete item;
    }

    m_progress_percent = -1;
    int active_idx = find_strip_index(m_active_extension);
    if (active_idx < 0) {
        hide();
        return;
    }

    const registered_strip& active = m_strips.at(active_idx);
    if (m_strips.size() > 1) {
        auto* switcher = new ui::ui_pill_button(active.extension_name, this);
        switcher->set_has_chevron(true);
        connect(switcher, &QAbstractButton::clicked, this, [this, switcher] { open_switcher_picker(switcher); });
        m_layout->addWidget(switcher, 0, Qt::AlignVCenter);
        m_layout->addWidget(new strip_divider(this), 0, Qt::AlignVCenter);
    }

    for (const picker_spec& picker : active.spec.pickers) {
        auto* button = new ui::ui_pill_button(picker.label, this);
        button->set_detail(picker.detail);
        button->set_has_chevron(true);
        button->setEnabled(picker.is_enabled);

        std::function<QList<picker_choice>()> list_fn = picker.list_choices;
        std::function<void(const QString&)> select_fn = picker.on_selected;
        QString placeholder = picker.search_placeholder;
        connect(button, &QAbstractButton::clicked, this, [this, button, list_fn, select_fn, placeholder] {
            QList<picker_choice> choices = list_fn ? list_fn() : QList<picker_choice>();
            auto* popup = new ui::ui_searchable_picker(button, placeholder, choices);
            popup->setAttribute(Qt::WA_DeleteOnClose);
            popup->on_selected = select_fn;
            m_open_picker = popup;

            QPointer<ui::ui_pill_button> guard(button);
            button->set_is_open(true);
            popup->on_closed = [guard] {
                if (guard) {
                    guard->set_is_open(false);
                }
            };

            popup->open_below_anchor();
        });

        m_layout->addWidget(button, 0, Qt::AlignVCenter);
    }

    if (!active.spec.pickers.isEmpty() && !active.spec.buttons.isEmpty()) {
        m_layout->addWidget(new strip_divider(this), 0, Qt::AlignVCenter);
    }

    for (const button_spec& btn_spec : active.spec.buttons) {
        auto* button = new ui::ui_pill_button(btn_spec.label, this);
        button->setEnabled(btn_spec.is_enabled);
        button->set_checked_dot(btn_spec.is_checked);
        if (!btn_spec.tooltip.isEmpty()) {
            button->setToolTip(btn_spec.tooltip);
        }

        std::function<void()> clicked = btn_spec.on_clicked;
        connect(button, &QAbstractButton::clicked, this, [clicked] {
            if (clicked) {
                clicked();
            }
        });

        m_layout->addWidget(button, 0, Qt::AlignVCenter);
    }

    m_layout->addStretch(1);

    for (const readout_spec& readout : active.spec.readouts) {
        if (readout.progress_percent >= 0 && readout.progress_percent <= 100) {
            m_progress_percent = readout.progress_percent;
        }

        if (readout.label.isEmpty()) {
            continue;
        }

        auto* label = new QLabel(readout.label, this);
        label->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
        uint32_t tone = readout.is_danger ? STLXTHEME_DANGER : STLXTHEME_TEXT_SECONDARY;
        label->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(token(tone).name()));
        m_layout->addWidget(label, 0, Qt::AlignVCenter);
    }

    show();
    update();
}

void action_strip::open_switcher_picker(QWidget* anchor) {
    QList<picker_choice> choices;
    for (const registered_strip& entry : m_strips) {
        QString badge = entry.extension_id == m_pinned_extension ? QStringLiteral("Pinned") : QString();
        choices.append({ entry.extension_id, entry.extension_name, {}, badge, false, false });
    }

    if (!m_pinned_extension.isEmpty()) {
        choices.append({ QStringLiteral("__auto__"), QStringLiteral("Follow Active File"), {}, {}, true, false });
    }

    auto* popup = new ui::ui_searchable_picker(anchor, QStringLiteral("Switch toolbar..."), choices);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    m_open_picker = popup;
    popup->on_selected = [this](const QString& choice_id) {
        if (choice_id == QLatin1String("__auto__")) {
            m_pinned_extension.clear();
            choose_active_strip(m_last_file_path);
        } else {
            m_pinned_extension = choice_id;
            m_active_extension = choice_id;
        }

        if (on_pin_changed) {
            on_pin_changed(m_pinned_extension);
        }

        rebuild_widgets();
    };

    popup->open_below_anchor();
}

void action_strip::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), token(STLXTHEME_SHADE));
    painter.fillRect(QRect(0, height() - 1, width(), 1), token(STLXTHEME_LINE));

    if (m_progress_percent >= 0) {
        int bar_w = width() * qBound(0, m_progress_percent, 100) / 100;
        painter.fillRect(QRect(0, height() - PROGRESS_HEIGHT, bar_w, PROGRESS_HEIGHT), token(STLXTHEME_ACCENT));
    }
}

} // namespace sheen
