#include "ui/ui_kit.h"
#include "core/kiln.h"
#include "ui/chrome.h"

#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWindow>
#include <QtMath>

#include <Scintilla.h>
#include <ScintillaEditBase.h>

namespace sheen::ui {

static constexpr int PILL_HEIGHT = STLXTHEME_TOOLBAR_CONTROL_HEIGHT;
static constexpr int PILL_PADDING_X = 10;
static constexpr int PILL_DOT_SIZE = 6;
static constexpr int PILL_GAP = 6;
static constexpr qreal PILL_CHEVRON_HALF = 3.0;
static constexpr qreal PILL_CHEVRON_STROKE = 1.4;
static constexpr qreal HALF_PIXEL = 0.5;

static constexpr int SWITCH_WIDTH = 36;
static constexpr int SWITCH_HEIGHT = 20;
static constexpr int SWITCH_KNOB_SIZE = 14;
static constexpr int SWITCH_INSET = 3;

static constexpr int POPOVER_GAP_Y = 4;
static constexpr int POPOVER_SCREEN_MARGIN = 8;
static constexpr int PICKER_WIDTH = 368;
static constexpr int PICKER_HEIGHT = 304;
static constexpr int PICKER_SEARCH_HEIGHT = 34;
static constexpr int PICKER_PADDING = 6;
static constexpr int MIN_PICKER_LABEL_WIDTH = 48;
static constexpr int ROW_HEIGHT = STLXTHEME_LIST_ROW_HEIGHT;
static constexpr int ROW_PADDING_X = 10;

static constexpr int DIAG_BAR_HEIGHT = 28;
static constexpr int DIAG_BAR_PADDING_X = 10;
static constexpr int DIAG_ACCENT_WIDTH = 2;
static constexpr qreal DIAG_FILL_ALPHA = 0.14;
static constexpr int SPLIT_MATCH_MARKER = 22;
static constexpr uint32_t SPLIT_MATCH_TINT = 0x1EFFF0D7;

static constexpr int CHOICE_ID_ROLE = Qt::UserRole;
static constexpr int CHOICE_DETAIL_ROLE = Qt::UserRole + 1;
static constexpr int CHOICE_BADGE_ROLE = Qt::UserRole + 2;
static constexpr int CHOICE_ACTION_ROLE = Qt::UserRole + 3;
static constexpr int CHOICE_STRUCK_ROLE = Qt::UserRole + 4;

namespace {

class search_header : public QWidget {
public:
    explicit search_header(QLineEdit* edit, QWidget* parent) : QWidget(parent), m_edit(edit) {
        setFixedHeight(PICKER_SEARCH_HEIGHT);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(ROW_PADDING_X, 0, ROW_PADDING_X, STLXTHEME_FOCUS_RULE_WIDTH);
        layout->setSpacing(0);
        layout->addWidget(m_edit);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        if (m_edit->hasFocus()) {
            painter.fillRect(QRect(0, height() - STLXTHEME_FOCUS_RULE_WIDTH, width(), STLXTHEME_FOCUS_RULE_WIDTH),
                             token(STLXTHEME_ACCENT));
        } else {
            painter.fillRect(QRect(0, height() - 1, width(), 1), token(STLXTHEME_LINE));
        }
    }

private:
    QLineEdit* m_edit;
};

class picker_item_delegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(0, ROW_HEIGHT);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        bool is_selected = option.state & QStyle::State_Selected;
        bool is_hovered = option.state & QStyle::State_MouseOver;
        if (is_selected || is_hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(token(is_selected ? STLXTHEME_SURFACE_SELECTED : STLXTHEME_ROW_HOVER));
            painter->drawRoundedRect(option.rect.adjusted(2, 1, -2, -1), STLXTHEME_MENU_ITEM_RADIUS,
                                     STLXTHEME_MENU_ITEM_RADIUS);
        }

