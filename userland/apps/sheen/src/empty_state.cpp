#include "empty_state.h"
#include "chrome.h"
#include "keycaps.h"

#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

static constexpr int BLOCK_WIDTH = 400;
static constexpr int ICON_TITLE_GAP = 12;
static constexpr int TITLE_HEIGHT = 20;
static constexpr int HINT_HEIGHT = 18;
static constexpr int HINT_ROWS_GAP = 16;
static constexpr int ACTION_ROW_HEIGHT = 36;
static constexpr int ROW_GAP = 2;
static constexpr int ROW_PADDING_X = 12;
static constexpr int CAPTION_TOP_GAP = 14;
static constexpr int CAPTION_HEIGHT = 22;
static constexpr int FILE_ROW_HEIGHT = 30;
static constexpr int FILE_LIMIT = 5;
static constexpr int NAME_PATH_GAP = 16;
static constexpr int GLYPH_SIZE = 16;
static constexpr int GLYPH_GAP = 10;
static constexpr qreal GLYPH_CUT = 1.5;
static constexpr qreal CAPTION_LETTER_SPACING = 106; // percent, 0.06 em between letters

// Drawn at its own size, since a scaled icon blurs
static const char* const ICON_PATH = "/etc/res/icons/sheen-32.png";

// A glyph's filled shape on a 16 px grid
static void fill_glyph(QPainter* painter, row_glyph glyph) {
    switch (glyph) {
        case row_glyph::DOC:
            painter->drawRoundedRect(QRectF(3, 1.5, 10, 13), 1.5, 1.5);
            break;
        case row_glyph::FOLDER:
            painter->drawRoundedRect(QRectF(1.5, 2.5, 6, 3), 1, 1);
            painter->drawRoundedRect(QRectF(1.5, 4, 13, 9.5), 1.5, 1.5);
            break;
        case row_glyph::CLOCK:
            painter->drawEllipse(QPointF(8, 8), 6.5, 6.5);
            break;
        case row_glyph::TERMINAL:
            painter->drawRoundedRect(QRectF(1.5, 2.5, 13, 11), 2, 2);
            break;
    }
}

// The lines cut through a filled glyph: a page's text, a clock's hands and a terminal's prompt
static void cut_glyph(QPainter* painter, row_glyph glyph) {
    switch (glyph) {
        case row_glyph::DOC:
            painter->drawLine(QPointF(5.75, 6.5), QPointF(10.25, 6.5));
            painter->drawLine(QPointF(5.75, 9.5), QPointF(10.25, 9.5));
            break;
        case row_glyph::FOLDER:
            break;
        case row_glyph::CLOCK:
            painter->drawPolyline(QPolygonF({ QPointF(8, 4.5), QPointF(8, 8), QPointF(10.5, 9.5) }));
            break;
        case row_glyph::TERMINAL:
            painter->drawPolyline(QPolygonF({ QPointF(4.5, 6), QPointF(6.75, 8), QPointF(4.5, 10) }));
            painter->drawLine(QPointF(8.5, 10.25), QPointF(11.5, 10.25));
            break;
    }
}

