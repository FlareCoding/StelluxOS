#include "style.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QListView>
#include <QPainter>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTreeView>

#include <stlxtheme/theme.h>

static const char* BASE_STYLE = "Fusion";

static const int SCROLL_BAR_EXTENT = 8;
static const int ROW_CONTENT_INSET = 8;

// QLineEdit insets its text by this much inside the contents rect on its own
static const int LINE_EDIT_MARGIN = 2;

static const qreal CHECK_MARK_WIDTH = 1.5;

// Centers a 1 px anti-aliased line on the pixels it covers
static const qreal HALF_PIXEL = 0.5;

static const QColor ACCENT_HOVER = QColor::fromRgba(STLXTHEME_ACCENT_HOVER);
static const QColor ACCENT_PRESS = QColor::fromRgba(STLXTHEME_ACCENT_PRESS);
static const QColor ACCENT_TEXT = QColor::fromRgba(STLXTHEME_ACCENT_TEXT);
static const QColor CHECKED = QColor::fromRgba(STLXTHEME_TEXT_SECONDARY);
static const QColor LINE_STRONG = QColor::fromRgba(STLXTHEME_LINE_STRONG);
static const QColor MENU_TEXT = QColor::fromRgba(STLXTHEME_TEXT_SECONDARY);
static const QColor MENU_BG = QColor::fromRgba(STLXTHEME_MENU_BG);
static const QColor ROW_HOVER = QColor::fromRgba(STLXTHEME_ROW_HOVER);
static const QColor SELECTED_RING = QColor::fromRgba(STLXTHEME_SELECTED_RING);

static bool is_hovered(const QStyleOption* option) {
    return (option->state & QStyle::State_MouseOver) && (option->state & QStyle::State_Enabled);
}

static bool is_pressed(const QStyleOption* option) {
    return option->state & (QStyle::State_Sunken | QStyle::State_On);
}

// A focus ring shows only when the keyboard moved focus, not after every click
static bool has_keyboard_focus(const QStyleOption* option) {
    return (option->state & QStyle::State_HasFocus) && (option->state & QStyle::State_KeyboardFocusChange);
}

static bool has_frame(const QStyleOption* option) {
    const auto* frame = qstyleoption_cast<const QStyleOptionFrame*>(option);
    return frame && frame->lineWidth > 0;
}

static void fill_rounded(QPainter* painter, const QRect& rect, int radius, const QColor& color) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawRoundedRect(rect, radius, radius);
    painter->restore();
}

// The face sits inside the edge, not under it, so translucent colors never stack
static void draw_framed(QPainter* painter, const QRect& rect, int radius, const QColor& face, const QColor& edge) {
    fill_rounded(painter, rect.adjusted(1, 1, -1, -1), radius - 1, face);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(edge, 1));
    painter->setBrush(Qt::NoBrush);

    QRectF outline = QRectF(rect).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
    painter->drawRoundedRect(outline, radius - HALF_PIXEL, radius - HALF_PIXEL);
    painter->restore();
}

static void draw_focus_rule(QPainter* painter, const QRect& rect, const QColor& accent) {
    painter->fillRect(QRect(rect.left() + STLXTHEME_CONTROL_RADIUS, rect.bottom() - STLXTHEME_FOCUS_RULE_WIDTH,
                            rect.width() - 2 * STLXTHEME_CONTROL_RADIUS, STLXTHEME_FOCUS_RULE_WIDTH),
                      accent);
}