        bool is_action = index.data(CHOICE_ACTION_ROLE).toBool();
        bool is_struck = index.data(CHOICE_STRUCK_ROLE).toBool();
        QString label = index.data(Qt::DisplayRole).toString();
        QString detail = index.data(CHOICE_DETAIL_ROLE).toString();
        QString badge = index.data(CHOICE_BADGE_ROLE).toString();

        QRect content = option.rect.adjusted(ROW_PADDING_X, 0, -ROW_PADDING_X, 0);
        QFont mono = mono_font(STLXTHEME_CAPTION_SIZE);
        QFontMetrics mono_metrics(mono);

        int right_used = 0;
        if (!badge.isEmpty()) {
            painter->setFont(mono);
            painter->setPen(token(STLXTHEME_ACCENT));
            painter->drawText(content, Qt::AlignRight | Qt::AlignVCenter, badge);
            right_used = mono_metrics.horizontalAdvance(badge) + PILL_GAP * 2;
        }

        QRect left_area = content.adjusted(0, 0, -right_used, 0);
        QFont label_font = ui_font(STLXTHEME_BODY_SIZE, is_action ? QFont::Medium : QFont::Normal);
        label_font.setStrikeOut(is_struck);
        painter->setFont(label_font);
        QColor label_color = is_struck   ? token(STLXTHEME_TEXT_DISABLED)
                           : is_action   ? token(STLXTHEME_ACCENT)
                           : is_selected ? token(STLXTHEME_TEXT)
                                         : token(STLXTHEME_TEXT_SECONDARY);
        painter->setPen(label_color);

        int detail_width = detail.isEmpty() ? 0 : mono_metrics.horizontalAdvance(detail) + PILL_GAP * 2;
        int max_label_width = qMax(MIN_PICKER_LABEL_WIDTH, left_area.width() - detail_width);
        QString shown_label = painter->fontMetrics().elidedText(label, Qt::ElideRight, max_label_width);
        painter->drawText(left_area, Qt::AlignLeft | Qt::AlignVCenter, shown_label);

        if (!detail.isEmpty() && left_area.width() > max_label_width) {
            int label_advance = painter->fontMetrics().horizontalAdvance(shown_label) + PILL_GAP;
            painter->setFont(mono);
            painter->setPen(token(STLXTHEME_TEXT_DIM));
            QRect detail_rect = left_area.adjusted(label_advance, 0, 0, 0);
            painter->drawText(detail_rect, Qt::AlignLeft | Qt::AlignVCenter,
                              mono_metrics.elidedText(detail, Qt::ElideMiddle, detail_rect.width()));
        }

        painter->restore();
    }
};

class section_row_delegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return QSize(0, ROW_HEIGHT);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        bool is_selected = option.state & QStyle::State_Selected;
        bool is_hovered = option.state & QStyle::State_MouseOver;
        if (is_selected || is_hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(token(is_selected ? STLXTHEME_SURFACE_SELECTED : STLXTHEME_ROW_HOVER));
            painter->drawRoundedRect(option.rect.adjusted(4, 1, -4, -1), STLXTHEME_MENU_ITEM_RADIUS,
                                     STLXTHEME_MENU_ITEM_RADIUS);
        }

        QRect room = option.rect.adjusted(ROW_PADDING_X, 0, -ROW_PADDING_X, 0);
        QString label = index.data(Qt::DisplayRole).toString();
        QString detail = index.data(CHOICE_DETAIL_ROLE).toString();

        QFont mono = mono_font(STLXTHEME_CAPTION_SIZE);
        QFontMetrics mono_metrics(mono);
        int detail_width = detail.isEmpty() ? 0 : mono_metrics.horizontalAdvance(detail) + PILL_GAP;

        if (!detail.isEmpty()) {
            painter->setFont(mono);
            painter->setPen(token(STLXTHEME_TEXT_DIM));
            painter->drawText(room, Qt::AlignRight | Qt::AlignVCenter, detail);
        }

        painter->setFont(ui_font(STLXTHEME_BODY_SIZE));
        painter->setPen(token(is_selected || is_hovered ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY));
        QRect label_rect = room.adjusted(0, 0, -detail_width, 0);
        painter->drawText(label_rect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(label, Qt::ElideRight, label_rect.width()));

        painter->restore();
    }
};

