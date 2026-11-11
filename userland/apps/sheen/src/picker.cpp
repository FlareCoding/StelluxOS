#include "picker.h"
#include "chrome.h"
#include "keycaps.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

static constexpr int PICKER_WIDTH = 560;
static constexpr int PICKER_TOP = 8;
static constexpr int PANEL_PADDING = 8;
static constexpr int FIELD_HEIGHT = 36;
static constexpr int FIELD_PADDING_X = 12;
static constexpr int PREFIX_GAP = 6;
static constexpr int HELP_HEIGHT = 30;
static constexpr int UNDERLINE_HEIGHT = 2;
static constexpr int LIST_TOP_GAP = 6;
static constexpr int ROW_HEIGHT = 32;
static constexpr int ROW_GAP = 1;
static constexpr int HEADING_HEIGHT = 24;
static constexpr int ROW_PADDING_X = 10;
static constexpr int HEADING_PADDING_BOTTOM = 3;
static constexpr int VISIBLE_ROWS = 10;
static constexpr int CHECK_SIZE = 12;
static constexpr qreal CHECK_STROKE = 1.5;
static constexpr qreal HEADING_LETTER_SPACING = 106; // percent, 0.06 em between letters
static constexpr qreal MATCH_ALPHA = 0.13;
static constexpr qreal MATCH_RADIUS = 2;
static constexpr qreal HALF_PIXEL = 0.5;
static constexpr int ROW_ROLE = Qt::UserRole;
static constexpr int HEADING_ROLE = Qt::UserRole + 1;
static constexpr int CURRENT_ROLE = Qt::UserRole + 2;
static constexpr int DETAIL_ROLE = Qt::UserRole + 3;
static constexpr int KEYS_ROLE = Qt::UserRole + 4;
static constexpr int MARKS_ROLE = Qt::UserRole + 5;
static constexpr int NOTE_ROLE = Qt::UserRole + 6;

namespace {

/* The recessed query row, underlined in amber since it always holds the focus while the picker is open */
class query_row : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QRectF edge = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
        painter.setPen(QPen(token(STLXTHEME_LINE_STRONG), 1));
        painter.setBrush(token(STLXTHEME_RECESS));
        painter.drawRoundedRect(edge, STLXTHEME_CONTROL_RADIUS - HALF_PIXEL, STLXTHEME_CONTROL_RADIUS - HALF_PIXEL);

        QPainterPath shape;
        shape.addRoundedRect(QRectF(rect()), STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);
        painter.setClipPath(shape);
        painter.fillRect(QRectF(0, height() - UNDERLINE_HEIGHT, width(), UNDERLINE_HEIGHT), token(STLXTHEME_ACCENT));
    }
};

/* A list row in the menus' look with the letters matching the query marked and a check on the current choice, or a
 * section heading in small capitals */
class row_delegate : public QStyledItemDelegate {
public:
    row_delegate(const QString* filter, QObject* parent) : QStyledItemDelegate(parent), m_filter(filter) {
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        bool is_heading = index.data(HEADING_ROLE).toBool();
        return QSize(option.rect.width(), (is_heading ? HEADING_HEIGHT : ROW_HEIGHT) + ROW_GAP);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QRect rect = option.rect.adjusted(0, 0, 0, -ROW_GAP);
        QRect room = rect.adjusted(ROW_PADDING_X, 0, -ROW_PADDING_X, 0);
        QString label = index.data().toString();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        if (index.data(HEADING_ROLE).toBool()) {
            QFont font = ui_font(STLXTHEME_CAPTION_SIZE, QFont::DemiBold);
            font.setCapitalization(QFont::AllUppercase);
            font.setLetterSpacing(QFont::PercentageSpacing, HEADING_LETTER_SPACING);

            painter->setFont(font);
            painter->setPen(token(STLXTHEME_TEXT_DIM));
            painter->drawText(room.adjusted(0, 0, 0, -HEADING_PADDING_BOTTOM), Qt::AlignLeft | Qt::AlignBottom, label);
            painter->restore();
            return;
        }

        bool is_lit = option.state & QStyle::State_Selected;
        if (is_lit) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(token(STLXTHEME_SURFACE_SELECTED));
            painter->drawRoundedRect(rect, STLXTHEME_MENU_ITEM_RADIUS, STLXTHEME_MENU_ITEM_RADIUS);
        }

