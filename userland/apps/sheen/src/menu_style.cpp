#include "menu_style.h"
#include "chrome.h"
#include "keycaps.h"

#include <QApplication>
#include <QPainter>
#include <QStyleFactory>
#include <QStyleOptionMenuItem>

static constexpr int ROW_PADDING_X = STLXTHEME_MENU_ROW_PADDING_X;
static constexpr int SEPARATOR_HEIGHT = 9;
static constexpr int CHECK_COLUMN = 14;
static constexpr int CHECK_GAP = 6;
static constexpr int LABEL_KEYS_GAP = 24;
static constexpr int CHEVRON_WIDTH = 10;
static constexpr int CHEVRON_GAP = 12;
static constexpr int RADIO_DOT = 6;
static constexpr qreal CHECK_STROKE = 1.5;
static constexpr qreal CHEVRON_STROKE = 1.4;
static constexpr qreal HALF_PIXEL = 0.5;

static constexpr int BUTTON_SIZE = 28;
static constexpr qreal GLYPH_HALF_WIDTH = 6;
static constexpr int GLYPH_LINE_GAP = 4;
static constexpr qreal GLYPH_STROKE = 1.5;

struct key_name {
    const char* qt;
    const char* shown;
};

// Keys the keyboard map names differently from Qt
static constexpr key_name KEY_NAMES[] = {
    { "Return", "Enter" },
    { "PgDown", "PgDn" },
};

static QString row_label(const QStyleOptionMenuItem& item) {
    return item.text.section(QLatin1Char('\t'), 0, 0);
}

// The keys of the row's shortcut, such as Ctrl, Shift and S, each drawn as one keycap
static QStringList row_keys(const QStyleOptionMenuItem& item) {
    QString shortcut = item.text.section(QLatin1Char('\t'), 1);
    if (shortcut.isEmpty()) {
        return QStringList();
    }

    QStringList keys = shortcut.split(QLatin1Char('+'));
    if (shortcut.endsWith(QLatin1String("++"))) {
        keys.removeLast();
        keys.last() = QStringLiteral("+");
    }

    for (QString& key : keys) {
        for (const key_name& name : KEY_NAMES) {
            if (key == QLatin1String(name.qt)) {
                key = QString::fromLatin1(name.shown);
            }
        }
    }

    return keys;
}

static void paint_check(QPainter* painter, const QStyleOptionMenuItem& item, const QRect& column,
                        const QColor& color) {
    QPointF center = QRectF(column).center();
    if (item.checkType == QStyleOptionMenuItem::Exclusive) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawEllipse(center, RADIO_DOT / 2.0, RADIO_DOT / 2.0);
        return;
    }

    const QPointF mark[] = { center + QPointF(-4, 0), center + QPointF(-1, 3), center + QPointF(4, -3) };
    painter->setPen(QPen(color, CHECK_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPolyline(mark, 3);
}

static void paint_chevron(QPainter* painter, int right, int center_y) {
    qreal x = right - CHEVRON_WIDTH / 2.0;
    const QPointF chevron[] = { QPointF(x - 2, center_y - 4), QPointF(x + 2, center_y), QPointF(x - 2, center_y + 4) };
    painter->setPen(QPen(token(STLXTHEME_TEXT_DIM), CHEVRON_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPolyline(chevron, 3);
}

menu_style::menu_style() : QProxyStyle(QStyleFactory::create(QApplication::style()->name())) {
}

void menu_style::drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                             const QWidget* widget) const {
    const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
    if (element != CE_MenuItem || !item) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    QRect rect = item->rect;
    if (item->menuItemType == QStyleOptionMenuItem::Separator) {
        painter->fillRect(QRect(rect.left() + ROW_PADDING_X, rect.center().y(), rect.width() - 2 * ROW_PADDING_X, 1),
                          token(STLXTHEME_LINE_SUBTLE));
        painter->restore();
        return;
    }

    bool is_enabled = item->state & State_Enabled;
    bool is_lit = (item->state & State_Selected) && is_enabled;
    if (is_lit) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(token(STLXTHEME_SURFACE_SELECTED));
        painter->drawRoundedRect(rect, STLXTHEME_MENU_ITEM_RADIUS, STLXTHEME_MENU_ITEM_RADIUS);
    }

    QColor text = token(!is_enabled ? STLXTHEME_TEXT_DISABLED : is_lit ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY);
    int x = rect.left() + ROW_PADDING_X;
    if (item->menuHasCheckableItems) {
        QRect column(x, rect.top(), CHECK_COLUMN, rect.height());
        if (item->checked) {
            paint_check(painter, *item, column, text);
        }

        x += CHECK_COLUMN + CHECK_GAP;
    }

    painter->setFont(item->font);
    painter->setPen(text);
    painter->drawText(QRect(x, rect.top(), rect.right() - x, rect.height()), Qt::AlignVCenter | Qt::TextSingleLine,
                      row_label(*item));

    int right = rect.right() - ROW_PADDING_X;
    if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
        paint_chevron(painter, right, rect.center().y());
        right -= CHEVRON_WIDTH;
    }

    QStringList keys = row_keys(*item);
    if (!keys.isEmpty()) {
        QColor key_color = token(is_enabled ? STLXTHEME_TEXT_DIM : STLXTHEME_TEXT_DISABLED);
        paint_keycaps(painter, keys, right - keycaps_width(keys), rect.center().y(), key_color);
    }

    painter->restore();
}