class split_divider : public QWidget {
public:
    explicit split_divider(QWidget* parent) : QWidget(parent) {
        setFixedWidth(1);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), token(STLXTHEME_LINE));
    }
};

class sortable_tree_item : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override {
        int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        QString left = text(col).trimmed();
        QString right = other.text(col).trimmed();

        auto parse_leading_number = [](const QString& value, bool* ok) -> double {
            qsizetype end = 0;
            while (end < value.size() && (value.at(end).isDigit() || value.at(end) == QLatin1Char('.'))) {
                end++;
            }

            if (end == 0) {
                *ok = false;
                return 0.0;
            }

            return value.left(end).toDouble(ok);
        };

        bool left_num = false;
        bool right_num = false;
        double left_val = parse_leading_number(left, &left_num);
        double right_val = parse_leading_number(right, &right_num);
        if (left_num && right_num) {
            return left_val < right_val;
        }

        return left.compare(right, Qt::CaseInsensitive) < 0;
    }
};

} // namespace

ui_pill_button::ui_pill_button(const QString& label, QWidget* parent) : QAbstractButton(parent) {
    setText(label);
    setFixedHeight(PILL_HEIGHT);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::PointingHandCursor);
}

void ui_pill_button::set_checked_dot(bool is_active) {
    if (m_is_active != is_active) {
        m_is_active = is_active;
        updateGeometry();
        update();
    }
}

void ui_pill_button::set_detail(const QString& detail) {
    if (m_detail != detail) {
        m_detail = detail;
        updateGeometry();
        update();
    }
}

void ui_pill_button::set_has_chevron(bool has_chevron) {
    if (m_has_chevron != has_chevron) {
        m_has_chevron = has_chevron;
        updateGeometry();
        update();
    }
}

void ui_pill_button::set_is_open(bool is_open) {
    if (m_is_open != is_open) {
        m_is_open = is_open;
        update();
    }
}

QSize ui_pill_button::sizeHint() const {
    QFont label_font = ui_font(STLXTHEME_BODY_SIZE, QFont::Medium);
    int width = 2 * PILL_PADDING_X + qCeil(QFontMetricsF(label_font).horizontalAdvance(text()));
    if (m_is_active) {
        width += PILL_DOT_SIZE + PILL_GAP;
    }

    if (!m_detail.isEmpty()) {
        QFont detail_font = mono_font(STLXTHEME_CAPTION_SIZE);
        width += PILL_GAP + qCeil(QFontMetricsF(detail_font).horizontalAdvance(m_detail));
    }

    if (m_has_chevron) {
        width += PILL_GAP + static_cast<int>(2 * PILL_CHEVRON_HALF);
    }

    return QSize(width, PILL_HEIGHT);
}

