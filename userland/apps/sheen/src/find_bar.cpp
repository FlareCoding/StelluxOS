#include "find_bar.h"
#include "chrome.h"
#include "field_frame.h"
#include "keycaps.h"
#include "kiln.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <ScintillaEditBase.h>

#include <regex>

static constexpr int FIND_ROW_HEIGHT = 42;
static constexpr int REPLACE_ROW_HEIGHT = 36;
static constexpr int BAR_PADDING_LEFT = 4;
static constexpr int BAR_PADDING_RIGHT = 8;
static constexpr int ROW_GAP = 4;
static constexpr int FIELD_WIDTH = 316;
static constexpr int CHIP_HEIGHT = 20;
static constexpr int CHIP_PADDING_X = 5;
static constexpr int ICON_BUTTON_SIZE = 28;
static constexpr int TWISTY_WIDTH = 24;
static constexpr int COUNT_MIN_WIDTH = 84;
static constexpr int COUNT_PADDING_X = 8;
static constexpr int CLOSE_GAP = 8;
static constexpr int BUTTON_GAP = 8;
static constexpr int HINT_GAP = 12;
static constexpr int HOVER_INSET = 2;
static constexpr qreal GLYPH_STROKE = 1.5;
static constexpr qreal CHEVRON_HALF = 4;
static constexpr qreal CHEVRON_DEPTH = 2;
static constexpr qreal CROSS_HALF = 3.5;
static constexpr qreal HALF_PIXEL = 0.5;
static constexpr int MAX_COUNTED_MATCHES = 9999;
static constexpr int REPLACED_NOTE_MS = 3000;
static constexpr int HISTORY_LIMIT = 50;
static constexpr int FIND_INDICATOR = INDICATOR_CONTAINER + 2;
static constexpr int REGEX_FLAGS = SCFIND_REGEXP | SCFIND_CXX11REGEX;

