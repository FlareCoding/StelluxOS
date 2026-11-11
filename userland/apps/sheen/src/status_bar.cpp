#include "status_bar.h"
#include "chrome.h"

#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

static constexpr int BAR_HEIGHT = 28;
static constexpr int BAR_INSET_X = 8;
static constexpr int ITEM_PADDING_X = 8;
static constexpr int SEPARATOR_HEIGHT = 12;
static constexpr int FACE_INSET_Y = 4;
static constexpr qreal HALF_PIXEL = 0.5;

static void paint_separator(QPainter& painter, int x, int height) {
    painter.fillRect(QRect(x, (height - SEPARATOR_HEIGHT) / 2, 1, SEPARATOR_HEIGHT), token(STLXTHEME_LINE));
}

status_bar::status_bar(QWidget* parent) : QWidget(parent), m_font(mono_font(STLXTHEME_CAPTION_SIZE)) {
    setFixedHeight(BAR_HEIGHT);
    setMouseTracking(true);
}

void status_bar::set_items(const QList<status_item>& left, const QList<status_item>& right) {
    m_left = left;
    m_right = right;
    place_items();
    update();
}

void status_bar::set_message(const QString& message) {
    if (message != m_message) {
        m_message = message;
        update();
    }
}

void status_bar::set_open_item(const status_kind* kind) {
    m_has_open = kind != nullptr;
    m_open_kind = kind ? *kind : status_kind::POSITION;
    update();
}

bool status_bar::event(QEvent* event) {
    if (event->type() != QEvent::ToolTip) {
        return QWidget::event(event);
    }

    auto* help = static_cast<QHelpEvent*>(event);
    int index = item_at(help->pos());
    if (index < 0 || m_placed.at(index).item.tooltip.isEmpty()) {
        QToolTip::hideText();
        event->ignore();
        return true;
    }

    QToolTip::showText(help->globalPos(), m_placed.at(index).item.tooltip, this, m_placed.at(index).rect);
    return true;
}

// Buttons show a face under the pointer and the open face of a top bar target while their menu shows
void status_bar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setFont(m_font);
    painter.fillRect(QRect(0, 0, width(), 1), token(STLXTHEME_LINE));

    painter.setRenderHint(QPainter::Antialiasing);
    for (int index = 0; index < m_placed.size(); index++) {
        const placed_item& placed = m_placed.at(index);
        QRectF face = QRectF(placed.rect.adjusted(0, FACE_INSET_Y, 0, -FACE_INSET_Y))
                          .adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
        bool is_open = m_has_open && placed.item.kind == m_open_kind;
        if (is_open) {
            painter.setPen(QPen(token(STLXTHEME_LINE_ON), 1));
            painter.setBrush(token(STLXTHEME_SURFACE_ON));
            painter.drawRoundedRect(face, STLXTHEME_INNER_RADIUS, STLXTHEME_INNER_RADIUS);
        } else if (index == m_hovered && placed.item.is_button) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(token(STLXTHEME_SURFACE_HOVER));
            painter.drawRoundedRect(face, STLXTHEME_INNER_RADIUS, STLXTHEME_INNER_RADIUS);
        }

        bool is_lit = (index == m_hovered && placed.item.is_button) || is_open;
        painter.setPen(token(is_lit ? STLXTHEME_TEXT_SECONDARY : STLXTHEME_TEXT_DIM));
        painter.drawText(placed.rect, Qt::AlignCenter, placed.item.text);
    }

    painter.setRenderHint(QPainter::Antialiasing, false);
    for (int index = 1; index < m_placed.size(); index++) {
        const placed_item& previous = m_placed.at(index - 1);
        if (m_placed.at(index).is_right == previous.is_right) {
            paint_separator(painter, previous.rect.right() + 1, height());
        }
    }

    if (m_message.isEmpty()) {
        return;
    }

    int message_left = BAR_INSET_X;
    int message_right = width() - BAR_INSET_X;
    for (const placed_item& placed : m_placed) {
        if (placed.is_right) {
            message_right = qMin(message_right, placed.rect.left());
        } else {
            message_left = qMax(message_left, placed.rect.right() + 1);
        }
    }

    QRect message(message_left + ITEM_PADDING_X, 0, message_right - message_left - 2 * ITEM_PADDING_X, height());
    painter.setPen(token(STLXTHEME_TEXT_SECONDARY));
    painter.drawText(message, Qt::AlignVCenter | Qt::AlignRight,
                     QFontMetrics(m_font).elidedText(m_message, Qt::ElideRight, message.width()));
}

void status_bar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    place_items();
}

void status_bar::mouseMoveEvent(QMouseEvent* event) {
    int index = item_at(event->position().toPoint());
    if (index != m_hovered) {
        m_hovered = index;
        update();
    }
}

void status_bar::mousePressEvent(QMouseEvent* event) {
    int index = item_at(event->position().toPoint());
    if (event->button() != Qt::LeftButton || index < 0 || !m_placed.at(index).item.is_button || !on_item_clicked) {
        QWidget::mousePressEvent(event);
        return;
    }

    const placed_item& placed = m_placed.at(index);
    on_item_clicked(placed.item.kind, mapToGlobal(placed.rect.topLeft()));
}

void status_bar::leaveEvent(QEvent* event) {
    m_hovered = -1;
    update();
    QWidget::leaveEvent(event);
}

// The left items line up from the left edge and the right items from the right edge, the first leftmost
void status_bar::place_items() {
    QFontMetrics metrics(m_font);
    m_placed.clear();

    int left = BAR_INSET_X;
    for (const status_item& item : m_left) {
        int width = metrics.horizontalAdvance(item.text) + 2 * ITEM_PADDING_X;
        m_placed.append({ item, QRect(left, 0, width, height()), false });
        left += width + 1;
    }

    int right = width() - BAR_INSET_X;
    QList<placed_item> placed_right;
    for (int index = static_cast<int>(m_right.size()) - 1; index >= 0; index--) {
        int width = metrics.horizontalAdvance(m_right.at(index).text) + 2 * ITEM_PADDING_X;
        placed_right.prepend({ m_right.at(index), QRect(right - width, 0, width, height()), true });
        right -= width + 1;
    }

    m_placed.append(placed_right);
    m_hovered = qMin(m_hovered, static_cast<int>(m_placed.size()) - 1);
}

int status_bar::item_at(const QPoint& point) const {
    for (int index = 0; index < m_placed.size(); index++) {
        if (m_placed.at(index).rect.contains(point)) {
            return index;
        }
    }

    return -1;
}