void ui_pill_button::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QRectF face = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
    bool is_lit = m_is_open || isChecked() || m_is_active;
    QColor fill = !isEnabled()  ? token(STLXTHEME_SURFACE)
                : isDown()      ? token(STLXTHEME_SURFACE_PRESS)
                : is_lit        ? token(STLXTHEME_SURFACE_ON)
                : underMouse()  ? token(STLXTHEME_SURFACE_HOVER)
                                : token(STLXTHEME_SURFACE);
    QColor border = !isEnabled() ? token(STLXTHEME_LINE_SUBTLE)
                  : is_lit       ? token(STLXTHEME_LINE_ON)
                  : underMouse() ? token(STLXTHEME_LINE_STRONG)
                                 : token(STLXTHEME_LINE);

    painter.setPen(QPen(border, 1));
    painter.setBrush(fill);
    painter.drawRoundedRect(face, STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);

    int left = PILL_PADDING_X;
    int right = width() - PILL_PADDING_X;

    if (m_is_active) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(token(STLXTHEME_ACCENT));
        painter.drawEllipse(QPointF(left + PILL_DOT_SIZE / 2.0, height() / 2.0),
                            PILL_DOT_SIZE / 2.0, PILL_DOT_SIZE / 2.0);
        left += PILL_DOT_SIZE + PILL_GAP;
    }

    if (m_has_chevron) {
        QPointF tip(right - PILL_CHEVRON_HALF, height() / 2.0 + PILL_CHEVRON_HALF / 2.0);
        painter.setPen(QPen(token(isEnabled() ? STLXTHEME_TEXT_DIM : STLXTHEME_TEXT_DISABLED),
                            PILL_CHEVRON_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(tip + QPointF(-PILL_CHEVRON_HALF, -PILL_CHEVRON_HALF), tip);
        painter.drawLine(tip, tip + QPointF(PILL_CHEVRON_HALF, -PILL_CHEVRON_HALF));
        right -= static_cast<int>(2 * PILL_CHEVRON_HALF) + PILL_GAP;
    }

    QFont label_font = ui_font(STLXTHEME_BODY_SIZE, QFont::Medium);
    painter.setFont(label_font);
    painter.setPen(token(isEnabled() ? STLXTHEME_TEXT : STLXTHEME_TEXT_DISABLED));

    int avail = qMax(0, right - left);
    QString shown_label = painter.fontMetrics().elidedText(text(), Qt::ElideRight, avail);
    painter.drawText(QRect(left, 0, avail, height()), Qt::AlignLeft | Qt::AlignVCenter, shown_label);

    int label_used = painter.fontMetrics().horizontalAdvance(shown_label) + PILL_GAP;
    if (!m_detail.isEmpty() && label_used < avail) {
        QFont detail_font = mono_font(STLXTHEME_CAPTION_SIZE);
        painter.setFont(detail_font);
        painter.setPen(token(STLXTHEME_TEXT_DIM));
        int detail_avail = avail - label_used;
        painter.drawText(QRect(left + label_used, 0, detail_avail, height()), Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(m_detail, Qt::ElideMiddle, detail_avail));
    }
}

ui_switch::ui_switch(QWidget* parent) : QAbstractButton(parent) {
    setCheckable(true);
    setFixedSize(SWITCH_WIDTH, SWITCH_HEIGHT);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::PointingHandCursor);
}

QSize ui_switch::sizeHint() const {
    return QSize(SWITCH_WIDTH, SWITCH_HEIGHT);
}

void ui_switch::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QRectF track = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
    qreal radius = track.height() / 2.0;

    if (isChecked()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(token(!isEnabled() ? STLXTHEME_LINE_SUBTLE
                               : underMouse() ? STLXTHEME_ACCENT_HOVER
                                              : STLXTHEME_ACCENT));
        painter.drawRoundedRect(track, radius, radius);
    } else {
        painter.setPen(QPen(token(underMouse() && isEnabled() ? STLXTHEME_SWITCH_HOVER_RING : STLXTHEME_LINE), 1));
        painter.setBrush(token(STLXTHEME_RECESS));
        painter.drawRoundedRect(track, radius, radius);
    }

    qreal knob_x = isChecked() ? width() - SWITCH_INSET - SWITCH_KNOB_SIZE : SWITCH_INSET;
    QRectF knob(knob_x, (height() - SWITCH_KNOB_SIZE) / 2.0, SWITCH_KNOB_SIZE, SWITCH_KNOB_SIZE);
    if (!isChecked()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(token(STLXTHEME_KNOB_SHADOW));
        painter.drawEllipse(knob.translated(0, 1));
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(token(isChecked() ? STLXTHEME_SWITCH_KNOB : STLXTHEME_SLIDER_KNOB));
    painter.drawEllipse(knob);
}

