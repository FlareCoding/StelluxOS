#include "tab_strip.h"
#include "menu_style.h"

#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
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
static constexpr int LOCK_SIZE = 12;
static constexpr int LOCK_LABEL_GAP = 6;
static constexpr qreal LOCK_STROKE = 1.3;
static constexpr int TAB_MODIFIED = 1;
static constexpr int TAB_LOCKED = 2;
static constexpr int TAB_STRUCK = 4;
static constexpr qreal CLOSE_GLYPH_HALF = 3;
static constexpr qreal CLOSE_GLYPH_STROKE = 1.4;
static constexpr qreal HALF_PIXEL = 0.5;
static constexpr qreal POINTS_PER_INCH = 72;
static constexpr qreal SCREEN_DPI = 96;
static constexpr int FADE_WIDTH = 36;
static constexpr int WHEEL_STEP = 48;
static constexpr int WHEEL_NOTCH = 120;
static constexpr int LIST_BUTTON_HEIGHT = 28;
static constexpr int LIST_BUTTON_PADDING_X = 8;
static constexpr int LIST_BUTTON_GAP = 6;
static constexpr qreal CHEVRON_HALF = 3;
static constexpr qreal CHEVRON_STROKE = 1.4;

static const QColor LINE = QColor::fromRgba(STLXTHEME_LINE);
static const QColor SHADE = QColor::fromRgba(STLXTHEME_SHADE);
static const QColor TEXT = QColor::fromRgba(STLXTHEME_TEXT);
static const QColor TEXT_SECONDARY = QColor::fromRgba(STLXTHEME_TEXT_SECONDARY);
static const QColor TEXT_DIM = QColor::fromRgba(STLXTHEME_TEXT_DIM);
static const QColor TEXT_DISABLED = QColor::fromRgba(STLXTHEME_TEXT_DISABLED);
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

