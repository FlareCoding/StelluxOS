#include "tab_strip.h"
#include "menu_style.h"

#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <stlxtheme/theme.h>

static constexpr int ROW_HEIGHT = 36;
static constexpr int ROW_INSET_X = 8;
static constexpr int MENU_BUTTON_GAP = 6;
static constexpr int TAB_HEIGHT = 31;
static constexpr int TAB_PADDING_LEFT = 14;
static constexpr int TAB_PADDING_RIGHT = 8;
static constexpr int LABEL_SLOT_GAP = 8;
static constexpr int MIN_TAB_WIDTH = 80;
static constexpr int MAX_TAB_WIDTH = 240;
static constexpr int SLOT_SIZE = 16;
static constexpr int DOT_SIZE = 8;
static constexpr qreal CLOSE_GLYPH_HALF = 3;
static constexpr qreal CLOSE_GLYPH_STROKE = 1.4;
static constexpr qreal HALF_PIXEL = 0.5;
static constexpr qreal POINTS_PER_INCH = 72;
static constexpr qreal SCREEN_DPI = 96;

static const QColor LINE = QColor::fromRgba(STLXTHEME_LINE);
static const QColor SHADE = QColor::fromRgba(STLXTHEME_SHADE);
static const QColor TEXT = QColor::fromRgba(STLXTHEME_TEXT);
static const QColor TEXT_SECONDARY = QColor::fromRgba(STLXTHEME_TEXT_SECONDARY);
static const QColor TEXT_DIM = QColor::fromRgba(STLXTHEME_TEXT_DIM);
static const QColor ACCENT = QColor::fromRgba(STLXTHEME_ACCENT);
static const QColor SURFACE_HOVER = QColor::fromRgba(STLXTHEME_SURFACE_HOVER);

// Rounded at the top corners and open at the bottom, where the tab joins the text
static QPainterPath open_tab_outline(const QRectF& rect, qreal radius) {
    QPainterPath path;
    path.moveTo(rect.left(), rect.bottom());
    path.lineTo(rect.left(), rect.top() + radius);
    path.arcTo(QRectF(rect.left(), rect.top(), 2 * radius, 2 * radius), 180, -90);

    path.lineTo(rect.right() - radius, rect.top());
    path.arcTo(QRectF(rect.right() - 2 * radius, rect.top(), 2 * radius, 2 * radius), 90, -90);
    path.lineTo(rect.right(), rect.bottom());

    return path;
}

// The face sits inside the edge, not under it, so the two translucent colors never stack
static void paint_open_tab(QPainter& painter, const QRect& rect) {
    QRectF face = QRectF(rect).adjusted(1, 1, -1, 0);
    painter.fillPath(open_tab_outline(face, STLXTHEME_CONTROL_RADIUS - 1), SHADE);

    QRectF edge = QRectF(rect).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, 0);
    painter.setPen(QPen(LINE, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(open_tab_outline(edge, STLXTHEME_CONTROL_RADIUS - HALF_PIXEL));
}

static void paint_close_glyph(QPainter& painter, const QRect& slot, bool is_hovered) {
    if (is_hovered) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(SURFACE_HOVER);
        painter.drawRoundedRect(slot, STLXTHEME_INNER_RADIUS, STLXTHEME_INNER_RADIUS);
    }

    QPointF center = QRectF(slot).center();
    painter.setPen(QPen(is_hovered ? TEXT : TEXT_DIM, CLOSE_GLYPH_STROKE, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(center + QPointF(-CLOSE_GLYPH_HALF, -CLOSE_GLYPH_HALF),
                     center + QPointF(CLOSE_GLYPH_HALF, CLOSE_GLYPH_HALF));
    painter.drawLine(center + QPointF(CLOSE_GLYPH_HALF, -CLOSE_GLYPH_HALF),
                     center + QPointF(-CLOSE_GLYPH_HALF, CLOSE_GLYPH_HALF));
}

static void paint_unsaved_dot(QPainter& painter, const QRect& slot) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(ACCENT);
    painter.drawEllipse(QRectF(slot).center(), DOT_SIZE / 2.0, DOT_SIZE / 2.0);
}

tab_strip::tab_strip(QWidget* parent) : QTabBar(parent), m_font(QString::fromLatin1(STLXTHEME_UI_FONT_FAMILY)) {
    m_font.setPointSizeF(STLXTHEME_BODY_SIZE * POINTS_PER_INCH / SCREEN_DPI);
    m_font.setWeight(QFont::Medium);
    setFont(m_font);

    setDrawBase(false);
    setExpanding(false);
    setDocumentMode(true);
    setElideMode(Qt::ElideMiddle);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
}

void tab_strip::set_tab_modified(int index, bool is_modified) {
    if (is_tab_modified(index) != is_modified) {
        setTabData(index, is_modified);
        update(tabRect(index));
    }
}

// The label's width is rounded up, so a name that fits is never cut by a fraction of a pixel
QSize tab_strip::tabSizeHint(int index) const {
    int label = qCeil(QFontMetricsF(m_font).horizontalAdvance(tabText(index)));

    int natural = TAB_PADDING_LEFT + label + LABEL_SLOT_GAP + SLOT_SIZE + TAB_PADDING_RIGHT;
    return QSize(qBound(MIN_TAB_WIDTH, natural, MAX_TAB_WIDTH), TAB_HEIGHT);
}

QSize tab_strip::minimumTabSizeHint(int index) const {
    return tabSizeHint(index);
}

// A tab bar that is not shown yet defers its layout without telling the layout around it, so the row asks again
void tab_strip::tabInserted(int index) {
    QTabBar::tabInserted(index);
    updateGeometry();
}

void tab_strip::tabRemoved(int index) {
    QTabBar::tabRemoved(index);
    updateGeometry();
}

void tab_strip::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setFont(m_font);

    for (int index = 0; index < count(); index++) {
        QRect rect = tabRect(index);
        bool is_current = index == currentIndex();
        bool is_hovered = index == m_hovered_tab;
        if (is_current) {
            paint_open_tab(painter, rect);
        }

        QRect slot = close_slot(index);
        int label_left = rect.left() + TAB_PADDING_LEFT;
        QRect label(label_left, rect.top(), slot.left() - LABEL_SLOT_GAP - label_left, rect.height());
        painter.setPen(is_current ? TEXT : is_hovered ? TEXT_SECONDARY : TEXT_DIM);
        QString text = tabText(index);
        if (QFontMetricsF(m_font).horizontalAdvance(text) > label.width()) {
            text = painter.fontMetrics().elidedText(text, Qt::ElideMiddle, label.width());
        }

        painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeft, text);

        bool is_slot_hovered = is_hovered && m_is_slot_hovered;
        if (is_tab_modified(index) && !is_slot_hovered) {
            paint_unsaved_dot(painter, slot);
        } else if (is_current || is_hovered) {
            paint_close_glyph(painter, slot, is_slot_hovered);
        }
    }
}

