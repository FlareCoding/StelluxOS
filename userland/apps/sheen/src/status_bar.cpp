#include "status_bar.h"

#include <QMouseEvent>
#include <QPainter>

#include <stlxtheme/theme.h>

static constexpr int BAR_HEIGHT = 28;
static constexpr int BAR_INSET_X = 8;
static constexpr int PART_PADDING_X = 8;
static constexpr int SEPARATOR_HEIGHT = 12;
static constexpr int HOVER_INSET_Y = 4;
static constexpr qreal POINTS_PER_INCH = 72;
static constexpr qreal SCREEN_DPI = 96;

static const QColor LINE = QColor::fromRgba(STLXTHEME_LINE);
static const QColor TEXT_DIM = QColor::fromRgba(STLXTHEME_TEXT_DIM);
static const QColor TEXT_SECONDARY = QColor::fromRgba(STLXTHEME_TEXT_SECONDARY);
static const QColor SURFACE_HOVER = QColor::fromRgba(STLXTHEME_SURFACE_HOVER);

static void paint_separator(QPainter& painter, int x, int height) {
    painter.fillRect(QRect(x, (height - SEPARATOR_HEIGHT) / 2, 1, SEPARATOR_HEIGHT), LINE);
}

status_bar::status_bar(QWidget* parent)
    : QWidget(parent), m_font(QString::fromLatin1(STLXTHEME_MONO_FONT_FAMILY)) {
    m_font.setPointSizeF(STLXTHEME_CAPTION_SIZE * POINTS_PER_INCH / SCREEN_DPI);
    setFixedHeight(BAR_HEIGHT);
    setMouseTracking(true);
}

void status_bar::set_caret_parts(const QStringList& parts) {
    if (parts != m_caret_parts) {
        m_caret_parts = parts;
        update();
    }
}

void status_bar::set_message(const QString& message) {
    if (message != m_message) {
        m_message = message;
        update();
    }
}

void status_bar::set_facts(const QStringList& facts) {
    if (facts != m_facts) {
        m_facts = facts;
        update();
    }
}

void status_bar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setFont(m_font);
    painter.fillRect(QRect(0, 0, width(), 1), LINE);

    int x = BAR_INSET_X;
    QFontMetrics metrics(m_font);
    for (int index = 0; index < m_caret_parts.size(); index++) {
        if (index > 0) {
            paint_separator(painter, x, height());
        }

        int width = metrics.horizontalAdvance(m_caret_parts.at(index)) + 2 * PART_PADDING_X;
        painter.setPen(TEXT_DIM);
        painter.drawText(QRect(x, 0, width, height()), Qt::AlignCenter, m_caret_parts.at(index));
        x += width;
    }

    for (int index = 0; index < m_facts.size(); index++) {
        QRect rect = fact_rect(index);
        if (index == m_hovered_fact) {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(SURFACE_HOVER);
            painter.drawRoundedRect(rect.adjusted(0, HOVER_INSET_Y, 0, -HOVER_INSET_Y), STLXTHEME_INNER_RADIUS,
                                    STLXTHEME_INNER_RADIUS);
            painter.setRenderHint(QPainter::Antialiasing, false);
        }

        if (index + 1 < m_facts.size()) {
            paint_separator(painter, rect.right() + 1, height());
        }

        painter.setPen(index == m_hovered_fact ? TEXT_SECONDARY : TEXT_DIM);
        painter.drawText(rect, Qt::AlignCenter, m_facts.at(index));
    }

    if (!m_message.isEmpty() && !m_facts.isEmpty()) {
        int message_left = x + PART_PADDING_X;
        QRect message(message_left, 0, fact_rect(m_facts.size() - 1).left() - message_left, height());
        painter.setPen(TEXT_SECONDARY);
        painter.drawText(message, Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(m_message, Qt::ElideRight, message.width()));
    }
}

void status_bar::mouseMoveEvent(QMouseEvent* event) {
    int fact = fact_at(event->position().toPoint());
    if (fact != m_hovered_fact) {
        m_hovered_fact = fact;
        update();
    }
}

void status_bar::mousePressEvent(QMouseEvent* event) {
    int fact = fact_at(event->position().toPoint());
    if (event->button() != Qt::LeftButton || fact < 0 || !on_fact_clicked) {
        QWidget::mousePressEvent(event);
        return;
    }

    on_fact_clicked(static_cast<file_fact>(fact), mapToGlobal(fact_rect(fact).topLeft()));
}

void status_bar::leaveEvent(QEvent* event) {
    m_hovered_fact = -1;
    update();
    QWidget::leaveEvent(event);
}

// Facts line up from the right edge, the first fact leftmost
QRect status_bar::fact_rect(int index) const {
    QFontMetrics metrics(m_font);
    int right = width() - BAR_INSET_X;
    for (int fact = m_facts.size() - 1; fact >= 0; fact--) {
        int width = metrics.horizontalAdvance(m_facts.at(fact)) + 2 * PART_PADDING_X;
        if (fact == index) {
            return QRect(right - width, 0, width, height());
        }

        right -= width + 1;
    }

    return QRect();
}

int status_bar::fact_at(const QPoint& point) const {
    for (int index = 0; index < m_facts.size(); index++) {
        if (fact_rect(index).contains(point)) {
            return index;
        }
    }

    return -1;
}