ui_popover::ui_popover(QWidget* anchor, int width, int height)
    : QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint), m_anchor(anchor) {
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(width, height);
    if (anchor) {
        connect(anchor, &QObject::destroyed, this, &QWidget::close);
    }
}

void ui_popover::open_below_anchor() {
    if (m_anchor) {
        QPoint below = m_anchor->mapToGlobal(QPoint(0, m_anchor->height() + POPOVER_GAP_Y));
        QWidget* owner = m_anchor->window();
        if (owner) {
            int min_x = owner->mapToGlobal(QPoint(POPOVER_SCREEN_MARGIN, 0)).x();
            int max_x = owner->mapToGlobal(QPoint(owner->width() - POPOVER_SCREEN_MARGIN - width(), 0)).x();
            int min_y = owner->mapToGlobal(QPoint(0, POPOVER_SCREEN_MARGIN)).y();
            int max_y = owner->mapToGlobal(QPoint(0, owner->height() - POPOVER_SCREEN_MARGIN - height())).y();

            int x = qBound(min_x, below.x(), qMax(min_x, max_x));
            int y = qBound(min_y, below.y(), qMax(min_y, max_y));
            setGeometry(x, y, width(), height());
        } else {
            setGeometry(below.x(), below.y(), width(), height());
        }

        (void)winId();
        if (owner && windowHandle() && owner->windowHandle()) {
            windowHandle()->setTransientParent(owner->windowHandle());
        }
    }

    show();
}

void ui_popover::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QRectF panel = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
    painter.setPen(QPen(token(STLXTHEME_RIM), 1));
    painter.setBrush(token(STLXTHEME_MENU_BG));
    painter.drawRoundedRect(panel, STLXTHEME_MENU_RADIUS, STLXTHEME_MENU_RADIUS);
}

void ui_popover::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (on_closed) {
        on_closed();
    }
}

ui_searchable_picker::ui_searchable_picker(QWidget* anchor, const QString& placeholder,
                                           const QList<picker_choice>& choices)
    : ui_popover(anchor, PICKER_WIDTH, PICKER_HEIGHT), m_choices(choices) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, PICKER_PADDING);
    layout->setSpacing(2);

    m_search = new QLineEdit(this);
    m_search->setFont(ui_font(STLXTHEME_BODY_SIZE));
    m_search->setFrame(false);
    m_search->setPlaceholderText(placeholder);
    m_search->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; color: %1; border: none; }")
                                .arg(token(STLXTHEME_TEXT).name()));

    auto* header = new search_header(m_search, this);
    layout->addWidget(header);

    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_list->viewport()->setMouseTracking(true);
    m_list->setItemDelegate(new picker_item_delegate(m_list));
    m_list->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; outline: none; }"));
    layout->addWidget(m_list, 1);

    m_search->installEventFilter(this);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& query) { filter_choices(query); });
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        activate_row(m_list->row(item));
    });

    filter_choices(QString());
    m_search->setFocus();
}

bool ui_searchable_picker::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        int count = m_list->count();
        if (key->key() == Qt::Key_Down && count > 0) {
            m_list->setCurrentRow((m_list->currentRow() + 1) % count);
            return true;
        }

        if (key->key() == Qt::Key_Up && count > 0) {
            m_list->setCurrentRow((m_list->currentRow() + count - 1) % count);
            return true;
        }

        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && m_list->currentRow() >= 0) {
            activate_row(m_list->currentRow());
            return true;
        }

        if (key->key() == Qt::Key_Escape) {
            close();
            return true;
        }
    }

    return ui_popover::eventFilter(watched, event);
}

