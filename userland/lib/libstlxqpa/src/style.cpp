#include "style.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QListView>
#include <QPainter>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTreeView>

static const char* BASE_STYLE = "Fusion";

// stlxui's control metrics
static const int CONTROL_HEIGHT = 28;
static const int CONTROL_RADIUS = 6;
static const int BUTTON_PADDING = 12;
static const int FIELD_PADDING = 8;
static const int CHECK_SIZE = 16;
static const int CHECK_RADIUS = 4;
static const int CHECK_SPACING = 8;
static const int MENU_PADDING = 4;
static const int MENU_ROW_HEIGHT = 24;
static const int MENU_ROW_PADDING = 12;
static const int MENU_SEPARATOR_HEIGHT = 5;
static const int SCROLL_BAR_EXTENT = 8;
static const int SCROLL_THUMB_WIDTH = 4;
static const int SCROLL_THUMB_RADIUS = 2;
static const int SCROLL_THUMB_MIN = 16;
static const int ROW_HEIGHT = 26;
static const int ROW_RADIUS = 4;
static const int ROW_CONTENT_INSET = 8;
static const int ROW_MARKER_WIDTH = 3;
static const int ROW_MARKER_INSET = 5;
static const int ROW_MARKER_RADIUS = 2;

// How far a hovered row's fill moves from the window color toward the surface color
static const float ROW_HOVER_TINT = 0.25f;

// QLineEdit insets its text by this much inside the contents rect on its own
static const int LINE_EDIT_MARGIN = 2;

// The accent's hover and press shades, as factors of QColor::lighter and darker
static const int ACCENT_HOVER_FACTOR = 112;
static const int ACCENT_PRESS_FACTOR = 108;

static const qreal CHECK_MARK_WIDTH = 1.5;

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

// stlxui draws a border or a focus ring as an outer fill under the control's own
static void fill_ringed(QPainter* painter, const QRect& rect, int radius, const QColor& ring,
                        const QColor& fill) {
    fill_rounded(painter, rect, radius, ring);
    fill_rounded(painter, rect.adjusted(1, 1, -1, -1), radius, fill);
}

static void draw_button_surface(const QStyleOption* option, QPainter* painter, bool is_accent) {
    const QPalette& palette = option->palette;
    QColor fill;
    if (is_accent) {
        QColor accent = palette.color(QPalette::Highlight);
        fill = is_pressed(option) ? accent.darker(ACCENT_PRESS_FACTOR)
             : is_hovered(option) ? accent.lighter(ACCENT_HOVER_FACTOR)
                                  : accent;
    } else {
        fill = is_pressed(option) ? palette.color(QPalette::Light)
             : is_hovered(option) ? palette.color(QPalette::Midlight)
                                  : palette.color(QPalette::Button);
    }

    if (has_keyboard_focus(option)) {
        QColor ring = palette.color(is_accent ? QPalette::ButtonText : QPalette::Highlight);
        fill_ringed(painter, option->rect, CONTROL_RADIUS, ring, fill);
        return;
    }

    fill_rounded(painter, option->rect, CONTROL_RADIUS, fill);
}

static bool is_default_button(const QStyleOption* option) {
    const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
    return button && (button->features & QStyleOptionButton::DefaultButton);
}