static void draw_button_surface(const QStyleOption* option, QPainter* painter, bool is_accent) {
    const QPalette& palette = option->palette;
    if (is_accent) {
        QColor fill = is_pressed(option) ? ACCENT_PRESS
                    : is_hovered(option) ? ACCENT_HOVER
                                         : palette.color(QPalette::Accent);

        if (has_keyboard_focus(option)) {
            draw_framed(painter, option->rect, STLXTHEME_CONTROL_RADIUS, fill, palette.color(QPalette::ButtonText));
            return;
        }

        fill_rounded(painter, option->rect, STLXTHEME_CONTROL_RADIUS, fill);
        return;
    }

    QColor face = is_pressed(option) ? palette.color(QPalette::Light)
                : is_hovered(option) ? palette.color(QPalette::Midlight)
                                     : palette.color(QPalette::Button);
    bool is_lit = !is_pressed(option) && (is_hovered(option) || has_keyboard_focus(option));

    draw_framed(painter, option->rect, STLXTHEME_CONTROL_RADIUS, face,
                is_lit ? LINE_STRONG : palette.color(QPalette::Mid));
}

static bool is_default_button(const QStyleOption* option) {
    const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
    return button && (button->features & QStyleOptionButton::DefaultButton);
}

// QPushButton keeps its size hint when it becomes the default, so a button that may become one measures as one
static bool may_be_default(const QStyleOption* option) {
    const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
    return button && (button->features & (QStyleOptionButton::DefaultButton | QStyleOptionButton::AutoDefaultButton));
}

static void draw_check_box(const QStyleOption* option, QPainter* painter) {
    const QPalette& palette = option->palette;
    QRect box(option->rect.topLeft(), QSize(STLXTHEME_CHECK_SIZE, STLXTHEME_CHECK_SIZE));
    box.moveCenter(option->rect.center());

    bool is_checked = option->state & (QStyle::State_On | QStyle::State_NoChange);
    bool is_lit = is_hovered(option) || has_keyboard_focus(option);
    QColor face = is_checked ? CHECKED : palette.color(QPalette::Base);
    QColor lit_edge = is_checked ? palette.color(QPalette::Text) : LINE_STRONG;
    QColor rest_edge = is_checked ? CHECKED : palette.color(QPalette::Mid);
    QColor edge = is_lit ? lit_edge : rest_edge;

    draw_framed(painter, box, STLXTHEME_CHECK_RADIUS, face, edge);

    if (!is_checked) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(palette.color(QPalette::Window), CHECK_MARK_WIDTH, Qt::SolidLine, Qt::RoundCap,
                         Qt::RoundJoin));
    painter->translate(box.topLeft());
    if (option->state & QStyle::State_NoChange) {
        painter->drawLine(QPointF(4, 8), QPointF(12, 8));
    } else {
        const QPointF mark[] = { QPointF(4, 8), QPointF(7, 11), QPointF(12, 5) };
        painter->drawPolyline(mark, 3);
    }

    painter->restore();
}

// Menus of text items alone take stlxui's layout, any with icons or checks keep Fusion's
static bool is_plain_menu_item(const QStyleOptionMenuItem& item) {
    return item.menuItemType == QStyleOptionMenuItem::Normal && item.checkType == QStyleOptionMenuItem::NotCheckable
        && item.icon.isNull() && item.maxIconWidth == 0 && !item.menuHasCheckableItems;
}

static QString menu_label(const QStyleOptionMenuItem& item) {
    return item.text.section(QLatin1Char('\t'), 0, 0);
}

static QString menu_shortcut(const QStyleOptionMenuItem& item) {
    return item.text.section(QLatin1Char('\t'), 1);
}

// The cells of a row share one panel, rounded only where the row starts and ends
static bool is_row_start(const QStyleOptionViewItem& item) {
    return item.viewItemPosition != QStyleOptionViewItem::Middle && item.viewItemPosition != QStyleOptionViewItem::End;
}

static bool is_row_end(const QStyleOptionViewItem& item) {
    return item.viewItemPosition != QStyleOptionViewItem::Beginning
        && item.viewItemPosition != QStyleOptionViewItem::Middle;
}

