#include "field_frame.h"
#include "chrome.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>

static constexpr int FIELD_HEIGHT = 28;
static constexpr int FIELD_PADDING_LEFT = 9;
static constexpr int FIELD_PADDING_RIGHT = 3;
static constexpr int OPTION_GAP = 2;
static constexpr int UNDERLINE_HEIGHT = 2;
static constexpr int MAGNIFIER_SIZE = 14;
static constexpr int MAGNIFIER_GAP = 8;
static constexpr qreal LENS_CENTER = 6;
static constexpr qreal LENS_RADIUS = 4.25;
static constexpr qreal HANDLE_START = 9.2;
static constexpr qreal HANDLE_END = 12.5;
static constexpr qreal GLYPH_STROKE = 1.5;
static constexpr qreal HALF_PIXEL = 0.5;
static constexpr qreal SELECTED_TEXT_ALPHA = 0.17;

field_frame::field_frame(QLineEdit* edit, QWidget* parent)
    : QWidget(parent), m_edit(edit), m_layout(new QHBoxLayout(this)) {
    setFixedHeight(FIELD_HEIGHT);
    m_layout->setContentsMargins(FIELD_PADDING_LEFT, 0, FIELD_PADDING_RIGHT, 0);
    m_layout->setSpacing(OPTION_GAP);

    QColor selected = token(STLXTHEME_TEXT);
    selected.setAlphaF(SELECTED_TEXT_ALPHA);

    edit->setParent(this);
    edit->setFrame(false);
    edit->setFont(mono_font(STLXTHEME_BODY_SIZE));
    edit->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; color: %1; "
                                       "selection-background-color: rgba(%2, %3, %4, %5); }")
                            .arg(token(STLXTHEME_TEXT).name())
                            .arg(selected.red())
                            .arg(selected.green())
                            .arg(selected.blue())
                            .arg(selected.alpha()));

    m_layout->addWidget(edit, 1);
    edit->installEventFilter(this);
}

void field_frame::add_option(QToolButton* option) {
    m_layout->addWidget(option);
}

void field_frame::show_magnifier() {
    m_has_magnifier = true;
    m_layout->setContentsMargins(FIELD_PADDING_LEFT + MAGNIFIER_SIZE + MAGNIFIER_GAP, 0, FIELD_PADDING_RIGHT, 0);
}

void field_frame::set_bad(bool is_bad) {
    if (m_is_bad != is_bad) {
        m_is_bad = is_bad;
        update();
    }
}

bool field_frame::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_edit && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        update();
    }

    return QWidget::eventFilter(watched, event);
}

void field_frame::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    bool is_marked = m_edit->hasFocus() || m_is_bad;
    QRectF edge = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
    painter.setPen(QPen(token(is_marked ? STLXTHEME_LINE_STRONG : STLXTHEME_LINE), 1));
    painter.setBrush(token(STLXTHEME_RECESS));
    painter.drawRoundedRect(edge, STLXTHEME_CONTROL_RADIUS - HALF_PIXEL, STLXTHEME_CONTROL_RADIUS - HALF_PIXEL);

    if (m_has_magnifier) {
        QPointF corner(FIELD_PADDING_LEFT, (height() - MAGNIFIER_SIZE) / 2.0);
        painter.setPen(QPen(token(STLXTHEME_TEXT_DIM), GLYPH_STROKE, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(corner + QPointF(LENS_CENTER, LENS_CENTER), LENS_RADIUS, LENS_RADIUS);
        painter.drawLine(corner + QPointF(HANDLE_START, HANDLE_START), corner + QPointF(HANDLE_END, HANDLE_END));
    }

    if (!is_marked) {
        return;
    }

    QPainterPath shape;
    shape.addRoundedRect(QRectF(rect()), STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);
    painter.setClipPath(shape);
    painter.fillRect(QRectF(0, height() - UNDERLINE_HEIGHT, width(), UNDERLINE_HEIGHT),
                     token(m_is_bad ? STLXTHEME_DANGER : STLXTHEME_ACCENT));
}