namespace {

enum class glyph { PREVIOUS, NEXT, CLOSE, TWISTY };
enum class chip_face { LABEL, UNDERLINED, SELECTION };

/* A borderless button holding one glyph, lit on hover */
class icon_button : public QToolButton {
public:
    icon_button(glyph kind, const QString& tip, int width, QWidget* parent) : QToolButton(parent), m_glyph(kind) {
        setFixedSize(width, ICON_BUTTON_SIZE);
        setToolTip(tip);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void enterEvent(QEnterEvent* event) override {
        QToolButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent* event) override {
        QToolButton::leaveEvent(event);
        update();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (underMouse() && isEnabled()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(token(STLXTHEME_SURFACE_HOVER));
            painter.drawRoundedRect(QRectF(rect()).adjusted(HOVER_INSET, HOVER_INSET, -HOVER_INSET, -HOVER_INSET),
                                    STLXTHEME_CONTROL_RADIUS, STLXTHEME_CONTROL_RADIUS);
        }

        QColor color = token(isEnabled() ? STLXTHEME_TEXT_SECONDARY : STLXTHEME_TEXT_DISABLED);
        painter.setPen(QPen(color, GLYPH_STROKE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        QPointF center = QRectF(rect()).center();
        bool is_down = m_glyph == glyph::NEXT || (m_glyph == glyph::TWISTY && isChecked());
        if (m_glyph == glyph::CLOSE) {
            painter.drawLine(center + QPointF(-CROSS_HALF, -CROSS_HALF), center + QPointF(CROSS_HALF, CROSS_HALF));
            painter.drawLine(center + QPointF(-CROSS_HALF, CROSS_HALF), center + QPointF(CROSS_HALF, -CROSS_HALF));
        } else if (m_glyph == glyph::TWISTY && !isChecked()) {
            QPointF points[] = { center + QPointF(-CHEVRON_DEPTH, -CHEVRON_HALF), center + QPointF(CHEVRON_DEPTH, 0),
                                 center + QPointF(-CHEVRON_DEPTH, CHEVRON_HALF) };
            painter.drawPolyline(points, 3);
        } else {
            qreal tip = is_down ? CHEVRON_DEPTH : -CHEVRON_DEPTH;
            QPointF points[] = { center + QPointF(-CHEVRON_HALF, -tip), center + QPointF(0, tip),
                                 center + QPointF(CHEVRON_HALF, -tip) };
            painter.drawPolyline(points, 3);
        }
    }

private:
    glyph m_glyph;
};

/* A toggle inside a field: its label dim while off, lit on a chip while on */
class option_chip : public QToolButton {
public:
    option_chip(const QString& label, const QString& tip, chip_face face, QWidget* parent)
        : QToolButton(parent), m_label(label), m_face(face) {
        setCheckable(true);
        setToolTip(tip);
        setFocusPolicy(Qt::NoFocus);
        QFontMetrics metrics(mono_font(STLXTHEME_CAPTION_SIZE));
        setFixedSize(qMax(CHIP_HEIGHT + 2, metrics.horizontalAdvance(label) + 2 * CHIP_PADDING_X), CHIP_HEIGHT);
    }

protected:
    void enterEvent(QEnterEvent* event) override {
        QToolButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent* event) override {
        QToolButton::leaveEvent(event);
        update();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QRectF edge = QRectF(rect()).adjusted(HALF_PIXEL, HALF_PIXEL, -HALF_PIXEL, -HALF_PIXEL);
        if (isChecked()) {
            painter.setPen(QPen(token(STLXTHEME_LINE_ON), 1));
            painter.setBrush(token(STLXTHEME_SURFACE_ON));
            painter.drawRoundedRect(edge, STLXTHEME_CHIP_RADIUS, STLXTHEME_CHIP_RADIUS);
        }

        uint32_t tone = isChecked() ? STLXTHEME_TEXT : underMouse() ? STLXTHEME_TEXT_SECONDARY : STLXTHEME_TEXT_DIM;
        QColor color = token(tone);
        if (m_face == chip_face::SELECTION) {
            // Lines inside a bracket, the sign for searching inside the selection
            QPointF center = edge.center();
            painter.setPen(QPen(color, 1));
            painter.setBrush(Qt::NoBrush);

            painter.drawRect(QRectF(center.x() - 5.5, center.y() - 4.5, 11, 9));
            painter.drawLine(QPointF(center.x() - 3, center.y() - 1.5), QPointF(center.x() + 3, center.y() - 1.5));
            painter.drawLine(QPointF(center.x() - 3, center.y() + 1.5), QPointF(center.x() + 1, center.y() + 1.5));
            return;
        }

        QFont font = mono_font(STLXTHEME_CAPTION_SIZE);
        font.setUnderline(m_face == chip_face::UNDERLINED);
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(rect(), Qt::AlignCenter, m_label);
    }

private:
    QString m_label;
    chip_face m_face;
};

} // namespace

static QString text_in(ScintillaEditBase* view, sptr_t start, sptr_t end) {
    const char* text = reinterpret_cast<const char*>(view->send(SCI_GETRANGEPOINTER, start, end - start));
    return QString::fromUtf8(text, end - start);
}

// The word the caret touches, with where it starts
static QString word_at_caret(ScintillaEditBase* view, sptr_t* start) {
    sptr_t caret = view->send(SCI_GETCURRENTPOS);
    *start = view->send(SCI_WORDSTARTPOSITION, caret, 1);
    sptr_t end = view->send(SCI_WORDENDPOSITION, caret, 1);
    return end > *start ? text_in(view, *start, end) : QString();
}

// A replacement takes the shape of the text it replaces: all capitals, all small letters or a leading capital
static QString with_case_of(const QString& matched, const QString& replacement) {
    if (matched.toUpper() == matched.toLower()) {
        return replacement;
    }

    if (matched == matched.toUpper()) {
        return replacement.toUpper();
    }

    if (matched == matched.toLower()) {
        return replacement.toLower();
    }

    QString shaped = replacement.toLower();
    bool is_capitalized = matched.at(0).isUpper() && matched.mid(1) == matched.mid(1).toLower();
    if (!is_capitalized || shaped.isEmpty()) {
        return replacement;
    }

    shaped[0] = shaped[0].toUpper();
    return shaped;
}

static void prepare_indicator(ScintillaEditBase* view) {
    view->send(SCI_INDICSETSTYLE, FIND_INDICATOR, INDIC_ROUNDBOX);
    view->send(SCI_INDICSETFORE, FIND_INDICATOR, scintilla_rgba(STLXTHEME_MATCH_MARK) & 0xFFFFFF);
    view->send(SCI_INDICSETALPHA, FIND_INDICATOR, qAlpha(STLXTHEME_MATCH_MARK));
    view->send(SCI_INDICSETOUTLINEALPHA, FIND_INDICATOR, 0);
    view->send(SCI_INDICSETUNDER, FIND_INDICATOR, 1);
}

find_bar::find_bar(QWidget* parent)
    : QWidget(parent), m_query(new QLineEdit), m_replacement(new QLineEdit), m_replaced_note(new QTimer(this)) {
    auto* find_row = new QWidget(this);
    find_row->setFixedHeight(FIND_ROW_HEIGHT);
    auto* query_field = new field_frame(m_query, find_row);
    m_query_field = query_field;
    query_field->setFixedWidth(FIELD_WIDTH);
    query_field->show_magnifier();

    m_case = new option_chip(QStringLiteral("Aa"), QStringLiteral("Match Case (Alt C)"), chip_face::LABEL, query_field);
    m_word = new option_chip(QStringLiteral("ab"), QStringLiteral("Whole Word (Alt W)"), chip_face::UNDERLINED,
                             query_field);
    m_regex = new option_chip(QStringLiteral(".*"), QStringLiteral("Regular Expression (Alt R)"), chip_face::LABEL,
                              query_field);
    m_in_selection = new option_chip(QString(), QStringLiteral("In Selection (Alt L)"), chip_face::SELECTION,
                                     query_field);

    for (QToolButton* option : { m_case, m_word, m_regex }) {
        query_field->add_option(option);
        connect(option, &QToolButton::toggled, this, [this] { search_from_anchor(); });
    }

    query_field->add_option(m_in_selection);
    connect(m_in_selection, &QToolButton::toggled, this, [this](bool is_checked) {
        if (is_checked && m_view) {
            m_scope_start = m_view->send(SCI_GETSELECTIONSTART);
            m_scope_end = m_view->send(SCI_GETSELECTIONEND);
            m_anchor = m_scope_start;
        }

        search_from_anchor();
    });

    m_count = new QLabel(find_row);
    m_count->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
    m_count->setMinimumWidth(COUNT_MIN_WIDTH);
    m_count->setContentsMargins(COUNT_PADDING_X, 0, COUNT_PADDING_X, 0);

    m_twisty = new icon_button(glyph::TWISTY, QStringLiteral("Replace (Ctrl H)"), TWISTY_WIDTH, find_row);
    m_twisty->setCheckable(true);
    auto* previous = new icon_button(glyph::PREVIOUS, QStringLiteral("Previous (Shift Enter)"), ICON_BUTTON_SIZE,
                                     find_row);
    auto* next = new icon_button(glyph::NEXT, QStringLiteral("Next (Enter)"), ICON_BUTTON_SIZE, find_row);
    auto* close = new icon_button(glyph::CLOSE, QStringLiteral("Close (Esc)"), ICON_BUTTON_SIZE, find_row);

    auto* find_layout = new QHBoxLayout(find_row);
    find_layout->setContentsMargins(0, 0, 0, 0);
    find_layout->setSpacing(ROW_GAP);

    find_layout->addWidget(m_twisty);
    find_layout->addWidget(query_field);
    find_layout->addWidget(m_count);
    find_layout->addWidget(previous);
    find_layout->addWidget(next);

    find_layout->addSpacing(CLOSE_GAP);
    find_layout->addWidget(close);
    find_layout->addStretch(1);

    m_replace_row = new QWidget(this);
    m_replace_row->setFixedHeight(REPLACE_ROW_HEIGHT);
    auto* replace_field = new field_frame(m_replacement, m_replace_row);
    m_replace_field = replace_field;
    replace_field->setFixedWidth(FIELD_WIDTH);

    m_preserve_case = new option_chip(QStringLiteral("AB"), QStringLiteral("Preserve Case (Alt P)"), chip_face::LABEL,
                                      replace_field);
    replace_field->add_option(m_preserve_case);

    auto* replace = new QPushButton(QStringLiteral("Replace"), m_replace_row);
    auto* replace_all_button = new QPushButton(QStringLiteral("Replace All"), m_replace_row);

    auto* hint = new keycap_hint({ QStringLiteral("Ctrl"), QStringLiteral("Alt"), QStringLiteral("Enter") },
                                 QStringLiteral("replaces all"), m_replace_row);

    auto* replace_layout = new QHBoxLayout(m_replace_row);
    replace_layout->setContentsMargins(0, 0, 0, 0);
    replace_layout->setSpacing(ROW_GAP);

    replace_layout->addSpacing(TWISTY_WIDTH);
    replace_layout->addWidget(replace_field);
    replace_layout->addSpacing(BUTTON_GAP);
    replace_layout->addWidget(replace);
    replace_layout->addWidget(replace_all_button);

    replace_layout->addSpacing(HINT_GAP);
    replace_layout->addWidget(hint);
    replace_layout->addStretch(1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(BAR_PADDING_LEFT, 0, BAR_PADDING_RIGHT, 0);
    layout->setSpacing(0);
    layout->addWidget(find_row);
    layout->addWidget(m_replace_row);

    for (QPushButton* button : { replace, replace_all_button }) {
        button->setFocusPolicy(Qt::NoFocus);
    }

    connect(replace, &QPushButton::clicked, this, [this] { replace_current(); });
    connect(replace_all_button, &QPushButton::clicked, this, [this] { replace_all(); });
    connect(previous, &QToolButton::clicked, this, [this] { step(false); });
    connect(next, &QToolButton::clicked, this, [this] { step(true); });
    connect(close, &QToolButton::clicked, this, [this] { close_bar(); });
    connect(m_twisty, &QToolButton::clicked, this, [this](bool is_checked) {
        set_replace_shown(is_checked);
        (is_checked ? m_replacement : m_query)->setFocus();
    });

    connect(m_query, &QLineEdit::textEdited, this, [this] {
        m_history_index = static_cast<int>(m_history.size());
        search_from_anchor();
    });

    m_query->installEventFilter(this);
    m_replacement->installEventFilter(this);

    m_replaced_note->setSingleShot(true);
    m_replaced_note->setInterval(REPLACED_NOTE_MS);
    connect(m_replaced_note, &QTimer::timeout, this, [this] { refresh_matches(); });

    set_replace_shown(false);
    hide();
}

// A new tab drops the old one's range and highlights, and the query runs again there
void find_bar::set_view(ScintillaEditBase* view) {
    if (view == m_view) {
        return;
    }

    clear_matches();
    m_view = view;
    QSignalBlocker blocker(m_in_selection);
    m_in_selection->setChecked(false);
    if (m_view) {
        prepare_indicator(m_view);
        m_anchor = m_view->send(SCI_GETSELECTIONSTART);
    }

    if (isVisible()) {
        refresh_matches();
    }
}

void find_bar::open(bool with_replace) {
    if (!m_view) {
        return;
    }

    // With the bar open the query stays as typed, so a regex query keeps its groups for a replacement
    bool is_reopened = isVisible();
    if (!is_reopened) {
        seed_query();
    }

    set_replace_shown(with_replace || (is_reopened && m_twisty->isChecked()));
    show();

    QLineEdit* field = with_replace ? m_replacement : m_query;
    field->setFocus();
    field->selectAll();
    if (!is_reopened) {
        search_from_anchor();
    }
}

void find_bar::seed_query() {
    sptr_t start = m_view->send(SCI_GETSELECTIONSTART);
    sptr_t end = m_view->send(SCI_GETSELECTIONEND);
    bool is_several_lines = m_view->send(SCI_LINEFROMPOSITION, start) != m_view->send(SCI_LINEFROMPOSITION, end);
    if (is_several_lines) {
        m_scope_start = start;
        m_scope_end = end;
    } else {
        QString seed = start != end ? text_in(m_view, start, end) : word_at_caret(m_view, &start);
        if (!seed.isEmpty()) {
            m_query->setText(seed);
        }
    }

    {
        QSignalBlocker blocker(m_in_selection);
        m_in_selection->setChecked(is_several_lines);
    }

    m_anchor = start;
}

void find_bar::close_bar() {
    remember_query();
    clear_matches();
    hide();
    if (on_closed) {
        on_closed();
    }
}

bool find_bar::step(bool is_forward) {
    if (!m_view || m_query->text().isEmpty() || !pattern_problem().isEmpty()) {
        return false;
    }

    sptr_t selection_start = m_view->send(SCI_GETSELECTIONSTART);
    sptr_t selection_end = m_view->send(SCI_GETSELECTIONEND);
    sptr_t start = 0;
    sptr_t end = 0;
    bool is_found = find_wrapping(is_forward, is_forward ? selection_end : selection_start, &start, &end);

    // An empty match where the caret already sits would be found again, so the search moves one past it
    if (is_found && start == end && start == selection_start && selection_start == selection_end) {
        sptr_t from = is_forward ? m_view->send(SCI_POSITIONAFTER, end) : m_view->send(SCI_POSITIONBEFORE, start);
        is_found = find_wrapping(is_forward, from, &start, &end);
    }

    if (is_found) {
        select_match(start, end);
        m_anchor = start;
    }

    refresh_matches();
    remember_query();
    return true;
}

void find_bar::find_word_at_caret() {
    if (!m_view) {
        return;
    }

    sptr_t start = m_view->send(SCI_GETSELECTIONSTART);
    sptr_t end = m_view->send(SCI_GETSELECTIONEND);
    QString word = start != end ? text_in(m_view, start, end) : word_at_caret(m_view, &start);
    if (word.isEmpty()) {
        return;
    }

    m_query->setText(word);
    m_view->send(SCI_SETSEL, start, start + word.toUtf8().size());
    step(true);
}

void find_bar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(QRect(0, height() - 1, width(), 1), token(STLXTHEME_LINE_SUBTLE));
}

// Keys while either field has focus: Enter steps or replaces, Alt toggles an option, Esc closes
bool find_bar::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::KeyPress || (watched != m_query && watched != m_replacement)) {
        return QWidget::eventFilter(watched, event);
    }