static void draw_row_panel(const QStyleOptionViewItem& item, QPainter* painter) {
    bool is_selected = item.state & QStyle::State_Selected;
    if (!is_selected && !is_hovered(&item)) {
        return;
    }

    // Rounded past the cell's inner edges, which the clip then cuts straight
    QRect panel = item.rect;
    if (!is_row_start(item)) {
        panel.setLeft(panel.left() - STLXTHEME_CONTROL_RADIUS);
    }

    if (!is_row_end(item)) {
        panel.setRight(panel.right() + STLXTHEME_CONTROL_RADIUS);
    }

    painter->save();
    painter->setClipRect(item.rect, Qt::IntersectClip);
    if (is_selected) {
        draw_framed(painter, panel, STLXTHEME_CONTROL_RADIUS, item.palette.color(QPalette::Highlight), SELECTED_RING);
    } else {
        fill_rounded(painter, panel, STLXTHEME_CONTROL_RADIUS, ROW_HOVER);
    }

    painter->restore();
}

// Every cell insets its content alike, which keeps it clear of the selected row's ring and
// lets a size hint, measured without the cell's place in the row, match the painted width
static QStyleOptionViewItem inset_cell_content(const QStyleOptionViewItem& item) {
    QStyleOptionViewItem inset = item;
    inset.rect.adjust(ROW_CONTENT_INSET, 0, -ROW_CONTENT_INSET, 0);
    return inset;
}

static bool is_row_content(QStyle::SubElement element) {
    return element == QStyle::SE_ItemViewItemCheckIndicator || element == QStyle::SE_ItemViewItemDecoration
        || element == QStyle::SE_ItemViewItemText || element == QStyle::SE_ItemViewItemFocusRect;
}

// stlxui draws its lists on the window color, which list and tree views take in every mode
static bool is_list_or_tree(const QWidget* widget) {
    return qobject_cast<const QListView*>(widget) || qobject_cast<const QTreeView*>(widget);
}

// Decided as each item paints, since a list view can switch to an icon grid, which keeps
// Fusion's look like the tables and other item views stlxui lacks
static bool takes_row_look(const QWidget* widget) {
    if (const auto* list = qobject_cast<const QListView*>(widget)) {
        return list->viewMode() == QListView::ListMode;
    }

    return qobject_cast<const QTreeView*>(widget);
}

static QRect scroll_bar_slider(const QStyleOptionSlider& bar) {
    bool is_horizontal = bar.orientation == Qt::Horizontal;
    int length = is_horizontal ? bar.rect.width() : bar.rect.height();
    int range = bar.maximum - bar.minimum;

    int thumb = length;
    if (range > 0) {
        thumb = static_cast<int>(static_cast<qint64>(length) * bar.pageStep / (range + bar.pageStep));
        thumb = qBound(STLXTHEME_SCROLL_THUMB_MIN_LENGTH, thumb, length);
    }

    int start = QStyle::sliderPositionFromValue(bar.minimum, bar.maximum, bar.sliderPosition, length - thumb,
                                                bar.upsideDown);
    if (is_horizontal) {
        return QRect(bar.rect.x() + start, bar.rect.y(), thumb, bar.rect.height());
    }

    return QRect(bar.rect.x(), bar.rect.y() + start, bar.rect.width(), thumb);
}

QStelluxStyle::QStelluxStyle()
    : QProxyStyle(QStyleFactory::create(QString::fromLatin1(BASE_STYLE))) {
}

// List and tree views paint on the window color and track the hovered row
void QStelluxStyle::polish(QWidget* widget) {
    QProxyStyle::polish(widget);

    if (is_list_or_tree(widget)) {
        auto* view = static_cast<QAbstractItemView*>(widget);
        view->viewport()->setBackgroundRole(QPalette::Window);
        view->viewport()->setAttribute(Qt::WA_Hover);
    }
}

void QStelluxStyle::unpolish(QWidget* widget) {
    if (is_list_or_tree(widget)) {
        auto* view = static_cast<QAbstractItemView*>(widget);
        view->viewport()->setBackgroundRole(QPalette::Base);
        view->viewport()->setAttribute(Qt::WA_Hover, false);
    }

    QProxyStyle::unpolish(widget);
}