void ui_searchable_picker::filter_choices(const QString& query) {
    QString trimmed = query.trimmed();
    m_list->clear();
    m_filtered_indices.clear();

    for (int i = 0; i < m_choices.size(); i++) {
        const picker_choice& choice = m_choices.at(i);
        bool matches = trimmed.isEmpty() || choice.is_action
                    || choice.label.contains(trimmed, Qt::CaseInsensitive)
                    || choice.detail.contains(trimmed, Qt::CaseInsensitive)
                    || choice.id.contains(trimmed, Qt::CaseInsensitive);
        if (!matches) {
            continue;
        }

        auto* item = new QListWidgetItem(choice.label, m_list);
        item->setData(CHOICE_ID_ROLE, choice.id);
        item->setData(CHOICE_DETAIL_ROLE, choice.detail);
        item->setData(CHOICE_BADGE_ROLE, choice.badge);

        item->setData(CHOICE_ACTION_ROLE, choice.is_action);
        item->setData(CHOICE_STRUCK_ROLE, choice.is_struck);
        m_filtered_indices.append(i);
    }

    if (m_list->count() > 0) {
        m_list->setCurrentRow(0);
    }
}

void ui_searchable_picker::activate_row(int row) {
    if (row < 0 || row >= m_filtered_indices.size()) {
        return;
    }

    const picker_choice& chosen = m_choices.at(m_filtered_indices.at(row));
    if (chosen.is_struck) {
        return;
    }

    QString chosen_id = chosen.id;
    std::function<void(const QString&)> callback = on_selected;
    close();
    if (callback) {
        callback(chosen_id);
    }
}

ui_section_list::ui_section_list(const QString& action_label, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 4);
    layout->setSpacing(4);

    if (!action_label.isEmpty()) {
        auto* header = new QHBoxLayout;
        header->setContentsMargins(ROW_PADDING_X, 0, ROW_PADDING_X, 0);
        header->addStretch(1);

        auto* button = new ui_pill_button(action_label, this);
        connect(button, &QAbstractButton::clicked, this, [this] {
            if (on_action_clicked) {
                on_action_clicked();
            }
        });

        header->addWidget(button);
        layout->addLayout(header);
    }

    m_empty = new QLabel(QStringLiteral("None installed"), this);
    m_empty->setFont(ui_font(STLXTHEME_CAPTION_SIZE));
    m_empty->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_empty->setContentsMargins(ROW_PADDING_X, 4, ROW_PADDING_X, 4);
    m_empty->setStyleSheet(QStringLiteral("color: %1").arg(token(STLXTHEME_TEXT_DIM).name()));
    layout->addWidget(m_empty);

    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->viewport()->setMouseTracking(true);

    m_list->setItemDelegate(new section_row_delegate(m_list));
    m_list->setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; outline: none; }"));
    m_list->hide();

    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        if (on_row_activated && item) {
            on_row_activated(item->data(CHOICE_ID_ROLE).toString());
        }
    });

    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (on_row_activated && item) {
            on_row_activated(item->data(CHOICE_ID_ROLE).toString());
        }
    });

    layout->addWidget(m_list, 1);
}

void ui_section_list::set_rows(const QList<section_row>& rows) {
    m_list->clear();
    for (const section_row& row : rows) {
        auto* item = new QListWidgetItem(row.label, m_list);
        item->setData(CHOICE_ID_ROLE, row.id);
        item->setData(CHOICE_DETAIL_ROLE, row.detail);
        if (!row.tooltip.isEmpty()) {
            item->setToolTip(row.tooltip);
        }
    }

    bool has_rows = !rows.isEmpty();
    m_empty->setVisible(!has_rows);
    m_list->setVisible(has_rows);
}

void ui_section_list::set_empty_text(const QString& text) {
    m_empty->setText(text);
}