static void draw_check_box(const QStyleOption* option, QPainter* painter) {
    const QPalette& palette = option->palette;
    QRect box(option->rect.topLeft(), QSize(CHECK_SIZE, CHECK_SIZE));
    box.moveCenter(option->rect.center());

    bool is_checked = option->state & (QStyle::State_On | QStyle::State_NoChange);
    QColor fill = palette.color(is_checked ? QPalette::Highlight : QPalette::Button);
    if (has_keyboard_focus(option)) {
        fill_ringed(painter, box, CHECK_RADIUS, palette.color(QPalette::Highlight), fill);
    } else if (is_hovered(option)) {
        fill_ringed(painter, box, CHECK_RADIUS, palette.color(QPalette::Midlight), fill);
    } else {
        fill_rounded(painter, box, CHECK_RADIUS, fill);
    }

    if (!is_checked) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(palette.color(QPalette::HighlightedText), CHECK_MARK_WIDTH, Qt::SolidLine, Qt::RoundCap,
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

static QStyleOptionMenuItem with_desktop_highlight(const QStyleOptionMenuItem& item) {
    QStyleOptionMenuItem copy = item;
    copy.palette.setColor(QPalette::Highlight, item.palette.color(QPalette::Midlight));
    copy.palette.setColor(QPalette::HighlightedText, item.palette.color(QPalette::Text));
    return copy;
}

static QColor blend_colors(const QColor& from, const QColor& to, float amount) {
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}

// The cells of a row share one panel, rounded only where the row starts and ends
static bool is_row_start(const QStyleOptionViewItem& item) {
    return item.viewItemPosition != QStyleOptionViewItem::Middle && item.viewItemPosition != QStyleOptionViewItem::End;
}

static bool is_row_end(const QStyleOptionViewItem& item) {
    return item.viewItemPosition != QStyleOptionViewItem::Beginning
        && item.viewItemPosition != QStyleOptionViewItem::Middle;
}

// stlxui marks a selected row with the surface and an accent bar, and tints a hovered one
static void draw_row_panel(const QStyleOptionViewItem& item, QPainter* painter) {
    bool is_selected = item.state & QStyle::State_Selected;
    if (!is_selected && !is_hovered(&item)) {
        return;
    }

    const QPalette& palette = item.palette;
    QColor fill = is_selected ? palette.color(QPalette::Button)
                              : blend_colors(palette.color(QPalette::Window), palette.color(QPalette::Button),
                                             ROW_HOVER_TINT);

    // Rounded past the cell's inner edges, which the clip then cuts straight
    QRect panel = item.rect;
    if (!is_row_start(item)) {
        panel.setLeft(panel.left() - ROW_RADIUS);
    }

    if (!is_row_end(item)) {
        panel.setRight(panel.right() + ROW_RADIUS);
    }

    painter->save();
    painter->setClipRect(item.rect, Qt::IntersectClip);
    fill_rounded(painter, panel, ROW_RADIUS, fill);
    painter->restore();

    if (!is_selected || !is_row_start(item)) {
        return;
    }

    QRect marker(item.rect.left(), item.rect.top() + ROW_MARKER_INSET, ROW_MARKER_WIDTH,
                 item.rect.height() - 2 * ROW_MARKER_INSET);
    fill_rounded(painter, marker, ROW_MARKER_RADIUS, palette.color(QPalette::Highlight));
}

// Every cell insets its content alike, which keeps it clear of the selection marker and
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
        thumb = qBound(SCROLL_THUMB_MIN, thumb, length);
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
            return CHECK_SIZE;
        case PM_CheckBoxLabelSpacing:
            return CHECK_SPACING;
        case PM_MenuPanelWidth:
            return 1;
        case PM_MenuHMargin:
        case PM_MenuVMargin:
            return MENU_PADDING - 1;
        case PM_ScrollBarExtent:
            return SCROLL_BAR_EXTENT;
        case PM_ScrollBarSliderMin:
            return SCROLL_THUMB_MIN;
        default:
            return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

QSize QStelluxStyle::sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& contents,
                                      const QWidget* widget) const {
    switch (type) {
        case CT_PushButton:
        case CT_ToolButton:
            return QSize(contents.width() + 2 * BUTTON_PADDING, qMax(contents.height(), CONTROL_HEIGHT));
        case CT_LineEdit:
            if (has_frame(option)) {
                int inset = FIELD_PADDING - LINE_EDIT_MARGIN;
                return QSize(contents.width() + 2 * inset, qMax(contents.height(), CONTROL_HEIGHT));
            }

            break;
        case CT_MenuItem: {
            const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
            if (item && item->menuItemType == QStyleOptionMenuItem::Separator) {
                return QSize(contents.width(), MENU_SEPARATOR_HEIGHT);
            }

            if (item && is_plain_menu_item(*item)) {
                QFontMetrics metrics(item->font);
                int width = metrics.horizontalAdvance(menu_label(*item)) + 2 * MENU_ROW_PADDING;
                QString shortcut = menu_shortcut(*item);
                if (!shortcut.isEmpty()) {
                    width += metrics.horizontalAdvance(shortcut) + MENU_ROW_PADDING;
                }

                return QSize(width, MENU_ROW_HEIGHT);
            }

            break;
        }
        case CT_ItemViewItem: {
            if (!takes_row_look(widget)) {
                break;
            }

            QSize size = QProxyStyle::sizeFromContents(type, option, contents, widget);
            return QSize(size.width() + 2 * ROW_CONTENT_INSET, qMax(size.height(), ROW_HEIGHT));
        }
        default:
            break;
    }

    return QProxyStyle::sizeFromContents(type, option, contents, widget);
}

QRect QStelluxStyle::subElementRect(SubElement element, const QStyleOption* option, const QWidget* widget) const {
    if (element == SE_LineEditContents && has_frame(option)) {
        int inset = FIELD_PADDING - LINE_EDIT_MARGIN;
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
        case PE_PanelLineEdit:
            if (!has_frame(option)) {
                break;
            }

            fill_ringed(painter, option->rect, CONTROL_RADIUS,
                        option->palette.color((option->state & State_HasFocus) ? QPalette::Highlight
                                                                                : QPalette::Midlight),
                        option->palette.color(QPalette::Base));
            return;
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
            painter->fillRect(option->rect, option->palette.color(QPalette::Button));
            return;
        case PE_FrameMenu:
            painter->save();
            painter->setPen(option->palette.color(QPalette::Midlight));
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
        accented.palette.setColor(QPalette::ButtonText, option->palette.color(QPalette::HighlightedText));
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
        QStyleOptionMenuItem highlighted = with_desktop_highlight(*item);
        QProxyStyle::drawControl(element, &highlighted, painter, widget);
        return;
    }

    bool is_enabled = item->state & State_Enabled;
    if ((item->state & State_Selected) && is_enabled) {
        painter->fillRect(item->rect, item->palette.color(QPalette::Midlight));
    }

    QRect text = item->rect.adjusted(MENU_ROW_PADDING, 0, -MENU_ROW_PADDING, 0);
    int flags = Qt::AlignVCenter | Qt::TextSingleLine | Qt::TextHideMnemonic;
    proxy()->drawItemText(painter, text, flags | Qt::AlignLeft, item->palette, is_enabled, menu_label(*item),
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
                ? QRect(slider.left(), slider.bottom() - SCROLL_THUMB_WIDTH + 1, slider.width(), SCROLL_THUMB_WIDTH)
                : QRect(slider.right() - SCROLL_THUMB_WIDTH + 1, slider.top(), SCROLL_THUMB_WIDTH, slider.height());
    bool is_dragged = (bar->state & State_Sunken) && (bar->activeSubControls & SC_ScrollBarSlider);
    fill_rounded(painter, thumb, SCROLL_THUMB_RADIUS,
                 bar->palette.color(is_dragged ? QPalette::Light : QPalette::Midlight));
}