        QFont font = ui_font(STLXTHEME_BODY_SIZE);
        QFontMetricsF metrics(font);
        QString detail = index.data(DETAIL_ROLE).toString();
        qreal label_left = room.left() + metrics.horizontalAdvance(detail);

        QList<int> marks = index.data(MARKS_ROLE).value<QList<int>>();
        qsizetype at = m_filter->isEmpty() ? -1 : label.indexOf(*m_filter, 0, Qt::CaseInsensitive);
        for (qsizetype letter = at; at >= 0 && letter < at + m_filter->size(); letter++) {
            marks.append(static_cast<int>(letter));
        }

        QColor tint = token(STLXTHEME_TEXT);
        tint.setAlphaF(MATCH_ALPHA);
        painter->setPen(Qt::NoPen);
        painter->setBrush(tint);
        for (qsizetype first = 0; first < marks.size();) {
            qsizetype last = first;
            while (last + 1 < marks.size() && marks.at(last + 1) == marks.at(last) + 1) {
                last++;
            }

            qreal left = label_left + metrics.horizontalAdvance(label.left(marks.at(first)));
            qreal width = metrics.horizontalAdvance(label.mid(marks.at(first), last - first + 1));
            painter->drawRoundedRect(QRectF(left, rect.center().y() - metrics.height() / 2, width, metrics.height()),
                                     MATCH_RADIUS, MATCH_RADIUS);
            first = last + 1;
        }

        painter->setFont(font);
        painter->setPen(token(STLXTHEME_TEXT_DIM));
        painter->drawText(room, Qt::AlignLeft | Qt::AlignVCenter, detail);
        painter->setPen(token(is_lit ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY));
        QRectF label_room(label_left, room.top(), room.right() - label_left, room.height());
        painter->drawText(label_room, Qt::AlignLeft | Qt::AlignVCenter, label);

        QStringList keys = index.data(KEYS_ROLE).toStringList();
        QString note = index.data(NOTE_ROLE).toString();
        if (!keys.isEmpty()) {
            paint_keycaps(painter, keys, room.right() + 1 - keycaps_width(keys), rect.center().y(),
                          token(is_lit ? STLXTHEME_TEXT_SECONDARY : STLXTHEME_TEXT_DIM));
        } else if (!note.isEmpty()) {
            painter->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
            painter->setPen(token(STLXTHEME_TEXT_DIM));
            painter->drawText(room, Qt::AlignRight | Qt::AlignVCenter, note);
        }

        if (index.data(CURRENT_ROLE).toBool()) {
            QPointF corner(room.right() + 1 - CHECK_SIZE, rect.center().y() - CHECK_SIZE / 2.0);
            QPointF check[] = { corner + QPointF(2.4, 6.3), corner + QPointF(4.8, 8.7), corner + QPointF(9.6, 3.5) };
            painter->setPen(QPen(token(STLXTHEME_TEXT_DIM), CHECK_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->drawPolyline(check, 3);
        }

        painter->restore();
    }

private:
    const QString* m_filter;
};

} // namespace