QStyle* QStelluxStylePlugin::create(const QString& key) {
    if (key.compare(QLatin1String(QStelluxStyle::NAME), Qt::CaseInsensitive) != 0) {
        return nullptr;
    }

    return new QStelluxStyle;
}

int QStelluxStyle::pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const {
    switch (metric) {
        case PM_ButtonDefaultIndicator:
        case PM_ButtonShiftHorizontal:
        case PM_ButtonShiftVertical:
            return 0;
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
            return STLXTHEME_CHECK_SIZE;
        case PM_CheckBoxLabelSpacing:
            return STLXTHEME_CHECK_LABEL_SPACING;
        case PM_MenuPanelWidth:
            return 1;
        case PM_MenuHMargin:
        case PM_MenuVMargin:
            return STLXTHEME_MENU_PADDING - 1;
        case PM_ScrollBarExtent:
            return SCROLL_BAR_EXTENT;
        case PM_ScrollBarSliderMin:
            return STLXTHEME_SCROLL_THUMB_MIN_LENGTH;
        default:
            return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

QSize QStelluxStyle::sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& contents,
                                      const QWidget* widget) const {
    switch (type) {
        case CT_PushButton:
        case CT_ToolButton: {
            int padding = may_be_default(option) ? STLXTHEME_ACCENT_BUTTON_PADDING_X : STLXTHEME_BUTTON_PADDING_X;
            return QSize(contents.width() + 2 * padding, qMax(contents.height(), STLXTHEME_CONTROL_HEIGHT));
        }
        case CT_LineEdit:
            if (has_frame(option)) {
                int inset = STLXTHEME_FIELD_PADDING_X - LINE_EDIT_MARGIN;
                return QSize(contents.width() + 2 * inset, qMax(contents.height(), STLXTHEME_CONTROL_HEIGHT));
            }

            break;
        case CT_MenuItem: {
            const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
            if (item && item->menuItemType == QStyleOptionMenuItem::Separator) {
                return QSize(contents.width(), STLXTHEME_MENU_SEPARATOR_HEIGHT);
            }

            if (item && is_plain_menu_item(*item)) {
                QFontMetrics metrics(item->font);
                int width = metrics.horizontalAdvance(menu_label(*item)) + 2 * STLXTHEME_MENU_ROW_PADDING_X;
                QString shortcut = menu_shortcut(*item);
                if (!shortcut.isEmpty()) {
                    width += metrics.horizontalAdvance(shortcut) + STLXTHEME_MENU_ROW_PADDING_X;
                }

                return QSize(width, STLXTHEME_MENU_ROW_HEIGHT);
            }

            break;
        }
        case CT_ItemViewItem: {
            if (!takes_row_look(widget)) {
                break;
            }

            QSize size = QProxyStyle::sizeFromContents(type, option, contents, widget);
            return QSize(size.width() + 2 * ROW_CONTENT_INSET, qMax(size.height(), STLXTHEME_LIST_ROW_HEIGHT));
        }
        default:
            break;
    }

    return QProxyStyle::sizeFromContents(type, option, contents, widget);
}

QRect QStelluxStyle::subElementRect(SubElement element, const QStyleOption* option, const QWidget* widget) const {
    if (element == SE_LineEditContents && has_frame(option)) {
        int inset = STLXTHEME_FIELD_PADDING_X - LINE_EDIT_MARGIN;
        return option->rect.adjusted(inset, 1, -inset, -1);
    }

    // Painting, hit testing and editors all place a row's content through here
    const auto* view_item = qstyleoption_cast<const QStyleOptionViewItem*>(option);
    if (view_item && is_row_content(element) && takes_row_look(widget)) {
        QStyleOptionViewItem inset = inset_cell_content(*view_item);
        return QProxyStyle::subElementRect(element, &inset, widget);
    }

    return QProxyStyle::subElementRect(element, option, widget);
}

// A scroll bar is its thumb alone, without arrows or a drawn track
QRect QStelluxStyle::subControlRect(ComplexControl control, const QStyleOptionComplex* option, SubControl sub,
                                    const QWidget* widget) const {
    const auto* bar = qstyleoption_cast<const QStyleOptionSlider*>(option);
    if (control != CC_ScrollBar || !bar) {
        return QProxyStyle::subControlRect(control, option, sub, widget);
    }

    QRect track = bar->rect;
    QRect slider = scroll_bar_slider(*bar);
    bool is_horizontal = bar->orientation == Qt::Horizontal;
    switch (sub) {
        case SC_ScrollBarGroove:
            return track;
        case SC_ScrollBarSlider:
            return slider;
        case SC_ScrollBarSubPage:
            return is_horizontal ? QRect(track.topLeft(), QPoint(slider.left() - 1, track.bottom()))
                                 : QRect(track.topLeft(), QPoint(track.right(), slider.top() - 1));
        case SC_ScrollBarAddPage:
            return is_horizontal ? QRect(QPoint(slider.right() + 1, track.top()), track.bottomRight())
                                 : QRect(QPoint(track.left(), slider.bottom() + 1), track.bottomRight());
        default:
            return QRect();
    }
}

void QStelluxStyle::drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                                  const QWidget* widget) const {
    switch (element) {
        case PE_PanelButtonCommand:
            draw_button_surface(option, painter, is_default_button(option));
            return;
        case PE_PanelButtonTool:
            if ((option->state & State_AutoRaise) && !is_hovered(option) && !is_pressed(option)) {
                return;
            }

            draw_button_surface(option, painter, false);
            return;
        case PE_PanelLineEdit: {
            if (!has_frame(option)) {
                break;
            }

            bool is_focused = option->state & State_HasFocus;
            draw_framed(painter, option->rect, STLXTHEME_CONTROL_RADIUS, option->palette.color(QPalette::Base),
                        is_focused ? LINE_STRONG : option->palette.color(QPalette::Mid));
            if (is_focused) {
                draw_focus_rule(painter, option->rect, option->palette.color(QPalette::Accent));
            }

            return;
        }
        case PE_IndicatorCheckBox:
            draw_check_box(option, painter);
            return;
        case PE_PanelItemViewItem: {
            const auto* view_item = qstyleoption_cast<const QStyleOptionViewItem*>(option);
            if (!view_item || !takes_row_look(widget)) {
                break;
            }

            // The model's background brush, then the panel marking selection and hover over it
            QStyleOptionViewItem background = *view_item;
            background.state &= ~(State_Selected | State_MouseOver);
            QProxyStyle::drawPrimitive(element, &background, painter, widget);
            draw_row_panel(*view_item, painter);
            return;
        }
        case PE_PanelItemViewRow: {
            // Selection shows only in the item panels, so a row background keeps its alternating color
            const auto* view_item = qstyleoption_cast<const QStyleOptionViewItem*>(option);
            if (view_item && (option->state & State_Selected) && takes_row_look(widget)) {
                QStyleOptionViewItem unselected = *view_item;
                unselected.state &= ~State_Selected;
                QProxyStyle::drawPrimitive(element, &unselected, painter, widget);
                return;
            }

            break;
        }
        case PE_PanelMenu:
            painter->fillRect(option->rect, MENU_BG);
            return;
        case PE_FrameMenu:
            painter->save();
            painter->setPen(option->palette.color(QPalette::Mid));
            painter->drawRect(option->rect.adjusted(0, 0, -1, -1));
            painter->restore();
            return;
        case PE_FrameFocusRect:
            // Buttons and check boxes ring their own surface on keyboard focus
            if (qobject_cast<const QAbstractButton*>(widget)) {
                return;
            }

            break;
        case PE_FrameDefaultButton:
            return;
        default:
            break;
    }

    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

void QStelluxStyle::drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                                const QWidget* widget) const {
    const auto* view_item = qstyleoption_cast<const QStyleOptionViewItem*>(option);
    if (element == CE_ItemViewItem && view_item && takes_row_look(widget)) {
        proxy()->drawPrimitive(PE_PanelItemViewItem, view_item, painter, widget);

        // The background is painted and the panel marks selection, hover and the selected
        // current item, so the content draws plain over them
        QStyleOptionViewItem content = *view_item;
        content.backgroundBrush = Qt::NoBrush;
        content.state &= ~(State_Selected | State_MouseOver);
        if (view_item->state & State_Selected) {
            content.state &= ~State_HasFocus;
        }

        QProxyStyle::drawControl(element, &content, painter, widget);
        return;
    }

    if (element == CE_PushButtonLabel && is_default_button(option)) {
        QStyleOptionButton accented = *qstyleoption_cast<const QStyleOptionButton*>(option);
        accented.palette.setColor(QPalette::ButtonText, ACCENT_TEXT);
        QProxyStyle::drawControl(element, &accented, painter, widget);
        return;
    }

    const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
    if (element != CE_MenuItem || !item) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }

    // stlxui separates menu rows with a gap, not a line
    if (item->menuItemType == QStyleOptionMenuItem::Separator) {
        return;
    }

    if (!is_plain_menu_item(*item)) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }

    bool is_enabled = item->state & State_Enabled;
    bool is_lit = (item->state & State_Selected) && is_enabled;
    if (is_lit) {
        fill_rounded(painter, item->rect, STLXTHEME_MENU_ITEM_RADIUS, item->palette.color(QPalette::Highlight));
    }

    QPalette palette = item->palette;
    if (is_enabled && !is_lit) {
        palette.setColor(QPalette::Text, MENU_TEXT);
    }

    QRect text = item->rect.adjusted(STLXTHEME_MENU_ROW_PADDING_X, 0, -STLXTHEME_MENU_ROW_PADDING_X, 0);
    int flags = Qt::AlignVCenter | Qt::TextSingleLine | Qt::TextHideMnemonic;
    proxy()->drawItemText(painter, text, flags | Qt::AlignLeft, palette, is_enabled, menu_label(*item),
                          QPalette::Text);

    QString shortcut = menu_shortcut(*item);
    if (!shortcut.isEmpty()) {
        proxy()->drawItemText(painter, text, flags | Qt::AlignRight, item->palette, is_enabled, shortcut,
                              QPalette::PlaceholderText);
    }
}