ui_inline_diagnostic::ui_inline_diagnostic(const diagnostic_item& item, QWidget* parent)
    : QWidget(parent), m_item(item) {
    setFixedHeight(DIAG_BAR_HEIGHT);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(DIAG_ACCENT_WIDTH + DIAG_BAR_PADDING_X, 0, DIAG_BAR_PADDING_X, 0);
    layout->setSpacing(PILL_GAP * 2);

    auto* message = new QLabel(item.message, this);
    message->setFont(ui_font(STLXTHEME_BODY_SIZE));
    uint32_t text_tone = item.severity == diagnostic_severity::ERROR ? STLXTHEME_DANGER_TEXT : STLXTHEME_TEXT;
    message->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(token(text_tone).name()));
    layout->addWidget(message, 1);

    if (!item.fix.label.isEmpty() && item.fix.apply) {
        auto* fix_button = new ui_pill_button(item.fix.label, this);
        std::function<void()> apply = item.fix.apply;
        connect(fix_button, &QAbstractButton::clicked, this, [apply] { apply(); });
        layout->addWidget(fix_button, 0, Qt::AlignVCenter);
    }
}

void ui_inline_diagnostic::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QColor accent = token(m_item.severity == diagnostic_severity::ERROR ? STLXTHEME_DANGER : STLXTHEME_ACCENT);
    QColor fill = accent;
    fill.setAlphaF(DIAG_FILL_ALPHA);

    painter.fillRect(rect(), token(STLXTHEME_OPAQUE_POCKET));
    painter.fillRect(rect(), fill);
    painter.fillRect(QRect(0, 0, DIAG_ACCENT_WIDTH, height()), accent);
}

ui_table_view::ui_table_view(const QList<table_column_spec>& columns, QWidget* parent)
    : QWidget(parent), m_columns(columns), m_tree(new QTreeWidget(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_tree);

    m_tree->setColumnCount(static_cast<int>(columns.size()));
    QStringList headers;
    for (int i = 0; i < columns.size(); i++) {
        headers.append(columns.at(i).title);
    }

    m_tree->setHeaderLabels(headers);
    m_tree->setRootIsDecorated(false);
    m_tree->setItemsExpandable(false);
    m_tree->setUniformRowHeights(true);

    m_tree->setSortingEnabled(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setFont(mono_font(STLXTHEME_BODY_SIZE));

    QHeaderView* header = m_tree->header();
    header->setFont(ui_font(STLXTHEME_CAPTION_SIZE, QFont::DemiBold));
    header->setStretchLastSection(false);
    for (int i = 0; i < columns.size(); i++) {
        if (columns.at(i).width > 0) {
            header->setSectionResizeMode(i, QHeaderView::Interactive);
            m_tree->setColumnWidth(i, columns.at(i).width);
        } else {
            header->setSectionResizeMode(i, QHeaderView::Stretch);
        }
    }

    m_tree->setStyleSheet(QStringLiteral(
        "QTreeWidget { background: %1; color: %2; border: none; outline: none; }"
        "QTreeWidget::item { height: %3px; padding: 0 8px; }"
        "QTreeWidget::item:selected { background: %4; color: %5; }"
        "QHeaderView::section { background: %1; color: %6; border: none; border-bottom: 1px solid %7; "
        "height: 28px; padding: 0 8px; }")
                              .arg(token(STLXTHEME_OPAQUE_POCKET).name(),
                                   token(STLXTHEME_TERMINAL_TEXT).name(),
                                   QString::number(ROW_HEIGHT),
                                   token(STLXTHEME_SURFACE_SELECTED).name(QColor::HexArgb),
                                   token(STLXTHEME_TEXT).name(),
                                   token(STLXTHEME_TEXT_DIM).name(),
                                   token(STLXTHEME_LINE).name(QColor::HexArgb)));

    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (on_row_clicked && item) {
            on_row_clicked(item->data(0, CHOICE_ID_ROLE).toString());
        }
    });

    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
        if (on_row_activated && item) {
            on_row_activated(item->data(0, CHOICE_ID_ROLE).toString());
        }
    });
}