picker::picker(QWidget* parent)
    : QWidget(parent), m_prefix(new QLabel), m_query(new QLineEdit), m_help_row(new QWidget(this)), m_help(new QLabel),
      m_aside(new QLabel), m_list(new QListWidget(this)) {
    setFixedWidth(PICKER_WIDTH);
    auto* row = new query_row(this);
    row->setFixedHeight(FIELD_HEIGHT);

    m_prefix->setParent(row);
    m_prefix->setFont(mono_font(STLXTHEME_BODY_SIZE));
    m_prefix->setStyleSheet(QStringLiteral("color: %1").arg(token(STLXTHEME_TEXT_DIM).name()));

    m_query->setParent(row);
    m_query->setFrame(false);
    m_query->setFont(mono_font(STLXTHEME_BODY_SIZE));
    m_query->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; color: %1; "
                                          "placeholder-text-color: %2; }")
                               .arg(token(STLXTHEME_TEXT).name(), token(STLXTHEME_TEXT_DISABLED).name()));

    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(FIELD_PADDING_X, 0, FIELD_PADDING_X, 0);
    row_layout->setSpacing(PREFIX_GAP);
    row_layout->addWidget(m_prefix);
    row_layout->addWidget(m_query, 1);

    m_help_row->setFixedHeight(HELP_HEIGHT);
    for (QLabel* label : { m_help, m_aside }) {
        label->setParent(m_help_row);
        label->setFont(ui_font(STLXTHEME_CAPTION_SIZE));
        label->setStyleSheet(QStringLiteral("color: %1").arg(token(STLXTHEME_TEXT_DIM).name()));
        label->setTextFormat(Qt::RichText);
    }

    auto* help_layout = new QHBoxLayout(m_help_row);
    help_layout->setContentsMargins(FIELD_PADDING_X, 0, FIELD_PADDING_X, 0);
    help_layout->addWidget(m_help, 1);
    help_layout->addWidget(m_aside);

    // The list never takes the focus, so the query keeps the keys and a click in the list cancels nothing
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setContentsMargins(0, LIST_TOP_GAP, 0, 0);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->viewport()->setAutoFillBackground(false);
    m_list->setItemDelegate(new row_delegate(&m_filter, m_list));
    m_list->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(PANEL_PADDING, PANEL_PADDING, PANEL_PADDING, 0);
    layout->setSpacing(0);
    layout->addWidget(row);
    layout->addWidget(m_list);
    layout->addWidget(m_help_row);

    connect(m_query, &QLineEdit::textEdited, this, [this](const QString& query) {
        if (on_changed) {
            on_changed(query);
        } else if (!m_list->isHidden()) {
            filter_rows(query);
        }
    });

    m_list->setMouseTracking(true);
    connect(m_list, &QListWidget::itemEntered, this, [this](QListWidgetItem* item) {
        if (item->flags() & Qt::ItemIsSelectable) {
            m_list->setCurrentItem(item);
        }
    });

    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        m_list->setCurrentItem(item);
        finish(true);
    });

    m_query->installEventFilter(this);
    hide();
}

void picker::open(const QString& prefix, const QString& help, const QString& aside) {
    on_prefix_erased = nullptr;
    m_rows.clear();
    m_list->hide();
    m_help_row->show();
    layout()->setContentsMargins(PANEL_PADDING, PANEL_PADDING, PANEL_PADDING, 0);

    m_prefix->setText(prefix);
    m_prefix->show();
    m_query->setPlaceholderText(QString());
    m_aside->setText(aside);
    set_help(help);
    present(QString());
}

void picker::open_list(const QString& placeholder, const QList<picker_row>& rows, const QString& query,
                       const QString& help, const QString& aside) {
    on_prefix_erased = nullptr;
    m_rows = rows;
    m_help_row->setVisible(!help.isEmpty());
    m_list->show();
    layout()->setContentsMargins(PANEL_PADDING, PANEL_PADDING, PANEL_PADDING, help.isEmpty() ? PANEL_PADDING : 0);

    m_prefix->hide();
    m_query->setPlaceholderText(placeholder);
    m_aside->setText(aside);
    set_help(help);
    present(query);
    if (on_changed) {
        on_changed(query);
    } else {
        filter_rows(query);
    }
}

void picker::set_rows(const QList<picker_row>& rows) {
    m_rows = rows;
    m_filter.clear();

    QList<int> indices;
    for (int index = 0; index < m_rows.size(); index++) {
        indices.append(index);
    }

    show_rows(indices, true);
}

void picker::present(const QString& query) {
    m_query->setText(query);
    adjustSize();

    move((parentWidget()->width() - width()) / 2, PICKER_TOP);
    raise();
    show();
    m_query->setFocus();
}

void picker::set_help(const QString& help) {
    m_help->setText(help);
}

QString picker::emphasized(const QString& text) {
    return QStringLiteral("<span style=\"color: %1; font-weight: 500\">%2</span>")
        .arg(token(STLXTHEME_TEXT_SECONDARY).name(), text.toHtmlEscaped());
}

QString picker::code(const QString& text) {
    QString family = QString::fromLatin1(STLXTHEME_MONO_FONT_FAMILY);
    return QStringLiteral("<span style=\"color: %1; font-family: '%2'\">%3</span>")
        .arg(token(STLXTHEME_TEXT_SECONDARY).name(), family, text.toHtmlEscaped());
}

