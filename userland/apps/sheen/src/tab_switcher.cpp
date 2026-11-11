#include "tab_switcher.h"
#include "chrome.h"

#include <QPainter>

static constexpr int PANEL_WIDTH = 520;
static constexpr int PANEL_PADDING = 6;
static constexpr int ROW_HEIGHT = 32;
static constexpr int ROW_PADDING_X = 12;
static constexpr int VISIBLE_ROWS = 10;
static constexpr int DOT_SIZE = 8;
static constexpr qreal LIT_ALPHA = 0.11;
static constexpr qreal HALF_PIXEL = 0.5;

tab_switcher::tab_switcher(QWidget* window) : QWidget(window) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    hide();
}

void tab_switcher::show_list(const QStringList& names, const QList<bool>& unsaved, int lit, int top) {
    m_names = names;
    m_unsaved = unsaved;
    m_lit = lit;

    int rows = qMin(static_cast<int>(names.size()), VISIBLE_ROWS);
    int width = qMin(PANEL_WIDTH, parentWidget()->width() - 2 * PANEL_PADDING);
    setGeometry((parentWidget()->width() - width) / 2, top, width, rows * ROW_HEIGHT + 2 * PANEL_PADDING);
    show();
    raise();
    update();
}

void tab_switcher::set_lit(int lit) {
    m_lit = lit;
    update();
}

// Ten rows show at once, scrolled so the lit one stays among them
void tab_switcher::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QRectF panel = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
    painter.setPen(QPen(token(STLXTHEME_RIM), 1));
    painter.setBrush(token(STLXTHEME_MENU_BG));
    painter.drawRoundedRect(panel, STLXTHEME_MENU_RADIUS, STLXTHEME_MENU_RADIUS);

    int first = qBound(0, m_lit - VISIBLE_ROWS + 1, qMax(0, static_cast<int>(m_names.size()) - VISIBLE_ROWS));
    int last = qMin(static_cast<int>(m_names.size()), first + VISIBLE_ROWS);
    painter.setFont(ui_font(STLXTHEME_BODY_SIZE));
    for (int index = first; index < last; index++) {
        QRect row(PANEL_PADDING, PANEL_PADDING + (index - first) * ROW_HEIGHT, width() - 2 * PANEL_PADDING, ROW_HEIGHT);
        if (index == m_lit) {
            QColor lit = token(STLXTHEME_TEXT);
            lit.setAlphaF(LIT_ALPHA);
            painter.setPen(Qt::NoPen);
            painter.setBrush(lit);
            painter.drawRoundedRect(row, STLXTHEME_MENU_ITEM_RADIUS, STLXTHEME_MENU_ITEM_RADIUS);
        }

        painter.setPen(token(index == m_lit ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY));
        painter.drawText(row.adjusted(ROW_PADDING_X, 0, -ROW_PADDING_X - DOT_SIZE, 0), Qt::AlignVCenter,
                         m_names.at(index));
        if (m_unsaved.value(index)) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(token(STLXTHEME_ACCENT));
            painter.drawEllipse(QPointF(row.right() - ROW_PADDING_X - DOT_SIZE / 2.0, row.center().y() + HALF_PIXEL),
                                DOT_SIZE / 2.0, DOT_SIZE / 2.0);
        }
    }
}