void ui_table_view::set_rows(const QList<table_row>& rows) {
    m_tree->setSortingEnabled(false);
    m_tree->clear();

    for (const table_row& row : rows) {
        auto* item = new sortable_tree_item(m_tree);
        item->setData(0, CHOICE_ID_ROLE, row.id);
        for (int col = 0; col < m_columns.size() && col < row.cells.size(); col++) {
            item->setText(col, row.cells.at(col));
            if (m_columns.at(col).aligns_right) {
                item->setTextAlignment(col, Qt::AlignRight | Qt::AlignVCenter);
            } else {
                item->setTextAlignment(col, Qt::AlignLeft | Qt::AlignVCenter);
            }
        }
    }

    m_tree->setSortingEnabled(true);
}

static void configure_split_pane(ScintillaEditBase* view) {
    apply_text_area_look(view);
    set_line_numbers_shown(view, true);
    set_folding_shown(view, false);
    define_line_tint(view, SPLIT_MATCH_MARKER, SPLIT_MATCH_TINT);

    view->send(SCI_SETINDENTATIONGUIDES, SC_IV_NONE);
    view->send(SCI_SETEDGEMODE, EDGE_NONE);
    view->send(SCI_SETUNDOCOLLECTION, 0);
    view->send(SCI_SETCHANGEHISTORY, SC_CHANGE_HISTORY_DISABLED);
    view->send(SCI_SETREADONLY, 1);
}

ui_split_code_view::ui_split_code_view(QWidget* parent)
    : QWidget(parent), m_left(new ScintillaEditBase(this)), m_right(new ScintillaEditBase(this)) {
    configure_split_pane(m_left);
    configure_split_pane(m_right);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_left, 1);
    layout->addWidget(new split_divider(this));
    layout->addWidget(m_right, 1);

    connect(m_left, &ScintillaEditBase::updateUi, this, [this](Scintilla::Update updated) {
        if (static_cast<int>(updated) & SC_UPDATE_SELECTION) {
            sync_counterpart(m_left, m_right, m_left_to_right);
        }
    });

    connect(m_right, &ScintillaEditBase::updateUi, this, [this](Scintilla::Update updated) {
        if (static_cast<int>(updated) & SC_UPDATE_SELECTION) {
            sync_counterpart(m_right, m_left, m_right_to_left);
        }
    });
}

void ui_split_code_view::set_line_mapping(const QList<std::pair<int, int>>& left_to_right_lines) {
    m_left_to_right.clear();
    m_right_to_left.clear();
    for (const auto& [left_line, right_line] : left_to_right_lines) {
        if (!m_left_to_right.contains(left_line)) {
            m_left_to_right.insert(left_line, right_line);
        }

        if (!m_right_to_left.contains(right_line)) {
            m_right_to_left.insert(right_line, left_line);
        }
    }
}

void ui_split_code_view::sync_counterpart(ScintillaEditBase* source, ScintillaEditBase* target,
                                          const QHash<int, int>& mapping) {
    if (m_is_syncing || mapping.isEmpty()) {
        return;
    }

    int source_line = static_cast<int>(source->send(SCI_LINEFROMPOSITION, source->send(SCI_GETCURRENTPOS))) + 1;
    if (!mapping.contains(source_line)) {
        return;
    }

    m_is_syncing = true;
    int target_line = qBound(1, mapping.value(source_line), static_cast<int>(target->send(SCI_GETLINECOUNT)));
    target->send(SCI_MARKERDELETEALL, SPLIT_MATCH_MARKER);
    target->send(SCI_MARKERADD, target_line - 1, SPLIT_MATCH_MARKER);

    sptr_t visible = target->send(SCI_VISIBLEFROMDOCLINE, target_line - 1);
    target->send(SCI_SETFIRSTVISIBLELINE, qMax<sptr_t>(0, visible - target->send(SCI_LINESONSCREEN) / 3));
    m_is_syncing = false;
}

} // namespace sheen::ui