    auto* key = static_cast<QKeyEvent*>(event);
    Qt::KeyboardModifiers modifiers = key->modifiers() & ~Qt::KeypadModifier;
    bool is_enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
    if (is_enter && modifiers == (Qt::ControlModifier | Qt::AltModifier)) {
        replace_all();
        return true;
    }

    if (is_enter && modifiers == Qt::AltModifier) {
        select_all_matches();
        return true;
    }

    if (is_enter && watched == m_replacement) {
        replace_current();
        return true;
    }

    if (is_enter) {
        step(!(modifiers & Qt::ShiftModifier));
        return true;
    }

    if (key->key() == Qt::Key_Escape) {
        close_bar();
        return true;
    }

    bool is_recall = watched == m_query && modifiers == Qt::NoModifier;
    if (is_recall && (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
        recall_query(key->key() == Qt::Key_Up ? -1 : 1);
        return true;
    }

    QToolButton* option = nullptr;
    if (modifiers == Qt::AltModifier) {
        option = key->key() == Qt::Key_C   ? m_case
               : key->key() == Qt::Key_W ? m_word
               : key->key() == Qt::Key_R ? m_regex
               : key->key() == Qt::Key_L ? m_in_selection
               : key->key() == Qt::Key_P ? m_preserve_case
                                         : nullptr;
    }

    if (option) {
        option->toggle();
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

int find_bar::search_flags() const {
    int flags = m_case->isChecked() ? SCFIND_MATCHCASE : 0;
    flags |= m_word->isChecked() ? SCFIND_WHOLEWORD : 0;
    flags |= m_regex->isChecked() ? REGEX_FLAGS : 0;
    return flags;
}

// The range searched, the selection In Selection took or else the whole text
void find_bar::scope(sptr_t* start, sptr_t* end) const {
    sptr_t length = m_view->send(SCI_GETLENGTH);
    bool is_scoped = m_in_selection->isChecked() && m_scope_end > m_scope_start;
    *start = is_scoped ? qMin(m_scope_start, length) : 0;
    *end = is_scoped ? qMin(m_scope_end, length) : length;
}

// Searches from `from` toward `to`, backward when `to` lies before `from`
bool find_bar::find_in(sptr_t from, sptr_t to, sptr_t* match_start, sptr_t* match_end) const {
    QByteArray query = m_query->text().toUtf8();
    m_view->send(SCI_SETSEARCHFLAGS, search_flags());
    m_view->send(SCI_SETTARGETRANGE, from, to);
    if (m_view->send(SCI_SEARCHINTARGET, query.size(), reinterpret_cast<sptr_t>(query.constData())) < 0) {
        return false;
    }

    *match_start = m_view->send(SCI_GETTARGETSTART);
    *match_end = m_view->send(SCI_GETTARGETEND);
    return true;
}

bool find_bar::find_wrapping(bool is_forward, sptr_t from, sptr_t* match_start, sptr_t* match_end) const {
    sptr_t start = 0;
    sptr_t end = 0;
    scope(&start, &end);
    from = qBound(start, from, end);
    if (is_forward) {
        return find_in(from, end, match_start, match_end) || find_in(start, from, match_start, match_end);
    }

    return find_in(from, start, match_start, match_end) || find_in(end, from, match_start, match_end);
}

// Scintilla runs C++11's ECMAScript flavor of regular expressions, so the same parser names what is wrong
QString find_bar::pattern_problem() const {
    if (!m_regex->isChecked() || m_query->text().isEmpty()) {
        return QString();
    }

    try {
        std::regex probe(m_query->text().toStdString(), std::regex::ECMAScript);
    } catch (const std::regex_error& error) {
        switch (error.code()) {
            case std::regex_constants::error_paren:
                return QStringLiteral("Missing )");
            case std::regex_constants::error_brack:
                return QStringLiteral("Missing ]");
            case std::regex_constants::error_brace:
                return QStringLiteral("Missing }");
            case std::regex_constants::error_badrepeat:
                return QStringLiteral("Nothing to repeat");
            case std::regex_constants::error_escape:
                return QStringLiteral("Bad escape");
            default:
                return QStringLiteral("Bad pattern");
        }
    }

    return QString();
}

// Typing searches again from where the find began, so each letter refines the same place in the text
void find_bar::search_from_anchor() {
    if (!m_view) {
        return;
    }

    QString problem = pattern_problem();
    m_query_field->set_bad(!problem.isEmpty());
    sptr_t start = 0;
    sptr_t end = 0;
    if (!m_query->text().isEmpty() && problem.isEmpty() && find_wrapping(true, m_anchor, &start, &end)) {
        select_match(start, end);
    }

    refresh_matches();
}

// Marks every match up to a limit and counts them, naming which one the selection holds
void find_bar::refresh_matches() {
    clear_matches();
    m_replaced_note->stop();
    if (!isVisible()) {
        return;
    }

    QString problem = pattern_problem();
    if (!m_view || m_query->text().isEmpty() || !problem.isEmpty()) {
        m_count->setStyleSheet(QStringLiteral("color: %1").arg(token(STLXTHEME_TEXT).name()));
        m_count->setText(problem);
        return;
    }

    sptr_t start = 0;
    sptr_t end = 0;
    scope(&start, &end);
    sptr_t selection_start = m_view->send(SCI_GETSELECTIONSTART);
    sptr_t selection_end = m_view->send(SCI_GETSELECTIONEND);

    int count = 0;
    int current = 0;
    sptr_t from = start;
    sptr_t match_start = 0;
    sptr_t match_end = 0;

    m_view->send(SCI_SETINDICATORCURRENT, FIND_INDICATOR);
    while (count < MAX_COUNTED_MATCHES && from <= end && find_in(from, end, &match_start, &match_end)) {
        count++;
        current = match_start == selection_start && match_end == selection_end ? count : current;
        m_view->send(SCI_INDICATORFILLRANGE, match_start, match_end - match_start);
        from = match_end > match_start ? match_end : m_view->send(SCI_POSITIONAFTER, match_end);
        if (match_end >= end) {
            break;
        }
    }

    QString total = count < MAX_COUNTED_MATCHES ? QString::number(count) : QStringLiteral("%1+").arg(count);
    m_count->setStyleSheet(QStringLiteral("color: %1").arg(token(STLXTHEME_TEXT_SECONDARY).name()));
    m_count->setText(count == 0     ? QStringLiteral("No results")
                     : current > 0 ? QStringLiteral("%1 of %2").arg(current).arg(total)
                                   : QStringLiteral("%1 found").arg(total));
}

void find_bar::clear_matches() {
    if (m_view) {
        m_view->send(SCI_SETINDICATORCURRENT, FIND_INDICATOR);
        m_view->send(SCI_INDICATORCLEARRANGE, 0, m_view->send(SCI_GETLENGTH));
    }
}

void find_bar::select_match(sptr_t start, sptr_t end) {
    m_view->send(SCI_ENSUREVISIBLE, m_view->send(SCI_LINEFROMPOSITION, start));
    m_view->send(SCI_SETSEL, start, end);
}

// Every match becomes a selection of its own, and the text takes the focus to type over them
void find_bar::select_all_matches() {
    if (!m_view || m_query->text().isEmpty() || !pattern_problem().isEmpty()) {
        return;
    }

    sptr_t start = 0;
    sptr_t end = 0;
    scope(&start, &end);

    sptr_t from = start;
    sptr_t match_start = 0;
    sptr_t match_end = 0;
    int count = 0;
    while (count < MAX_COUNTED_MATCHES && from <= end && find_in(from, end, &match_start, &match_end)) {
        if (count == 0) {
            m_view->send(SCI_SETSELECTION, match_end, match_start);
        } else {
            m_view->send(SCI_ADDSELECTION, match_end, match_start);
        }

        count++;
        from = match_end > match_start ? match_end : m_view->send(SCI_POSITIONAFTER, match_end);
        if (match_end >= end) {
            break;
        }
    }

    if (count > 0) {
        remember_query();
        m_view->setFocus();
    }
}

// The target is the match just found, so a regular expression's groups fill \1 to \9
sptr_t find_bar::replace_target(sptr_t start, sptr_t end) {
    if (m_regex->isChecked()) {
        QByteArray pattern = m_replacement->text().toUtf8();
        return m_view->send(SCI_REPLACETARGETRE, pattern.size(), reinterpret_cast<sptr_t>(pattern.constData()));
    }

    QString text = m_replacement->text();
    if (m_preserve_case->isChecked()) {
        text = with_case_of(text_in(m_view, start, end), text);
    }

    QByteArray bytes = text.toUtf8();
    return m_view->send(SCI_REPLACETARGET, bytes.size(), reinterpret_cast<sptr_t>(bytes.constData()));
}

// Replaces the selected match, when the selection is one, and moves on to the next
void find_bar::replace_current() {
    if (!m_view || m_query->text().isEmpty() || !pattern_problem().isEmpty()) {
        return;
    }

    sptr_t selection_start = m_view->send(SCI_GETSELECTIONSTART);
    sptr_t selection_end = m_view->send(SCI_GETSELECTIONEND);
    sptr_t start = 0;
    sptr_t end = 0;
    bool is_match = find_in(selection_start, selection_end, &start, &end) && start == selection_start
                    && end == selection_end;
    if (is_match) {
        sptr_t length = replace_target(start, end);
        m_view->send(SCI_SETSEL, start + length, start + length);
        m_scope_end += length - (end - start);
    }

    step(true);
}

// One undo step replaces every match in the range, and the count says how many for a moment
void find_bar::replace_all() {
    if (!m_view || m_query->text().isEmpty() || !pattern_problem().isEmpty()) {
        return;
    }

    sptr_t start = 0;
    sptr_t end = 0;
    scope(&start, &end);

    sptr_t from = start;
    sptr_t match_start = 0;
    sptr_t match_end = 0;
    int replaced = 0;

    m_view->send(SCI_BEGINUNDOACTION);
    while (from <= end && find_in(from, end, &match_start, &match_end)) {
        sptr_t length = replace_target(match_start, match_end);
        end += length - (match_end - match_start);
        from = match_start + length;
        from = match_end > match_start ? from : m_view->send(SCI_POSITIONAFTER, from);
        replaced++;
    }

    m_view->send(SCI_ENDUNDOACTION);
    m_scope_end = m_in_selection->isChecked() ? end : m_scope_end;
    remember_query();
    refresh_matches();
    m_count->setText(QStringLiteral("Replaced %1").arg(replaced));
    m_replaced_note->start();
}

void find_bar::set_replace_shown(bool is_shown) {
    m_replace_row->setVisible(is_shown);
    m_twisty->setChecked(is_shown);
    setFixedHeight(FIND_ROW_HEIGHT + (is_shown ? REPLACE_ROW_HEIGHT : 0));
}

void find_bar::remember_query() {
    QString query = m_query->text();
    if (!query.isEmpty() && (m_history.isEmpty() || m_history.last() != query)) {
        m_history.removeAll(query);
        m_history.append(query);
        while (m_history.size() > HISTORY_LIMIT) {
            m_history.removeFirst();
        }
    }

    m_history_index = static_cast<int>(m_history.size());
}

// Up walks back through this session's queries and Down forward, past the newest to an empty field
void find_bar::recall_query(int direction) {
    if (m_history.isEmpty()) {
        return;
    }

    m_history_index = qBound(0, m_history_index + direction, static_cast<int>(m_history.size()));
    m_query->setText(m_history_index < m_history.size() ? m_history.at(m_history_index) : QString());
    search_from_anchor();
}