QSize menu_style::sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& contents,
                                   const QWidget* widget) const {
    const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
    if (type != CT_MenuItem || !item) {
        return QProxyStyle::sizeFromContents(type, option, contents, widget);
    }

    if (item->menuItemType == QStyleOptionMenuItem::Separator) {
        return QSize(contents.width(), SEPARATOR_HEIGHT);
    }

    int width = 2 * ROW_PADDING_X + QFontMetrics(item->font).horizontalAdvance(row_label(*item));
    if (item->menuHasCheckableItems) {
        width += CHECK_COLUMN + CHECK_GAP;
    }

    QStringList keys = row_keys(*item);
    if (!keys.isEmpty()) {
        width += LABEL_KEYS_GAP + keycaps_width(keys);
    }

    if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
        width += CHEVRON_GAP + CHEVRON_WIDTH;
    }

    return QSize(width, STLXTHEME_MENU_ROW_HEIGHT);
}

menu_button::menu_button(QWidget* parent) : QAbstractButton(parent) {
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
    setToolTip(QStringLiteral("Menu (F10)"));
}

void menu_button::set_open(bool is_open) {
    m_is_open = is_open;
    update();
}

void menu_button::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QRectF face = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
    if (m_is_open) {
        painter.setPen(QPen(token(STLXTHEME_LINE_ON), 1));
        painter.setBrush(token(STLXTHEME_SURFACE_ON));
        painter.drawRoundedRect(face, STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);
    } else if (underMouse() && isEnabled()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(token(STLXTHEME_SURFACE_HOVER));
        painter.drawRoundedRect(face, STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);
    }

    QPointF center = QRectF(rect()).center();
    QColor glyph = token(isEnabled() ? STLXTHEME_TEXT_SECONDARY : STLXTHEME_TEXT_DISABLED);
    painter.setPen(QPen(glyph, GLYPH_STROKE, Qt::SolidLine, Qt::RoundCap));
    for (int line = -1; line <= 1; line++) {
        qreal y = center.y() + line * GLYPH_LINE_GAP;
        painter.drawLine(QPointF(center.x() - GLYPH_HALF_WIDTH, y), QPointF(center.x() + GLYPH_HALF_WIDTH, y));
    }
}

QSize menu_button::sizeHint() const {
    return QSize(BUTTON_SIZE, BUTTON_SIZE);
}