// Fades what is already painted toward transparent over `span`, from opaque at `solid_x` to clear at `clear_x`
static void fade_out(QPainter& painter, const QRect& span, int solid_x, int clear_x) {
    QLinearGradient fade(solid_x, 0, clear_x, 0);
    fade.setColorAt(0, Qt::black);
    fade.setColorAt(1, Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.fillRect(span, fade);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
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

// A padlock drawn in a square: a filled body under an open stroked shackle
static void paint_lock(QPainter& painter, const QRectF& box) {
    QRectF body(box.left() + 2, box.top() + box.height() * 0.45, box.width() - 4, box.height() * 0.5);
    painter.setPen(Qt::NoPen);
    painter.setBrush(TEXT_DIM);
    painter.drawRoundedRect(body, 1.5, 1.5);

    QRectF shackle(box.left() + 3.5, box.top() + 1, box.width() - 7, box.height() * 0.6);
    QPainterPath arch;
    arch.moveTo(shackle.left(), body.top());
    arch.arcTo(shackle, 180, -180);
    arch.lineTo(shackle.right(), body.top());

    painter.setPen(QPen(TEXT_DIM, LOCK_STROKE));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(arch);
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

void tab_strip::set_visible_span(int offset, int width) {
    if (offset != m_span_offset || width != m_span_width) {
        m_span_offset = offset;
        m_span_width = width;
        update();
    }
}

void tab_strip::set_tab_modified(int index, bool is_modified) {
    set_tab_flag(index, TAB_MODIFIED, is_modified);
}

void tab_strip::set_tab_locked(int index, bool is_locked) {
    set_tab_flag(index, TAB_LOCKED, is_locked);
}

void tab_strip::set_tab_struck(int index, bool is_struck) {
    set_tab_flag(index, TAB_STRUCK, is_struck);
}

// The label's width is rounded up, so a name that fits is never cut by a fraction of a pixel
QSize tab_strip::tabSizeHint(int index) const {
    int label = qCeil(QFontMetricsF(m_font).horizontalAdvance(tabText(index)));
    int lock = is_tab_locked(index) ? LOCK_SIZE + LOCK_LABEL_GAP : 0;

    int natural = TAB_PADDING_LEFT + lock + label + LABEL_SLOT_GAP + SLOT_SIZE + TAB_PADDING_RIGHT;
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
        if (is_tab_locked(index)) {
            paint_lock(painter, QRectF(label_left, rect.center().y() - LOCK_SIZE / 2.0, LOCK_SIZE, LOCK_SIZE));
            label_left += LOCK_SIZE + LOCK_LABEL_GAP;
        }

        QRect label(label_left, rect.top(), slot.left() - LABEL_SLOT_GAP - label_left, rect.height());

        QFont name_font = m_font;
        name_font.setStrikeOut(is_tab_struck(index));
        painter.setFont(name_font);
        QColor tone = is_tab_struck(index) ? TEXT_DISABLED : is_current ? TEXT : is_hovered ? TEXT_SECONDARY : TEXT_DIM;
        painter.setPen(tone);

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

    int span_end = m_span_offset + m_span_width;
    if (m_span_offset > 0) {
        fade_out(painter, QRect(m_span_offset, 0, FADE_WIDTH, height()), m_span_offset + FADE_WIDTH, m_span_offset);
    }

    if (m_span_width > 0 && span_end < width()) {
        fade_out(painter, QRect(span_end - FADE_WIDTH, 0, FADE_WIDTH, height()), span_end - FADE_WIDTH, span_end);
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
    return tabData(index).toInt() & TAB_MODIFIED;
}

bool tab_strip::is_tab_locked(int index) const {
    return tabData(index).toInt() & TAB_LOCKED;
}

bool tab_strip::is_tab_struck(int index) const {
    return tabData(index).toInt() & TAB_STRUCK;
}

void tab_strip::set_tab_flag(int index, int flag, bool is_set) {
    int flags = tabData(index).toInt();
    int changed = is_set ? flags | flag : flags & ~flag;
    if (changed != flags) {
        setTabData(index, changed);
        update(tabRect(index));
    }
}

void tab_strip::set_hover(int tab, bool is_slot_hovered) {
    if (tab != m_hovered_tab || is_slot_hovered != m_is_slot_hovered) {
        m_hovered_tab = tab;
        m_is_slot_hovered = is_slot_hovered;
        update();
    }
}

tab_list_button::tab_list_button(QWidget* parent) : QAbstractButton(parent) {
    setFont(QFont(QString::fromLatin1(STLXTHEME_MONO_FONT_FAMILY)));
    QFont caption = font();
    caption.setPointSizeF(STLXTHEME_CAPTION_SIZE * POINTS_PER_INCH / SCREEN_DPI);
    setFont(caption);

    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
    setToolTip(QStringLiteral("Every open tab"));
}

void tab_list_button::set_open(bool is_open) {
    m_is_open = is_open;
    update();
}

void tab_list_button::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QRectF face = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
    if (m_is_open) {
        painter.setPen(QPen(QColor::fromRgba(STLXTHEME_LINE_ON), 1));
        painter.setBrush(QColor::fromRgba(STLXTHEME_SURFACE_ON));
        painter.drawRoundedRect(face, STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);
    } else if (underMouse()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(SURFACE_HOVER);
        painter.drawRoundedRect(face, STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);
    }

    int chevron_room = LIST_BUTTON_GAP + 2 * CHEVRON_HALF;
    QRect label = rect().adjusted(LIST_BUTTON_PADDING_X, 0, -(LIST_BUTTON_PADDING_X + chevron_room), 0);
    painter.setPen(TEXT_DIM);
    painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeft, text());

    QPointF tip(width() - LIST_BUTTON_PADDING_X - CHEVRON_HALF, height() / 2.0 + CHEVRON_HALF / 2);
    painter.setPen(QPen(TEXT_DIM, CHEVRON_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(tip + QPointF(-CHEVRON_HALF, -CHEVRON_HALF), tip);
    painter.drawLine(tip, tip + QPointF(CHEVRON_HALF, -CHEVRON_HALF));
}

QSize tab_list_button::sizeHint() const {
    int label = fontMetrics().horizontalAdvance(text());
    return QSize(2 * LIST_BUTTON_PADDING_X + label + LIST_BUTTON_GAP + 2 * static_cast<int>(CHEVRON_HALF),
                 LIST_BUTTON_HEIGHT);
}

tab_row::tab_row(QWidget* parent)
    : QWidget(parent), m_menu(new menu_button(this)), m_viewport(new QWidget(this)),
      m_tabs(new tab_strip(m_viewport)), m_tab_list(new tab_list_button(this)) {
    setFixedHeight(ROW_HEIGHT);
    m_tab_list->hide();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(ROW_INSET_X, 0, ROW_INSET_X, 0);
    layout->setSpacing(0);

    layout->addWidget(m_menu, 0, Qt::AlignVCenter);
    layout->addSpacing(MENU_BUTTON_GAP);
    layout->addWidget(m_viewport, 1);
    layout->addWidget(m_tab_list, 0, Qt::AlignVCenter);

    // The strip and its hairline gap follow the open tab, wherever the strip scrolls
    QObject::connect(m_tabs, &QTabBar::currentChanged, this, [this] { place_strip(true); });
    m_tabs->installEventFilter(this);
    m_viewport->installEventFilter(this);
}

bool tab_row::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Wheel && (watched == m_tabs || watched == m_viewport)) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        QPoint delta = wheel->angleDelta();
        int notches = delta.x() != 0 ? delta.x() : delta.y();
        m_offset -= notches * WHEEL_STEP / WHEEL_NOTCH;
        place_strip(false);
        return true;
    }

    bool is_layout_change = event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest;
    if (is_layout_change && (watched == m_viewport || watched == m_tabs)) {
        place_strip(true);
    }

    if (event->type() == QEvent::MouseButtonDblClick && watched == m_viewport && on_empty_double_click) {
        on_empty_double_click();
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

void tab_row::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && on_empty_double_click) {
        on_empty_double_click();
    }
}

// The strip keeps its full width inside the viewport, and scrolls the open tab into view when asked
void tab_row::place_strip(bool shows_current) {
    int strip_width = m_tabs->sizeHint().width();
    int view_width = m_viewport->width();
    bool is_overflowing = strip_width > view_width;
    m_tab_list->setText(QString::number(m_tabs->count()));
    // The button's own hidden state holds before the window shows, when every child still reads as not visible
    if (m_tab_list->isHidden() == is_overflowing) {
        m_tab_list->setVisible(is_overflowing);
    }

    int current = m_tabs->currentIndex();
    if (shows_current && current >= 0) {
        QRect open = m_tabs->tabRect(current);
        m_offset = qMin(m_offset, open.left());
        m_offset = qMax(m_offset, open.right() + 1 - view_width);
    }

    m_offset = qBound(0, m_offset, qMax(0, strip_width - view_width));
    m_tabs->setGeometry(-m_offset, m_viewport->height() - TAB_HEIGHT, strip_width, TAB_HEIGHT);
    m_tabs->set_visible_span(m_offset, view_width);
    update();
}

void tab_row::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    int bottom = height() - 1;
    int current = m_tabs->currentIndex();
    if (current < 0) {
        painter.fillRect(QRect(0, bottom, width(), 1), LINE);
        return;
    }

    QRect open = m_tabs->tabRect(current).translated(m_tabs->mapTo(this, QPoint(0, 0)));
    open = open.intersected(m_viewport->geometry());
    if (open.isEmpty()) {
        painter.fillRect(QRect(0, bottom, width(), 1), LINE);
        return;
    }

    painter.fillRect(QRect(0, bottom, open.left(), 1), LINE);
    painter.fillRect(QRect(open.right() + 1, bottom, width() - open.right() - 1, 1), LINE);
}