// Typing drops the section headings, since a list of matches reads best as one run
void picker::filter_rows(const QString& query) {
    m_filter = query;
    QList<int> indices;
    for (int index = 0; index < m_rows.size(); index++) {
        if (m_rows.at(index).label.contains(query, Qt::CaseInsensitive)) {
            indices.append(index);
        }
    }

    show_rows(indices, query.isEmpty());
}

// The first row is lit, the most recent choice before anything is typed and the best match after, and the current
// choice keeps its check
void picker::show_rows(const QList<int>& indices, bool has_headings) {
    m_list->clear();

    QString section;
    int lit = -1;
    for (int index : indices) {
        const picker_row& row = m_rows.at(index);
        if (has_headings && !row.section.isEmpty() && row.section != section) {
            auto* heading = new QListWidgetItem(row.section, m_list);
            heading->setData(HEADING_ROLE, true);
            heading->setFlags(Qt::NoItemFlags);
            section = row.section;
        }

        auto* item = new QListWidgetItem(row.label, m_list);
        item->setData(ROW_ROLE, index);
        item->setData(CURRENT_ROLE, row.is_current);

        item->setData(DETAIL_ROLE, row.detail);
        item->setData(KEYS_ROLE, row.keys);
        item->setData(MARKS_ROLE, QVariant::fromValue(row.marks));
        item->setData(NOTE_ROLE, row.note);

        if (lit < 0) {
            lit = m_list->count() - 1;
        }
    }

    int height = 0;
    for (int index = 0; index < m_list->count() && index < VISIBLE_ROWS; index++) {
        height += m_list->sizeHintForRow(index);
    }

    m_list->setFixedHeight(height + LIST_TOP_GAP);
    m_list->setCurrentRow(lit);
    if (lit >= 0) {
        m_list->scrollToItem(m_list->item(lit));
    }

    adjustSize();
}

// Moves the lit row past any heading, wrapping at either end
void picker::step(int delta) {
    QList<int> rows;
    for (int index = 0; index < m_list->count(); index++) {
        if (m_list->item(index)->flags() & Qt::ItemIsSelectable) {
            rows.append(index);
        }
    }

    if (rows.isEmpty()) {
        return;
    }

    qsizetype at = rows.indexOf(m_list->currentRow());
    int next = rows.at(at < 0 ? 0 : (at + delta + rows.size()) % rows.size());
    m_list->setCurrentRow(next);
    m_list->scrollToItem(m_list->item(next));
}

void picker::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QRectF edge = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
    painter.setPen(QPen(token(STLXTHEME_RIM), 1));
    painter.setBrush(token(STLXTHEME_MENU_BG));
    painter.drawRoundedRect(edge, STLXTHEME_MENU_RADIUS - HALF_PIXEL, STLXTHEME_MENU_RADIUS - HALF_PIXEL);
}

// Enter accepts and Esc cancels, Up and Down walk the list, and the focus leaving cancels too, as a click back in
// the text does
bool picker::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_query && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Escape) {
            finish(key->key() != Qt::Key_Escape);
            return true;
        }

        if (!m_list->isHidden() && (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
            step(key->key() == Qt::Key_Up ? -1 : 1);
            return true;
        }

        if (key->key() == Qt::Key_Backspace && m_query->text().isEmpty() && on_prefix_erased) {
            on_prefix_erased();
            return true;
        }
    }

    if (watched == m_query && event->type() == QEvent::FocusOut && isVisible()) {
        finish(false);
    }

    return QWidget::eventFilter(watched, event);
}

// Hidden first, so the focus it gives back finishes nothing twice. A list with nothing lit chooses nothing
void picker::finish(bool is_accepted) {
    hide();
    if (!is_accepted) {
        if (on_cancelled) {
            on_cancelled();
        }

        return;
    }

    if (m_list->isHidden()) {
        if (on_accepted) {
            on_accepted(m_query->text());
        }

        return;
    }

    QListWidgetItem* item = m_list->currentItem();
    if (item && on_chosen) {
        on_chosen(item->data(ROW_ROLE).toInt());
    } else if (on_cancelled) {
        on_cancelled();
    }
}