void QStelluxStyle::drawComplexControl(ComplexControl control, const QStyleOptionComplex* option, QPainter* painter,
                                       const QWidget* widget) const {
    const auto* bar = qstyleoption_cast<const QStyleOptionSlider*>(option);
    if (control != CC_ScrollBar || !bar) {
        QProxyStyle::drawComplexControl(control, option, painter, widget);
        return;
    }

    if (bar->maximum == bar->minimum) {
        return;
    }

    QRect slider = scroll_bar_slider(*bar);
    QRect thumb = bar->orientation == Qt::Horizontal
                ? QRect(slider.left(), slider.bottom() - STLXTHEME_SCROLL_THUMB_WIDTH + 1,
                        slider.width(), STLXTHEME_SCROLL_THUMB_WIDTH)
                : QRect(slider.right() - STLXTHEME_SCROLL_THUMB_WIDTH + 1, slider.top(),
                        STLXTHEME_SCROLL_THUMB_WIDTH, slider.height());
    bool is_dragged = (bar->state & State_Sunken) && (bar->activeSubControls & SC_ScrollBarSlider);
    fill_rounded(painter, thumb, STLXTHEME_SCROLL_THUMB_RADIUS,
                 is_dragged ? bar->palette.color(QPalette::PlaceholderText) : LINE_STRONG);
}