void tab_strip::mouseMoveEvent(QMouseEvent* event) {
    QPoint point = event->position().toPoint();
    int tab = tabAt(point);
    set_hover(tab, tab >= 0 && close_slot(tab).contains(point));
    QTabBar::mouseMoveEvent(event);
}

// The close slot and the middle button close a tab when released over it, as a button would
void tab_strip::mousePressEvent(QMouseEvent* event) {
    QPoint point = event->position().toPoint();
    int tab = tabAt(point);
    bool is_on_slot = tab >= 0 && close_slot(tab).contains(point);
    if ((event->button() == Qt::LeftButton && is_on_slot) || (event->button() == Qt::MiddleButton && tab >= 0)) {
        m_pressed_slot = tab;
        event->accept();
        return;
    }

    QTabBar::mousePressEvent(event);
}

void tab_strip::mouseReleaseEvent(QMouseEvent* event) {
    if (m_pressed_slot < 0) {
        QTabBar::mouseReleaseEvent(event);
        return;
    }

    QPoint point = event->position().toPoint();
    int pressed = m_pressed_slot;
    m_pressed_slot = -1;
    bool is_released_on_tab = tabAt(point) == pressed;
    if (is_released_on_tab && (event->button() == Qt::MiddleButton || close_slot(pressed).contains(point))) {
        Q_EMIT tabCloseRequested(pressed);
    }

    event->accept();
}

void tab_strip::contextMenuEvent(QContextMenuEvent* event) {
    int tab = tabAt(event->pos());
    if (tab >= 0 && on_context_menu) {
        on_context_menu(tab, event->globalPos());
    }
}

void tab_strip::leaveEvent(QEvent* event) {
    set_hover(-1, false);
    QTabBar::leaveEvent(event);
}

QRect tab_strip::close_slot(int index) const {
    QRect rect = tabRect(index);
    return QRect(rect.right() + 1 - TAB_PADDING_RIGHT - SLOT_SIZE, rect.top() + (rect.height() - SLOT_SIZE) / 2,
                 SLOT_SIZE, SLOT_SIZE);
}

bool tab_strip::is_tab_modified(int index) const {
    return tabData(index).toBool();
}

void tab_strip::set_hover(int tab, bool is_slot_hovered) {
    if (tab != m_hovered_tab || is_slot_hovered != m_is_slot_hovered) {
        m_hovered_tab = tab;
        m_is_slot_hovered = is_slot_hovered;
        update();
    }
}

tab_row::tab_row(QWidget* parent) : QWidget(parent), m_menu(new menu_button(this)), m_tabs(new tab_strip(this)) {
    setFixedHeight(ROW_HEIGHT);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(ROW_INSET_X, 0, ROW_INSET_X, 0);
    layout->setSpacing(0);

    layout->addWidget(m_menu, 0, Qt::AlignVCenter);
    layout->addSpacing(MENU_BUTTON_GAP);
    layout->addWidget(m_tabs, 0, Qt::AlignBottom);
    layout->addStretch(1);

    // The gap in the hairline follows the open tab wherever the strip lays it out
    QObject::connect(m_tabs, &QTabBar::currentChanged, this, [this] { update(); });
    m_tabs->installEventFilter(this);
}

bool tab_row::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_tabs && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        update();
    }

    return QWidget::eventFilter(watched, event);
}

void tab_row::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && on_empty_double_click) {
        on_empty_double_click();
    }
}

void tab_row::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    int bottom = height() - 1;
    int current = m_tabs->currentIndex();
    if (current < 0) {
        painter.fillRect(QRect(0, bottom, width(), 1), LINE);
        return;
    }

    QRect open = m_tabs->tabRect(current).translated(m_tabs->pos());
    painter.fillRect(QRect(0, bottom, open.left(), 1), LINE);
    painter.fillRect(QRect(open.right() + 1, bottom, width() - open.right() - 1, 1), LINE);
}