// Filled in the row's tone with its details cut through, so the row's lit face shows in them
static QPixmap glyph_pixmap(row_glyph glyph, const QColor& color) {
    QPixmap pixmap(GLYPH_SIZE, GLYPH_SIZE);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    fill_glyph(&painter, glyph);

    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.setPen(QPen(Qt::black, GLYPH_CUT, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    cut_glyph(&painter, glyph);
    painter.end();
    return pixmap;
}

empty_state::empty_state(QWidget* parent) : QWidget(parent), m_icon(QString::fromLatin1(ICON_PATH)) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

void empty_state::set_entries(const QList<empty_action>& actions, const QStringList& files) {
    m_actions = actions;
    m_files = files.mid(0, FILE_LIMIT);
    m_lit = 0;
    update();
}

int empty_state::icon_room() const {
    return m_icon.isNull() ? 0 : m_icon.height() + ICON_TITLE_GAP;
}

int empty_state::row_count() const {
    return m_actions.size() + m_files.size();
}

// The block of icon, words and rows sits a little above the middle, as the eye expects
int empty_state::block_top() const {
    int rows = m_actions.size() * (ACTION_ROW_HEIGHT + ROW_GAP);
    int files = m_files.isEmpty() ? 0 : CAPTION_TOP_GAP + CAPTION_HEIGHT + m_files.size() * FILE_ROW_HEIGHT;
    int block = icon_room() + TITLE_HEIGHT + HINT_HEIGHT + HINT_ROWS_GAP + rows + files;
    return qMax(0, (height() - block) * 2 / 5);
}

QRect empty_state::row_rect(int row) const {
    int left = (width() - BLOCK_WIDTH) / 2;
    int top = block_top() + icon_room() + TITLE_HEIGHT + HINT_HEIGHT + HINT_ROWS_GAP;
    if (row < m_actions.size()) {
        return QRect(left, top + row * (ACTION_ROW_HEIGHT + ROW_GAP), BLOCK_WIDTH, ACTION_ROW_HEIGHT);
    }

    top += m_actions.size() * (ACTION_ROW_HEIGHT + ROW_GAP) + CAPTION_TOP_GAP + CAPTION_HEIGHT;
    return QRect(left, top + (row - m_actions.size()) * FILE_ROW_HEIGHT, BLOCK_WIDTH, FILE_ROW_HEIGHT);
}

int empty_state::row_at(const QPoint& point) const {
    for (int row = 0; row < row_count(); row++) {
        if (row_rect(row).contains(point)) {
            return row;
        }
    }

    return -1;
}

void empty_state::run_row(int row) {
    if (row < 0 || row >= row_count()) {
        return;
    }

    if (row < m_actions.size()) {
        m_actions.at(row).run();
    } else if (on_file_chosen) {
        on_file_chosen(m_files.at(row - m_actions.size()));
    }
}

void empty_state::light_row(int row) {
    if (row >= 0 && row != m_lit) {
        m_lit = row;
        update();
    }
}

void empty_state::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int top = block_top();
    if (!m_icon.isNull()) {
        painter.drawPixmap((width() - m_icon.width()) / 2, top, m_icon);
    }

    QRect title(0, top + icon_room(), width(), TITLE_HEIGHT);
    painter.setFont(ui_font(STLXTHEME_BODY_SIZE, QFont::DemiBold));
    painter.setPen(token(STLXTHEME_TEXT));
    painter.drawText(title, Qt::AlignCenter, QStringLiteral("No open files"));

    QRect hint(0, title.bottom() + 1, width(), HINT_HEIGHT);
    painter.setFont(ui_font(STLXTHEME_CAPTION_SIZE));
    painter.setPen(token(STLXTHEME_TEXT_DIM));
    painter.drawText(hint, Qt::AlignCenter, QStringLiteral("Ctrl W again closes the window"));

    for (int row = 0; row < row_count(); row++) {
        QRect rect = row_rect(row);
        bool is_lit = row == m_lit;
        if (is_lit) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(token(STLXTHEME_SURFACE_SELECTED));
            painter.drawRoundedRect(rect, STLXTHEME_MENU_ITEM_RADIUS, STLXTHEME_MENU_ITEM_RADIUS);
        }

        QRect room = rect.adjusted(ROW_PADDING_X, 0, -ROW_PADDING_X, 0);
        QColor text = token(is_lit ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY);
        row_glyph glyph = row < m_actions.size() ? m_actions.at(row).glyph : row_glyph::DOC;
        painter.drawPixmap(room.left(), rect.center().y() - GLYPH_SIZE / 2, glyph_pixmap(glyph, text));
        room.setLeft(room.left() + GLYPH_SIZE + GLYPH_GAP);
        if (row < m_actions.size()) {
            const empty_action& action = m_actions.at(row);
            painter.setFont(ui_font(STLXTHEME_BODY_SIZE));
            painter.setPen(text);
            painter.drawText(room, Qt::AlignLeft | Qt::AlignVCenter, action.label);
            paint_keycaps(&painter, action.keys, room.right() + 1 - keycaps_width(action.keys), rect.center().y(),
                          token(is_lit ? STLXTHEME_TEXT_SECONDARY : STLXTHEME_TEXT_DIM));
            continue;
        }

        QFileInfo file(m_files.at(row - m_actions.size()));
        QFont path_font = mono_font(STLXTHEME_CAPTION_SIZE);
        int path_width = QFontMetrics(path_font).horizontalAdvance(file.absolutePath());
        int name_width = qMax(0, room.width() - path_width - NAME_PATH_GAP);

        painter.setFont(ui_font(STLXTHEME_BODY_SIZE));
        painter.setPen(text);
        painter.drawText(room, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(file.fileName(), Qt::ElideMiddle, name_width));

        painter.setFont(path_font);
        painter.setPen(token(STLXTHEME_TEXT_DIM));
        painter.drawText(room, Qt::AlignRight | Qt::AlignVCenter, file.absolutePath());
    }

    if (m_files.isEmpty()) {
        return;
    }

    QRect first_file = row_rect(m_actions.size());
    QRect caption(first_file.left() + ROW_PADDING_X, first_file.top() - CAPTION_HEIGHT, BLOCK_WIDTH, CAPTION_HEIGHT);
    QFont caption_font = ui_font(STLXTHEME_CAPTION_SIZE, QFont::DemiBold);
    caption_font.setCapitalization(QFont::AllUppercase);
    caption_font.setLetterSpacing(QFont::PercentageSpacing, CAPTION_LETTER_SPACING);

    painter.setFont(caption_font);
    painter.setPen(token(STLXTHEME_TEXT_DIM));
    painter.drawText(caption, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Closed this session"));
}

void empty_state::keyPressEvent(QKeyEvent* event) {
    int count = row_count();
    if (count == 0) {
        QWidget::keyPressEvent(event);
        return;
    }

    switch (event->key()) {
        case Qt::Key_Up:
            light_row((m_lit + count - 1) % count);
            break;
        case Qt::Key_Down:
            light_row((m_lit + 1) % count);
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            run_row(m_lit);
            break;
        default:
            QWidget::keyPressEvent(event);
            break;
    }
}

void empty_state::mouseMoveEvent(QMouseEvent* event) {
    light_row(row_at(event->position().toPoint()));
}

void empty_state::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        run_row(row_at(event->position().toPoint()));
    }
}
