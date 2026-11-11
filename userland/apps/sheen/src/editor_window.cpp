#include "editor_window.h"
#include "banner.h"
#include "chrome.h"
#include "empty_state.h"
#include "file_loader.h"
#include "file_sheet.h"
#include "find_bar.h"
#include "keycaps.h"
#include "kiln.h"
#include "menu_style.h"
#include "palette.h"
#include "picker.h"
#include "placeholder.h"
#include "settings_page.h"
#include "tab_strip.h"
#include "tab_switcher.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <ScintillaEditBase.h>

#include <algorithm>

// Where the caret, selection and scroll stood before go to line, so Esc can put them back
struct view_place {
    sptr_t anchor;
    sptr_t caret;
    sptr_t first_line;
    sptr_t x_offset;
};

struct eol_choice {
    int mode;
    const char* label;
};

static constexpr int WINDOW_W = 1520;
static constexpr int WINDOW_H = 920;
static constexpr int WINDOW_MIN_W = 720;
static constexpr int WINDOW_MIN_H = 480;
static constexpr int CLOSED_TAB_LIMIT = 20;
static constexpr int RECENT_FILE_LIMIT = 20;
static constexpr int RECENT_COMMAND_LIMIT = 5;
static constexpr qsizetype JUMP_LIMIT = 50;

// Wide enough that each menu's chevron stands apart from its name, with room for the keycaps under the hairline
static constexpr int CASCADE_WIDTH = 272;
static constexpr int STATUS_NOTE_MS = 4000;

static constexpr int SWITCHER_GAP = 8;
static constexpr int GOTO_MARKER = 1;
static constexpr int MIN_ZOOM = -10;
static constexpr int MAX_ZOOM = 20;
static constexpr char BRACKETS[] = "()[]{}";
static constexpr char OPENING_BRACKETS[] = "([{";
static constexpr char CLOSING_BRACKETS[] = ")]}";

// Far enough back for any block a person selects inside, near enough to answer at once
static constexpr sptr_t BRACKET_SCAN_BYTES = 256 * 1024;

// A list item in Text mode: its indent, a bullet or a number with its delimiter, the spacing and the item's text
static const QRegularExpression LIST_ITEM(QStringLiteral("^(\\s*)(?:([-*+])|(\\d+)([.)]))(\\s+)(.*)$"));
static constexpr int PERMILLE = 1000;
static constexpr qint64 BYTES_PER_MB = 1024 * 1024;
static constexpr qint64 BYTES_PER_GB = 1024 * BYTES_PER_MB;

// Files from here on read on a worker thread, where reading at once would hold the window still for a moment
static constexpr qint64 ASYNC_LOAD_BYTES = 4 * BYTES_PER_MB;
static constexpr int INDENT_WIDTHS[] = { 2, 4, 8 };
static constexpr QChar BULLET(0x2022);
static const QString NOTE_SEPARATOR = QStringLiteral("   ");

static const QByteArray UTF_8 = QByteArrayLiteral("UTF-8");

static const eol_choice EOL_CHOICES[] = {
    { SC_EOL_LF, "LF" },
    { SC_EOL_CRLF, "CRLF" },
    { SC_EOL_CR, "CR" },
};

static const command TAB_COMMANDS[] = {
    command::TAB_1, command::TAB_2, command::TAB_3, command::TAB_4,
    command::TAB_5, command::TAB_6, command::TAB_7, command::TAB_8,
};

// Commands that act on the current document, which an empty window has none of
static const command DOCUMENT_COMMANDS[] = {
    command::SAVE, command::SAVE_AS, command::REVERT, command::REOPEN_WITH_ENCODING, command::SAVE_WITH_ENCODING,
    command::CLOSE_ALL_TABS, command::UNDO, command::REDO, command::CUT, command::COPY, command::PASTE,
    command::DELETE_LINE, command::DUPLICATE, command::MOVE_LINES_UP, command::MOVE_LINES_DOWN, command::JOIN_LINES,
    command::INSERT_LINE_BELOW, command::INSERT_LINE_ABOVE, command::INDENT, command::OUTDENT, command::TRANSFORM,
    command::UPPER_CASE, command::LOWER_CASE, command::TITLE_CASE, command::SORT_LINES,
    command::TRIM_TRAILING_WHITESPACE, command::SELECT_ALL, command::SELECT_INSIDE_BRACKETS, command::ADD_CARET_ABOVE,
    command::ADD_CARET_BELOW, command::CARETS_AT_LINE_ENDS, command::UNDO_SELECTION, command::SELECT_LINE, command::ADD_NEXT_MATCH,
    command::SELECT_ALL_MATCHES, command::FIND, command::REPLACE, command::FIND_NEXT, command::FIND_PREVIOUS,
    command::FIND_WORD, command::GO_TO_LINE, command::CODE_MODE, command::TEXT_MODE, command::LANGUAGE, command::WORD_WRAP,
    command::LINE_NUMBERS, command::INDENT_GUIDES, command::COLUMN_EDGE, command::WHITESPACE, command::MATCHING_BRACKET,
};

// The commands that change the selection, which Undo Selection steps back through
static const command SELECTION_COMMANDS[] = {
    command::SELECT_ALL, command::SELECT_LINE, command::SELECT_INSIDE_BRACKETS, command::ADD_NEXT_MATCH,
    command::SELECT_ALL_MATCHES, command::ADD_CARET_ABOVE, command::ADD_CARET_BELOW, command::CARETS_AT_LINE_ENDS,
};

static QString eol_label(int mode) {
    for (const eol_choice& choice : EOL_CHOICES) {
        if (choice.mode == mode) {
            return QString::fromLatin1(choice.label);
        }
    }

    return QString();
}

// UTF-16 and UTF-32 files carry a byte order mark so other programs read their byte order, UTF-8 files go without
static file_encoding encoding_for_save(const QByteArray& name) {
    return { name, name != UTF_8 && name.startsWith("UTF-") };
}

// The keys of an action's first shortcut, such as Ctrl and S, each to be drawn as one keycap
static QStringList action_keys(const QAction* action) {
    return shortcut_keys(action->shortcut().toString(QKeySequence::NativeText));
}

// The menu an action sits in names it in the palette, and the actions reached only by their keys move between tabs
static QString palette_category(const command_entry& entry) {
    if (entry.place != menu::NONE) {
        return QString::fromLatin1(MENU_TITLES[static_cast<int>(entry.place)]);
    }

    return entry.id == command::SETTINGS ? QStringLiteral("Settings") : QStringLiteral("Go");
}

// The empty state's glyph for each of its actions, as mock 09 draws them
static row_glyph empty_glyph(command id) {
    switch (id) {
        case command::OPEN_FILE:
            return row_glyph::FOLDER;
        case command::REOPEN_CLOSED_TAB:
            return row_glyph::CLOCK;
        case command::COMMAND_PALETTE:
            return row_glyph::TERMINAL;
        default:
            return row_glyph::DOC;
    }
}

// Just under the caret, where a menu opened from the keyboard appears
static QPoint below_caret(ScintillaEditBase* view) {
    sptr_t caret = view->send(SCI_GETCURRENTPOS);
    sptr_t line_height = view->send(SCI_TEXTHEIGHT, view->send(SCI_LINEFROMPOSITION, caret));
    QPoint point(static_cast<int>(view->send(SCI_POINTXFROMPOSITION, 0, caret)),
                 static_cast<int>(view->send(SCI_POINTYFROMPOSITION, 0, caret) + line_height));
    return view->viewport()->mapToGlobal(point);
}

// The status bar names sheen.conf's first problem and how many more it has
static QString problems_note(const QStringList& problems) {
    if (problems.size() <= 1) {
        return problems.value(0);
    }

    return QStringLiteral("%1 and %2 more").arg(problems.first()).arg(problems.size() - 1);
}

static QString lines_phrase(int count) {
    return count == 1 ? QStringLiteral("1 line") : QStringLiteral("%1 lines").arg(count);
}

// A character with its code point, so one that draws nothing can still be found
static QString character_phrase(char32_t code) {
    QString hex = QString::number(code, 16).toUpper().rightJustified(4, QLatin1Char('0'));
    return QStringLiteral("%1 (U+%2)").arg(QString::fromUcs4(&code, 1), hex);
}

// The lines a selection covers, so line commands act on whole lines
static void selected_lines(ScintillaEditBase* view, sptr_t* first, sptr_t* last) {
    sptr_t start = view->send(SCI_GETSELECTIONSTART);
    sptr_t end = view->send(SCI_GETSELECTIONEND);
    *first = view->send(SCI_LINEFROMPOSITION, start);
    *last = view->send(SCI_LINEFROMPOSITION, end);
    if (*last > *first && view->send(SCI_POSITIONFROMLINE, *last) == end) {
        (*last)--;
    }
}

static void indent_lines(ScintillaEditBase* view, int direction) {
    sptr_t first = 0;
    sptr_t last = 0;
    selected_lines(view, &first, &last);
    sptr_t width = view->send(SCI_GETINDENT);
    view->send(SCI_BEGINUNDOACTION);
    for (sptr_t line = first; line <= last; line++) {
        sptr_t indent = view->send(SCI_GETLINEINDENTATION, line);
        view->send(SCI_SETLINEINDENTATION, line, qMax<sptr_t>(0, indent + direction * width));
    }

    view->send(SCI_ENDUNDOACTION);
}

static void insert_line(ScintillaEditBase* view, bool is_above) {
    view->send(SCI_BEGINUNDOACTION);
    if (is_above) {
        view->send(SCI_HOME);
        view->send(SCI_NEWLINE);
        view->send(SCI_LINEUP);
    } else {
        view->send(SCI_LINEEND);
        view->send(SCI_NEWLINE);
    }

    view->send(SCI_ENDUNDOACTION);
}

// Selects the caret's line, and each repeat extends the selection by the next line
static void select_line(ScintillaEditBase* view) {
    sptr_t first = 0;
    sptr_t last = 0;
    selected_lines(view, &first, &last);

    sptr_t start = view->send(SCI_POSITIONFROMLINE, first);
    sptr_t end = view->send(SCI_POSITIONFROMLINE, last + 1);
    bool is_whole_lines = view->send(SCI_GETSELECTIONSTART) == start && view->send(SCI_GETSELECTIONEND) == end;
    if (is_whole_lines) {
        end = view->send(SCI_POSITIONFROMLINE, last + 2);
    }

    view->send(SCI_SETSEL, start, end < 0 ? view->send(SCI_GETLENGTH) : end);
}

// "42" or "42:15", with the column at the line's start when absent
static bool read_line_target(const QString& query, sptr_t* line, sptr_t* column) {
    QStringList parts = query.trimmed().split(QLatin1Char(':'));
    bool is_line = false;
    bool is_column = true;
    *line = parts.at(0).toLongLong(&is_line);
    *column = parts.size() > 1 && !parts.at(1).isEmpty() ? parts.at(1).toLongLong(&is_column) : 1;
    return is_line && is_column && parts.size() <= 2 && *line > 0 && *column > 0;
}

// Reads a go to line query back, such as Line 143, column 9 of 348 lines
static QString line_target_help(const QString& query, sptr_t line, sptr_t column, sptr_t lines) {
    QString target = picker::emphasized(QString::number(line));
    if (!query.trimmed().section(QLatin1Char(':'), 1).isEmpty()) {
        target += QStringLiteral(", column %1").arg(picker::emphasized(QString::number(column)));
    }

    if (line > lines) {
        return QStringLiteral("Line %1 is past the end of %2 lines").arg(target).arg(lines);
    }

    return QStringLiteral("Line %1 of %2 lines").arg(target).arg(lines);
}

// Centers a line, counted from one, and tints it
static void preview_line(ScintillaEditBase* view, sptr_t line) {
    view->send(SCI_MARKERDELETEALL, GOTO_MARKER);
    view->send(SCI_ENSUREVISIBLE, line - 1);
    sptr_t shown = view->send(SCI_VISIBLEFROMDOCLINE, line - 1);
    view->send(SCI_SETFIRSTVISIBLELINE, qMax<sptr_t>(0, shown - view->send(SCI_LINESONSCREEN) / 2));
    view->send(SCI_MARKERADD, line - 1, GOTO_MARKER);
}

// Folds or opens the block holding the caret from its header, leaving the caret on the header once its line hides
static void fold_at_caret(ScintillaEditBase* view, bool is_folding) {
    sptr_t line = view->send(SCI_LINEFROMPOSITION, view->send(SCI_GETCURRENTPOS));
    bool is_header = view->send(SCI_GETFOLDLEVEL, line) & SC_FOLDLEVELHEADERFLAG;
    sptr_t header = is_header ? line : view->send(SCI_GETFOLDPARENT, line);
    if (header < 0) {
        return;
    }

    view->send(SCI_FOLDLINE, header, is_folding ? SC_FOLDACTION_CONTRACT : SC_FOLDACTION_EXPAND);
    if (is_folding && header != line) {
        view->send(SCI_GOTOPOS, view->send(SCI_GETLINEENDPOSITION, header));
    }
}

static bool is_bracket_at(ScintillaEditBase* view, sptr_t position) {
    char character = static_cast<char>(view->send(SCI_GETCHARAT, position));
    return position >= 0 && character != 0 && QByteArrayView(BRACKETS).contains(character);
}

// Boxes the bracket at the caret, else the one before it, with its partner, or marks it alone when it has none
static void highlight_brackets(ScintillaEditBase* view, bool is_on) {
    sptr_t caret = view->send(SCI_GETCURRENTPOS);
    sptr_t at = !is_on                          ? -1
              : is_bracket_at(view, caret)      ? caret
              : is_bracket_at(view, caret - 1) ? caret - 1
                                                : -1;
    if (at < 0) {
        view->send(SCI_BRACEHIGHLIGHT, INVALID_POSITION, INVALID_POSITION);
        return;
    }

    sptr_t match = view->send(SCI_BRACEMATCH, at, 0);
    if (match < 0) {
        view->send(SCI_BRACEBADLIGHT, at);
    } else {
        view->send(SCI_BRACEHIGHLIGHT, at, match);
    }
}

static QByteArray text_between(ScintillaEditBase* view, sptr_t start, sptr_t end) {
    const char* text = reinterpret_cast<const char*>(view->send(SCI_GETRANGEPOINTER, start, end - start));
    return QByteArray(text, end - start);
}

static QByteArray line_bytes(ScintillaEditBase* view, sptr_t line, sptr_t end) {
    return text_between(view, view->send(SCI_POSITIONFROMLINE, line), end);
}

// Comments the selected lines at their shallowest indent, or uncomments them when each already starts with the
// comment, as one undo step. Blank lines stay as they are
static void toggle_line_comment(ScintillaEditBase* view, const QByteArray& token) {
    sptr_t first = 0;
    sptr_t last = 0;
    selected_lines(view, &first, &last);

    bool is_commented = true;
    sptr_t shallowest = -1;
    for (sptr_t line = first; line <= last; line++) {
        sptr_t start = view->send(SCI_GETLINEINDENTPOSITION, line);
        sptr_t end = view->send(SCI_GETLINEENDPOSITION, line);
        if (start == end) {
            continue;
        }

        sptr_t indent = view->send(SCI_GETLINEINDENTATION, line);
        shallowest = shallowest < 0 ? indent : qMin(shallowest, indent);
        is_commented = is_commented && text_between(view, start, end).startsWith(token);
    }

    if (token.isEmpty() || shallowest < 0) {
        return;
    }

    QByteArray comment = token + ' ';
    view->send(SCI_BEGINUNDOACTION);
    for (sptr_t line = last; line >= first; line--) {
        sptr_t start = view->send(SCI_GETLINEINDENTPOSITION, line);
        if (start == view->send(SCI_GETLINEENDPOSITION, line)) {
            continue;
        }

        if (is_commented) {
            bool has_space = view->send(SCI_GETCHARAT, start + token.size()) == ' ';
            view->send(SCI_DELETERANGE, start, token.size() + (has_space ? 1 : 0));
        } else {
            sptr_t at = view->send(SCI_FINDCOLUMN, line, shallowest);
            view->send(SCI_INSERTTEXT, at, reinterpret_cast<sptr_t>(comment.constData()));
        }
    }

    view->send(SCI_ENDUNDOACTION);
}

// Wraps the selection, or the caret's line without one, in a block comment, or unwraps it when it already is one
static void toggle_block_comment(ScintillaEditBase* view, const QByteArray& open, const QByteArray& close) {
    if (open.isEmpty()) {
        return;
    }

    sptr_t start = view->send(SCI_GETSELECTIONSTART);
    sptr_t end = view->send(SCI_GETSELECTIONEND);
    if (start == end) {
        sptr_t line = view->send(SCI_LINEFROMPOSITION, start);
        start = view->send(SCI_GETLINEINDENTPOSITION, line);
        end = view->send(SCI_GETLINEENDPOSITION, line);
    }

    QByteArray text = text_between(view, start, end);
    bool is_long_enough = text.size() >= open.size() + close.size();
    bool is_commented = is_long_enough && text.startsWith(open) && text.endsWith(close);
    view->send(SCI_BEGINUNDOACTION);
    if (is_commented) {
        view->send(SCI_DELETERANGE, end - close.size(), close.size());
        view->send(SCI_DELETERANGE, start, open.size());
    } else {
        view->send(SCI_INSERTTEXT, end, reinterpret_cast<sptr_t>(close.constData()));
        view->send(SCI_INSERTTEXT, start, reinterpret_cast<sptr_t>(open.constData()));
    }

    view->send(SCI_ENDUNDOACTION);
}

// A caret on the line past the outermost caret, at the first caret's column as far as that line reaches
static void add_caret(ScintillaEditBase* view, int direction) {
    sptr_t first_caret = view->send(SCI_GETSELECTIONNCARET, 0);
    sptr_t edge = view->send(SCI_LINEFROMPOSITION, first_caret);
    for (sptr_t index = 1; index < view->send(SCI_GETSELECTIONS); index++) {
        sptr_t line = view->send(SCI_LINEFROMPOSITION, view->send(SCI_GETSELECTIONNCARET, index));
        edge = direction < 0 ? qMin(edge, line) : qMax(edge, line);
    }

    sptr_t target = edge + direction;
    if (target < 0 || target >= view->send(SCI_GETLINECOUNT)) {
        return;
    }

    sptr_t at = view->send(SCI_FINDCOLUMN, target, view->send(SCI_GETCOLUMN, first_caret));
    view->send(SCI_ADDSELECTION, at, at);
    view->send(SCI_SCROLLCARET);
}

// A caret at the end of each line the selection covers, in place of the selection
static void carets_at_line_ends(ScintillaEditBase* view) {
    sptr_t first = 0;
    sptr_t last = 0;
    selected_lines(view, &first, &last);

    sptr_t end = view->send(SCI_GETLINEENDPOSITION, first);
    view->send(SCI_SETSELECTION, end, end);
    for (sptr_t line = first + 1; line <= last; line++) {
        end = view->send(SCI_GETLINEENDPOSITION, line);
        view->send(SCI_ADDSELECTION, end, end);
    }
}

// Selects inside the innermost brackets around the selection, and with just their inside selected, the brackets too
static void select_inside_brackets(ScintillaEditBase* view) {
    sptr_t start = view->send(SCI_GETSELECTIONSTART);
    sptr_t end = view->send(SCI_GETSELECTIONEND);
    for (sptr_t at = start - 1; at >= qMax<sptr_t>(0, start - BRACKET_SCAN_BYTES); at--) {
        bool is_opening = QByteArrayView(OPENING_BRACKETS).contains(static_cast<char>(view->send(SCI_GETCHARAT, at)));
        sptr_t match = is_opening ? view->send(SCI_BRACEMATCH, at, 0) : -1;
        if (match < end) {
            continue;
        }

        bool is_inside_selected = start == at + 1 && end == match;
        view->send(SCI_SETSEL, is_inside_selected ? at : at + 1, is_inside_selected ? match + 1 : match);
        return;
    }
}

static caret_place caret_place_of(document* doc) {
    sptr_t caret = doc->view()->send(SCI_GETCURRENTPOS);
    return { doc, doc->view()->send(SCI_LINEFROMPOSITION, caret), doc->view()->send(SCI_GETCOLUMN, caret) };
}

// The selected lines, or every line without a selection
static std::pair<sptr_t, sptr_t> line_span(ScintillaEditBase* view) {
    if (view->send(SCI_GETSELECTIONEMPTY)) {
        return { 0, view->send(SCI_GETLINECOUNT) - 1 };
    }

    sptr_t first = 0;
    sptr_t last = 0;
    selected_lines(view, &first, &last);
    return { first, last };
}

// Where a line's text ends, before the spaces and tabs trailing it
static sptr_t content_end(ScintillaEditBase* view, sptr_t line) {
    sptr_t line_start = view->send(SCI_POSITIONFROMLINE, line);
    sptr_t end = view->send(SCI_GETLINEENDPOSITION, line);
    while (end > line_start && QByteArrayView(" \t").contains(static_cast<char>(view->send(SCI_GETCHARAT, end - 1)))) {
        end--;
    }

    return end;
}

static QString line_end_text(ScintillaEditBase* view) {
    switch (view->send(SCI_GETEOLMODE)) {
        case SC_EOL_CRLF:
            return QStringLiteral("\r\n");
        case SC_EOL_CR:
            return QStringLiteral("\r");
        default:
            return QStringLiteral("\n");
    }
}

// Joins the selected lines, or the caret's line and the next, each break and the indent after it becoming one space
static void join_lines(ScintillaEditBase* view) {
    sptr_t first = view->send(SCI_LINEFROMPOSITION, view->send(SCI_GETSELECTIONSTART));
    sptr_t last = view->send(SCI_LINEFROMPOSITION, view->send(SCI_GETSELECTIONEND));
    last = qMin(qMax(last, first + 1), view->send(SCI_GETLINECOUNT) - 1);
    if (last <= first) {
        return;
    }

    sptr_t caret = 0;
    view->send(SCI_BEGINUNDOACTION);
    for (sptr_t line = last; line > first; line--) {
        sptr_t cut = content_end(view, line - 1);
        sptr_t text = view->send(SCI_GETLINEINDENTPOSITION, line);
        bool is_bare = cut == view->send(SCI_POSITIONFROMLINE, line - 1) || text == view->send(SCI_GETLINEENDPOSITION, line);
        const char* joint = is_bare ? "" : " ";

        view->send(SCI_SETTARGETRANGE, cut, text);
        view->send(SCI_REPLACETARGET, static_cast<uptr_t>(qstrlen(joint)), reinterpret_cast<sptr_t>(joint));
        caret = cut + static_cast<sptr_t>(qstrlen(joint));
    }

    view->send(SCI_ENDUNDOACTION);
    view->send(SCI_SETEMPTYSELECTION, caret);
}

// Each word's first letter capital and the rest small, in every selection
static void title_case(ScintillaEditBase* view) {
    view->send(SCI_BEGINUNDOACTION);
    for (sptr_t index = 0; index < view->send(SCI_GETSELECTIONS); index++) {
        sptr_t start = view->send(SCI_GETSELECTIONNSTART, index);
        sptr_t end = view->send(SCI_GETSELECTIONNEND, index);
        QString text = QString::fromUtf8(text_between(view, start, end)).toLower();
        bool is_word_start = true;
        for (QChar& letter : text) {
            if (is_word_start && letter.isLetter()) {
                letter = letter.toUpper();
            }

            is_word_start = !letter.isLetterOrNumber() && letter != QLatin1Char('\'');
        }

        QByteArray bytes = text.toUtf8();
        view->send(SCI_SETTARGETRANGE, start, end);
        view->send(SCI_REPLACETARGET, static_cast<uptr_t>(bytes.size()), reinterpret_cast<sptr_t>(bytes.constData()));
    }

    view->send(SCI_ENDUNDOACTION);
}

// Sorts the lines ignoring case first, as one undo step, and selects them. A last empty line stays last
static void sort_lines(ScintillaEditBase* view) {
    auto [first, last] = line_span(view);
    if (last > first && view->send(SCI_LINELENGTH, last) == 0) {
        last--;
    }

    sptr_t from = view->send(SCI_POSITIONFROMLINE, first);
    sptr_t to = view->send(SCI_GETLINEENDPOSITION, last);
    static const QRegularExpression line_end(QStringLiteral("\r\n|\r|\n"));
    QStringList lines = QString::fromUtf8(text_between(view, from, to)).split(line_end);
    std::stable_sort(lines.begin(), lines.end(), [](const QString& left, const QString& right) {
        int order = left.compare(right, Qt::CaseInsensitive);
        return order != 0 ? order < 0 : left < right;
    });

    QByteArray sorted = lines.join(line_end_text(view)).toUtf8();
    view->send(SCI_SETTARGETRANGE, from, to);
    view->send(SCI_REPLACETARGET, static_cast<uptr_t>(sorted.size()), reinterpret_cast<sptr_t>(sorted.constData()));
    view->send(SCI_SETSEL, from, from + sorted.size());
}

// Drops the spaces and tabs ending each line, as one undo step
static void trim_trailing_whitespace(ScintillaEditBase* view) {
    auto [first, last] = line_span(view);
    view->send(SCI_BEGINUNDOACTION);
    for (sptr_t line = first; line <= last; line++) {
        sptr_t end = view->send(SCI_GETLINEENDPOSITION, line);
        sptr_t cut = content_end(view, line);
        if (cut < end) {
            view->send(SCI_DELETERANGE, cut, end - cut);
        }
    }

    view->send(SCI_ENDUNDOACTION);
}

// The next item's marker after `item`, the same bullet or the next number, followed by the same spacing
static QString next_list_marker(const QRegularExpressionMatch& item) {
    QString bullet = item.captured(2);
    QString lead = bullet.isEmpty() ? QString::number(item.captured(3).toLongLong() + 1) + item.captured(4) : bullet;
    return lead + item.captured(5);
}

// Enter keeps the indent. Code adds a level after an opening bracket and Text continues a list, where Enter on an
// empty item ends the list instead. A closing bracket typed alone on its line outdents to its partner's line
static void continue_line(ScintillaEditBase* view, int character, bool is_code) {
    if (view->send(SCI_GETSELECTIONS) != 1) {
        return;
    }

    sptr_t caret = view->send(SCI_GETCURRENTPOS);
    sptr_t line = view->send(SCI_LINEFROMPOSITION, caret);
    char line_end = view->send(SCI_GETEOLMODE) == SC_EOL_CR ? '\r' : '\n';
    if (character == line_end && line > 0) {
        sptr_t previous = line - 1;
        QString text = QString::fromUtf8(line_bytes(view, previous, view->send(SCI_GETLINEENDPOSITION, previous)));
        sptr_t indent = view->send(SCI_GETLINEINDENTATION, previous);

        QString trimmed = text.trimmed();
        bool opens_block = !trimmed.isEmpty() && QByteArrayView(OPENING_BRACKETS).contains(trimmed.back().toLatin1());
        QRegularExpressionMatch item = LIST_ITEM.match(text);
        QString marker;

        if (is_code && opens_block) {
            sptr_t unit = view->send(SCI_GETINDENT);
            indent += unit > 0 ? unit : view->send(SCI_GETTABWIDTH);
        } else if (!is_code && item.hasMatch() && item.captured(6).isEmpty()) {
            sptr_t start = view->send(SCI_POSITIONFROMLINE, previous) + item.capturedLength(1);
            view->send(SCI_SETTARGETRANGE, start, caret);
            view->send(SCI_REPLACETARGET, 0, reinterpret_cast<sptr_t>(""));
            return;
        } else if (!is_code && item.hasMatch()) {
            marker = next_list_marker(item);
        }

        view->send(SCI_SETLINEINDENTATION, line, indent);
        sptr_t at = view->send(SCI_GETLINEINDENTPOSITION, line);
        QByteArray bytes = marker.toUtf8();
        view->send(SCI_INSERTTEXT, at, reinterpret_cast<sptr_t>(bytes.constData()));
        view->send(SCI_SETEMPTYSELECTION, at + bytes.size());
        return;
    }

    bool is_closing = QByteArrayView(CLOSING_BRACKETS).contains(static_cast<char>(character));
    if (!is_code || !is_closing || !line_bytes(view, line, caret - 1).trimmed().isEmpty()) {
        return;
    }

    // The bracket just typed is styled first, since a bracket matches only one of the same style
    view->send(SCI_COLOURISE, view->send(SCI_POSITIONFROMLINE, line), caret);
    sptr_t match = view->send(SCI_BRACEMATCH, caret - 1, 0);
    if (match >= 0) {
        sptr_t match_line = view->send(SCI_LINEFROMPOSITION, match);
        view->send(SCI_SETLINEINDENTATION, line, view->send(SCI_GETLINEINDENTATION, match_line));
        view->send(SCI_SETEMPTYSELECTION, view->send(SCI_GETLINEINDENTPOSITION, line) + 1);
    }
}

// Enter types its line end inside one undo action, so the indentation continue_line adds undoes with it
static bool type_line_end(ScintillaEditBase* view, const QKeyEvent* key) {
    bool is_enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
    Qt::KeyboardModifiers extra = key->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier);
    if (!is_enter || extra != Qt::NoModifier) {
        return false;
    }

    view->send(SCI_BEGINUNDOACTION);
    view->send(SCI_NEWLINE);
    view->send(SCI_ENDUNDOACTION);
    return true;
}

static void go_to_matching_bracket(ScintillaEditBase* view) {
    sptr_t caret = view->send(SCI_GETCURRENTPOS);
    sptr_t match = view->send(SCI_BRACEMATCH, caret, 0);
    if (match < 0 && caret > 0) {
        match = view->send(SCI_BRACEMATCH, caret - 1, 0);
    }

    if (match >= 0) {
        view->send(SCI_GOTOPOS, match);
    }
}

// Free memory as /dev/sysinfo/mem reports it, or 0 when it cannot be read
static qint64 free_memory() {
    QFile file(QStringLiteral("/dev/sysinfo/mem"));
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }

    qint64 page_size = 0;
    qint64 free_pages = 0;
    for (const QByteArray& line : file.readAll().split('\n')) {
        QList<QByteArray> field = line.simplified().split(' ');
        if (field.size() == 2 && field[0] == "page_size") {
            page_size = field[1].toLongLong();
        } else if (field.size() == 2 && field[0] == "free_pages") {
            free_pages = field[1].toLongLong();
        }
    }

    return page_size * free_pages;
}

// The nearest folder above `path` that differs from the folder as far above each other path, empty when none does
static QString telling_folder(const QString& path, const QStringList& others) {
    QStringList folders = QFileInfo(path).absolutePath().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype depth = 1; depth <= folders.size(); depth++) {
        const QString& folder = folders.at(folders.size() - depth);
        bool is_shared = false;
        for (const QString& other : others) {
            QStringList theirs = QFileInfo(other).absolutePath().split(QLatin1Char('/'), Qt::SkipEmptyParts);
            is_shared = is_shared || (depth <= theirs.size() && theirs.at(theirs.size() - depth) == folder);
        }

        if (!is_shared) {
            return folder;
        }
    }

    return QString();
}

// Gigabytes with one decimal from 1 GB on, whole megabytes below
static QString size_label(qint64 bytes) {
    if (bytes >= BYTES_PER_GB) {
        return QStringLiteral("%1 GB").arg(QString::number(static_cast<double>(bytes) / BYTES_PER_GB, 'f', 1));
    }

    return QStringLiteral("%1 MB").arg((bytes + BYTES_PER_MB / 2) / BYTES_PER_MB);
}

editor_window::editor_window(const sheen_settings& settings, const QStringList& problems)
    : m_settings(settings), m_tab_row(new tab_row(this)), m_banner(new banner(this)), m_stack(new QStackedWidget(this)),
      m_empty_state(new empty_state(this)), m_status(new status_bar(this)), m_menu_style(new menu_style),
      m_main_menu(new QMenu(this)), m_switcher(new tab_switcher(this)) {
    m_find_bar = new find_bar(this);
    m_find_bar->on_jump = [this] { record_jump(current_document()); };
    m_picker = new picker(m_stack);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    layout->addWidget(m_tab_row);
    layout->addWidget(m_banner);
    layout->addWidget(m_find_bar);
    layout->addWidget(m_stack, 1);
    layout->addWidget(m_status);
    setCentralWidget(central);

    m_empty_state->on_file_chosen = [this](const QString& path) { open_paths({ path }); };
    m_stack->addWidget(m_empty_state);

    create_commands();
    connect(tabs(), &QTabBar::currentChanged, this, [this] { show_current_document(); });
    connect(tabs(), &QTabBar::tabCloseRequested, this, [this](int index) { close_document(index); });

    tabs()->on_context_menu = [this](int index, const QPoint& anchor) { show_tab_menu(index, anchor); };
    m_tab_row->on_empty_double_click = [this] { new_document(); };
    connect(m_tab_row->tab_list(), &QAbstractButton::clicked, this, [this] { show_tab_list(); });
    m_status->on_item_clicked = [this](status_kind kind, const QPoint& anchor) { show_status_menu(kind, anchor); };
    m_banner->on_dismissed = [this] {
        if (document* doc = current_document()) {
            doc->accept_disk_state();
            doc->set_banner(std::nullopt);
            refresh_banner();
        }
    };

    // The menu bar has no rows of its own for the palette and Settings, so File holds them above Quit
    if (m_settings.has_menu_bar) {
        QMenu* file = m_menus.at(static_cast<int>(menu::FILE));
        QAction* quit = m_actions.value(command::QUIT);
        file->insertAction(quit, m_actions.value(command::COMMAND_PALETTE));
        file->insertAction(quit, m_actions.value(command::SETTINGS));
        file->insertSeparator(quit);
        for (QMenu* submenu : m_menus) {
            if (!submenu->isEmpty()) {
                menuBar()->addMenu(submenu);
            }
        }

        m_tab_row->menu()->hide();
    }

    m_settings_problems = problems;
    m_status_note = problems_note(problems);

    resize(QSize(WINDOW_W, WINDOW_H).boundedTo(screen()->availableGeometry().size()));
    setMinimumSize(WINDOW_MIN_W, WINDOW_MIN_H);
    refresh_commands();
}

// The closed tabs' kept documents go with the process, and the views release their own
editor_window::~editor_window() {
    delete m_menu_style;
}

void editor_window::open_paths(const QStringList& paths) {
    QString folder;
    for (const QString& path : paths) {
        if (QFileInfo(path).isDir()) {
            folder = QFileInfo(path).absoluteFilePath();
            continue;
        }

        open_path(path, QByteArray(), insertion_index());
    }

    if (m_documents.empty()) {
        new_document();
    }

    if (!folder.isEmpty() && !m_open_sheet) {
        QTimer::singleShot(0, this, [this, folder] { choose_files_to_open(folder); });
    }
}

// Raised before the files open, since the desktop drops an activation that arrives too long after the user's press
void editor_window::receive_paths(const QStringList& paths) {
    if (isMinimized()) {
        showNormal();
    }

    raise();
    activateWindow();
    open_paths(paths);
}

// Quitting asks once about every unsaved tab, and a sheet open in the window keeps it open
void editor_window::closeEvent(QCloseEvent* event) {
    if (m_open_sheet) {
        event->ignore();
        return;
    }

    QList<document*> docs;
    for (const std::unique_ptr<document>& doc : m_documents) {
        docs.append(doc.get());
    }

    if (!close_documents(docs, QStringLiteral("quitting"))) {
        event->ignore();
        return;
    }

    event->accept();
}

// Files are compared with their disk records whenever the window comes forward
void editor_window::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow() && !m_open_sheet) {
        for (const std::unique_ptr<document>& doc : m_documents) {
            check_disk(doc.get());
        }
    }
}

// Scintilla leaves Esc to the window, which gives back one caret, else closes the find bar, else dismisses a change
// banner
void editor_window::keyPressEvent(QKeyEvent* event) {
    document* doc = current_document();
    if (event->key() != Qt::Key_Escape || event->modifiers() != Qt::NoModifier || !doc) {
        QMainWindow::keyPressEvent(event);
        return;
    }

    if (doc->view()->send(SCI_GETSELECTIONS) > 1) {
        doc->view()->send(SCI_CANCEL);
    } else if (m_find_bar->isVisible()) {
        m_find_bar->close_bar();
    } else if (doc->banner() && doc->banner()->kind == banner_kind::CHANGE) {
        m_banner->on_dismissed();
    }
}

// Each action is the window's too, so its keys work for commands outside the menus
void editor_window::create_commands() {
    for (const char* title : MENU_TITLES) {
        auto* submenu = new QMenu(QString::fromLatin1(title), this);
        submenu->setStyle(m_menu_style);
        m_menus.append(submenu);
    }

    QMenu* last_submenu = nullptr;
    for (const command_entry& entry : COMMANDS) {
        command id = entry.id;
        auto* action = new QAction(QString::fromLatin1(entry.label), this);
        QList<QKeySequence> keys;
        if (entry.shortcut[0]) {
            keys.append(QKeySequence(QString::fromLatin1(entry.shortcut), QKeySequence::PortableText));
        }

        if (entry.alternate[0]) {
            keys.append(QKeySequence(QString::fromLatin1(entry.alternate), QKeySequence::PortableText));
        }

        action->setShortcuts(keys);
        action->setCheckable(entry.kind == command_kind::TOGGLE);
        if (entry.kind == command_kind::SUBMENU) {
            auto* submenu = new QMenu(this);
            submenu->setStyle(m_menu_style);
            connect(submenu, &QMenu::aboutToShow, this, [this, id, submenu] { fill_submenu(id, submenu); });
            action->setMenu(submenu);
            last_submenu = submenu;
        } else {
            connect(action, &QAction::triggered, this, [this, id] { run(id); });
            addAction(action);
        }

        m_actions.insert(id, action);
        if (entry.kind == command_kind::SUBMENU_ROW) {
            // A submenu given rows by the table keeps them rather than being filled as it opens
            disconnect(last_submenu, &QMenu::aboutToShow, this, nullptr);
            last_submenu->addAction(action);
            continue;
        }

        if (entry.place == menu::NONE) {
            continue;
        }

        QMenu* target = m_menus.at(static_cast<int>(entry.place));
        if (entry.starts_group && !target->isEmpty()) {
            target->addSeparator();
        }

        target->addAction(action);
    }

    auto* modes = new QActionGroup(this);
    modes->addAction(m_actions.value(command::CODE_MODE));
    modes->addAction(m_actions.value(command::TEXT_MODE));

    m_main_menu->setStyle(m_menu_style);
    m_main_menu->setMinimumWidth(CASCADE_WIDTH);
    for (QMenu* submenu : m_menus) {
        if (!submenu->isEmpty()) {
            m_main_menu->addMenu(submenu);
        }
    }

    m_main_menu->addSeparator();
    m_main_menu->addAction(m_actions.value(command::COMMAND_PALETTE));
    m_main_menu->addAction(m_actions.value(command::SETTINGS));

    connect(m_menus.at(static_cast<int>(menu::GO)), &QMenu::aboutToShow, this, [this] {
        QMenu* go = m_menus.at(static_cast<int>(menu::GO));
        for (QAction* action : go->actions()) {
            if (action->property("tab").isValid()) {
                go->removeAction(action);
                action->deleteLater();
            }
        }

        QAction* separator = go->addSeparator();
        separator->setProperty("tab", true);
        for (int index = 0; index < tabs()->count(); index++) {
            QString keys = index < static_cast<int>(std::size(TAB_COMMANDS)) ? QStringLiteral("\tAlt+%1").arg(index + 1)
                                                                             : QString();
            QAction* action = go->addAction(m_documents[index]->display_name() + keys,
                                            [this, index] { select_tab(index); });
            action->setProperty("tab", true);
            action->setCheckable(true);
            action->setChecked(index == tabs()->currentIndex());
        }
    });

    connect(m_tab_row->menu(), &QAbstractButton::clicked, this, [this] { show_menu(m_main_menu, false); });
    auto* open_menu = new QAction(this);
    open_menu->setShortcut(QKeySequence(Qt::Key_F10));
    connect(open_menu, &QAction::triggered, this, [this] { show_menu(m_main_menu, true); });
    addAction(open_menu);

    // Shift F10 and the Menu key open the text menu at the caret, as a right click does at the pointer
    auto* open_text_menu = new QAction(this);
    open_text_menu->setShortcuts({ QKeySequence(Qt::SHIFT | Qt::Key_F10), QKeySequence(Qt::Key_Menu) });
    connect(open_text_menu, &QAction::triggered, this, [this] {
        if (document* doc = current_document()) {
            show_text_menu(below_caret(doc->view()));
        }
    });

    addAction(open_text_menu);
    for (int index = 0; index < static_cast<int>(std::size(MENU_KEYS)); index++) {
        if (m_menus.at(index)->isEmpty()) {
            continue;
        }

        auto* action = new QAction(this);
        action->setShortcut(QKeySequence(Qt::ALT | static_cast<Qt::Key>(MENU_KEYS[index])));
        connect(action, &QAction::triggered, this, [this, index] { show_menu(m_menus.at(index), true); });
        addAction(action);
    }
}

void editor_window::fill_submenu(command id, QMenu* submenu) {
    submenu->clear();
    document* doc = current_document();
    switch (id) {
        case command::OPEN_RECENT:
            for (const QString& path : m_recent_files) {
                submenu->addAction(path, [this, path] { open_path(path, QByteArray(), insertion_index()); });
            }

            if (!m_recent_files.isEmpty()) {
                submenu->addSeparator();
                submenu->addAction(QStringLiteral("Clear"), [this] {
                    m_recent_files.clear();
                    m_recent_opened.clear();
                    if (m_documents.empty()) {
                        show_empty_state();
                    }
                });
            }

            break;
        case command::LANGUAGE: {
            QAction* plain = submenu->addAction(language_name(nullptr), [this] {
                current_document()->set_language(nullptr);
                refresh_commands();
                refresh_status();
            });
            plain->setCheckable(true);
            plain->setChecked(doc && !doc->lang());
            for (const language* lang : all_languages()) {
                QAction* action = submenu->addAction(language_name(lang), [this, lang] {
                    current_document()->set_language(lang);
                    refresh_commands();
                    refresh_status();
                });
                action->setCheckable(true);
                action->setChecked(doc && doc->lang() == lang);
            }

            break;
        }
        default:
            break;
    }

    if (submenu->isEmpty()) {
        submenu->addAction(QStringLiteral("Nothing yet"))->setEnabled(false);
    }
}

void editor_window::run(command id) {
    if (id == command::NEW_FILE) {
        new_document();
        return;
    }

    if (id == command::OPEN_FILE) {
        choose_files_to_open(sheet_folder());
        return;
    }

    if (id == command::REOPEN_CLOSED_TAB) {
        reopen_closed_tab();
        return;
    }

    if (id == command::QUIT) {
        close();
        return;
    }

    if (id == command::SETTINGS || id == command::KEYBOARD_SHORTCUTS) {
        open_settings(id == command::SETTINGS ? QString() : QStringLiteral("Shortcuts"));
        return;
    }

    if (id == command::COMMAND_PALETTE || id == command::GO_TO_FILE) {
        open_palette(id == command::COMMAND_PALETTE ? QStringLiteral("> ") : QStringLiteral("@ "));
        return;
    }

    if (id == command::CLOSE_TAB && m_documents.empty()) {
        close();
        return;
    }

    document* doc = current_document();
    if (!doc) {
        return;
    }

    ScintillaEditBase* view = doc->view();
    if (std::ranges::find(SELECTION_COMMANDS, id) != std::end(SELECTION_COMMANDS)) {
        doc->remember_selection();
    }

    bool is_checked = m_actions.value(id)->isChecked();
    int count = tabs()->count();
    int current = tabs()->currentIndex();
    switch (id) {
        case command::SAVE:
            save_document(doc);
            break;
        case command::SAVE_AS:
            save_document_as(doc);
            break;
        case command::SAVE_ALL: {
            QList<document*> unsaved;
            for (const std::unique_ptr<document>& open : m_documents) {
                if (open->is_modified()) {
                    unsaved.append(open.get());
                }
            }

            save_documents(unsaved, QString());
            break;
        }
        case command::REVERT:
            revert_document(doc);
            break;
        case command::REOPEN_WITH_ENCODING:
        case command::SAVE_WITH_ENCODING:
            pick_encoding(id);
            break;
        case command::CLOSE_TAB:
            close_document(current);
            break;
        case command::CLOSE_ALL_TABS: {
            QList<document*> docs;
            for (const std::unique_ptr<document>& open : m_documents) {
                docs.append(open.get());
            }

            close_documents(docs, QStringLiteral("closing"));
            break;
        }
        case command::UNDO:
            view->send(SCI_UNDO);
            break;
        case command::REDO:
            view->send(SCI_REDO);
            break;
        case command::CUT:
            view->send(SCI_CUTALLOWLINE);
            break;
        case command::COPY:
            view->send(SCI_COPYALLOWLINE);
            break;
        case command::PASTE:
            view->send(SCI_PASTE);
            break;
        case command::DELETE_LINE:
            view->send(SCI_LINEDELETE);
            break;
        case command::DUPLICATE:
            view->send(SCI_SELECTIONDUPLICATE);
            break;
        case command::MOVE_LINES_UP:
            view->send(SCI_MOVESELECTEDLINESUP);
            break;
        case command::MOVE_LINES_DOWN:
            view->send(SCI_MOVESELECTEDLINESDOWN);
            break;
        case command::INSERT_LINE_BELOW:
            insert_line(view, false);
            break;
        case command::INSERT_LINE_ABOVE:
            insert_line(view, true);
            break;
        case command::TOGGLE_COMMENT:
            toggle_line_comment(view, comment_syntax_of(doc->lang()).line);
            break;
        case command::TOGGLE_BLOCK_COMMENT: {
            comment_syntax comments = comment_syntax_of(doc->lang());
            toggle_block_comment(view, comments.block_open, comments.block_close);
            break;
        }
        case command::INDENT:
            indent_lines(view, 1);
            break;
        case command::OUTDENT:
            indent_lines(view, -1);
            break;
        case command::UPPER_CASE:
            view->send(SCI_UPPERCASE);
            break;
        case command::LOWER_CASE:
            view->send(SCI_LOWERCASE);
            break;
        case command::TITLE_CASE:
            title_case(view);
            break;
        case command::SORT_LINES:
            sort_lines(view);
            break;
        case command::TRIM_TRAILING_WHITESPACE:
            trim_trailing_whitespace(view);
            break;
        case command::JOIN_LINES:
            join_lines(view);
            break;
        case command::SELECT_ALL:
            view->send(SCI_SELECTALL);
            break;
        case command::SELECT_LINE:
            select_line(view);
            break;
        case command::SELECT_INSIDE_BRACKETS:
            select_inside_brackets(view);
            break;
        case command::ADD_CARET_ABOVE:
        case command::ADD_CARET_BELOW:
            add_caret(view, id == command::ADD_CARET_ABOVE ? -1 : 1);
            break;
        case command::CARETS_AT_LINE_ENDS:
            carets_at_line_ends(view);
            break;
        case command::UNDO_SELECTION:
            doc->undo_selection();
            break;
        case command::ADD_NEXT_MATCH:
            view->send(SCI_TARGETWHOLEDOCUMENT);
            view->send(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE);
            view->send(SCI_MULTIPLESELECTADDNEXT);
            break;
        case command::FIND:
        case command::REPLACE:
            m_find_bar->set_view(view);
            m_find_bar->open(id == command::REPLACE);
            break;
        case command::FIND_NEXT:
        case command::FIND_PREVIOUS:
            m_find_bar->set_view(view);
            if (!m_find_bar->step(id == command::FIND_NEXT)) {
                m_find_bar->open(false);
            }

            break;
        case command::FIND_WORD:
            m_find_bar->set_view(view);
            m_find_bar->find_word_at_caret();
            break;
        case command::GO_TO_LINE:
            go_to_line(false);
            break;
        case command::SELECT_ALL_MATCHES:
            view->send(SCI_TARGETWHOLEDOCUMENT);
            view->send(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE);
            view->send(SCI_MULTIPLESELECTADDEACH);
            break;
        case command::CODE_MODE:
        case command::TEXT_MODE:
            doc->set_mode(id == command::CODE_MODE ? edit_mode::CODE : edit_mode::TEXT, true);
            refresh_commands();
            refresh_status();
            break;
        case command::WORD_WRAP:
            doc->choose_wrap(is_checked);
            refresh_status();
            break;
        case command::LINE_NUMBERS:
            set_line_numbers_shown(view, is_checked);
            doc->fit_text_width();
            break;
        case command::INDENT_GUIDES:
            view->send(SCI_SETINDENTATIONGUIDES, is_checked ? SC_IV_LOOKBOTH : SC_IV_NONE);
            break;
        case command::COLUMN_EDGE:
            view->send(SCI_SETEDGEMODE, is_checked ? EDGE_LINE : EDGE_NONE);
            break;
        case command::WHITESPACE:
            view->send(SCI_SETVIEWWS, is_checked ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE);
            break;
        case command::FOLD:
        case command::UNFOLD:
            fold_at_caret(view, id == command::FOLD);
            break;
        case command::FOLD_ALL:
            view->send(SCI_FOLDALL, SC_FOLDACTION_CONTRACT | SC_FOLDACTION_CONTRACT_EVERY_LEVEL);
            break;
        case command::UNFOLD_ALL:
            view->send(SCI_FOLDALL, SC_FOLDACTION_EXPAND);
            break;
        case command::ZOOM_IN:
            set_zoom(m_zoom + 1);
            break;
        case command::ZOOM_OUT:
            set_zoom(m_zoom - 1);
            break;
        case command::ZOOM_RESET:
            set_zoom(0);
            break;
        case command::MATCHING_BRACKET:
            record_jump(doc);
            go_to_matching_bracket(view);
            break;
        case command::GO_BACK:
        case command::GO_FORWARD:
            step_jump(id == command::GO_BACK);
            break;
        case command::RECENT_TAB:
            step_recent_tab(1);
            break;
        case command::RECENT_TAB_BACK:
            step_recent_tab(-1);
            break;
        case command::NEXT_TAB:
            select_tab((current + 1) % count);
            break;
        case command::PREVIOUS_TAB:
            select_tab((current + count - 1) % count);
            break;
        case command::MOVE_TAB_LEFT:
        case command::MOVE_TAB_RIGHT: {
            int target = current + (id == command::MOVE_TAB_LEFT ? -1 : 1);
            if (target >= 0 && target < count) {
                std::swap(m_documents[current], m_documents[target]);
                tabs()->moveTab(current, target);
            }

            break;
        }
        case command::LAST_TAB:
            select_tab(count - 1);
            break;
        case command::ABOUT:
            show_about();
            break;
        default:
            for (int index = 0; index < static_cast<int>(std::size(TAB_COMMANDS)); index++) {
                if (TAB_COMMANDS[index] == id && index < count) {
                    select_tab(index);
                }
            }

            break;
    }
}

// Every command but the sheet's own stands disabled while a sheet runs, and document commands need a document
void editor_window::refresh_commands() {
    bool is_free = !m_open_sheet;
    document* doc = current_document();
    for (QAction* action : m_actions) {
        action->setEnabled(is_free);
    }

    // A file held behind its placeholder has no text yet to act on
    bool has_text = doc && !doc->placeholder();
    for (command id : DOCUMENT_COMMANDS) {
        m_actions.value(id)->setEnabled(is_free && has_text);
    }

    // A large file stays plain and unwrapped, since styling or wrapping all of it would stall the window
    if (doc && doc->is_large()) {
        for (command id : { command::CODE_MODE, command::TEXT_MODE, command::LANGUAGE, command::WORD_WRAP }) {
            m_actions.value(id)->setEnabled(false);
        }
    }

    // Only Code mode folds, and a large file has no lexer to find its blocks
    bool can_fold = has_text && doc->mode() == edit_mode::CODE && !doc->is_large();
    for (command id : { command::FOLD, command::UNFOLD, command::FOLD_ALL, command::UNFOLD_ALL }) {
        m_actions.value(id)->setEnabled(is_free && can_fold);
    }

    comment_syntax comments = comment_syntax_of(doc ? doc->lang() : nullptr);
    m_actions.value(command::TOGGLE_COMMENT)->setEnabled(is_free && has_text && !comments.line.isEmpty());
    m_actions.value(command::TOGGLE_BLOCK_COMMENT)->setEnabled(is_free && has_text && !comments.block_open.isEmpty());

    m_actions.value(command::REOPEN_CLOSED_TAB)->setEnabled(is_free && !m_closed_tabs.isEmpty());
    m_actions.value(command::REVERT)->setEnabled(is_free && doc && !doc->is_untitled() && doc->is_modified());
    if (doc && doc->is_page()) {
        m_actions.value(command::SAVE)->setEnabled(is_free);
    }

    m_actions.value(command::REOPEN_WITH_ENCODING)->setEnabled(is_free && has_text && !doc->is_untitled());
    m_tab_row->menu()->setEnabled(is_free);
    if (!doc) {
        return;
    }

    m_actions.value(command::CODE_MODE)->setChecked(doc->mode() == edit_mode::CODE);
    m_actions.value(command::TEXT_MODE)->setChecked(doc->mode() == edit_mode::TEXT);

    ScintillaEditBase* view = doc->view();
    m_actions.value(command::WORD_WRAP)->setChecked(view->send(SCI_GETWRAPMODE) != SC_WRAP_NONE);
    m_actions.value(command::LINE_NUMBERS)->setChecked(are_line_numbers_shown(view));
    m_actions.value(command::INDENT_GUIDES)->setChecked(view->send(SCI_GETINDENTATIONGUIDES) != SC_IV_NONE);
    m_actions.value(command::COLUMN_EDGE)->setChecked(view->send(SCI_GETEDGEMODE) != EDGE_NONE);
    m_actions.value(command::WHITESPACE)->setChecked(view->send(SCI_GETVIEWWS) != SCWS_INVISIBLE);
}

// Opened from the keyboard, a menu lights its first row the way the arrow keys do, which leaves a submenu closed
void editor_window::show_menu(QMenu* menu, bool is_from_keyboard) {
    if (m_open_sheet) {
        return;
    }

    if (m_settings.has_menu_bar) {
        QMenu* shown = menu == m_main_menu ? m_menus.first() : menu;
        menuBar()->setActiveAction(shown->menuAction());
        return;
    }

    if (is_from_keyboard) {
        QTimer::singleShot(0, menu, [menu] {
            QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
            QCoreApplication::sendEvent(menu, &down);
        });
    }

    QWidget* button = m_tab_row->menu();
    m_tab_row->menu()->set_open(true);
    menu->exec(button->mapToGlobal(QPoint(0, button->height())));
    m_tab_row->menu()->set_open(false);
}

int editor_window::run_sheet(sheet& open_sheet) {
    set_open_sheet(&open_sheet);
    int result = open_sheet.run();
    set_open_sheet(nullptr);
    return result;
}

// Once a sheet closes, by Esc as well, the current tab takes the keys back
void editor_window::set_open_sheet(sheet* open_sheet) {
    m_open_sheet = open_sheet;
    refresh_commands();
    if (!open_sheet) {
        focus_current_document();
    }
}

void editor_window::focus_current_document() {
    document* doc = current_document();
    doc ? doc->shown_widget()->setFocus() : m_empty_state->setFocus();
}

document* editor_window::add_document(std::unique_ptr<document> owned, int index) {
    document* doc = owned.get();
    ScintillaEditBase* view = doc->view();
    index = qBound(0, index, static_cast<int>(m_documents.size()));
    m_documents.insert(m_documents.begin() + index, std::move(owned));
    m_stack->addWidget(view);

    // A right click outside the selection moves the caret there first, as a left click would
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view, &QWidget::customContextMenuRequested, this, [this, view](const QPoint& point) {
        sptr_t at = view->send(SCI_POSITIONFROMPOINT, point.x(), point.y());
        if (at < view->send(SCI_GETSELECTIONSTART) || at > view->send(SCI_GETSELECTIONEND)) {
            view->send(SCI_GOTOPOS, at);
        }

        show_text_menu(view->viewport()->mapToGlobal(point));
    });

    connect(view, &ScintillaEditBase::savePointChanged, this, [this, doc, view] {
        QTimer::singleShot(0, view, [this, doc] {
            refresh_tab(doc);
            refresh_commands();
        });
    });

    // An untitled tab takes its name from its first line, read once the change is complete
    connect(view, &ScintillaEditBase::modified, this, [this, doc, view](Scintilla::ModificationFlags type,
                                                                        Scintilla::Position position,
                                                                        Scintilla::Position,
                                                                        Scintilla::Position lines_added,
                                                                        const QByteArray&, Scintilla::Position,
                                                                        Scintilla::FoldLevel, Scintilla::FoldLevel) {
        if (lines_added != 0) {
            fit_line_numbers(view);
        }

        bool is_text_change = static_cast<int>(type) & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT);
        if (is_text_change) {
            doc->forget_selections();
        }

        if (is_text_change && doc->is_untitled() && view->send(SCI_LINEFROMPOSITION, position) == 0) {
            QTimer::singleShot(0, view, [this, doc] { refresh_tab(doc); });
        }

        bool is_history_step = static_cast<int>(type) & (SC_PERFORMED_UNDO | SC_PERFORMED_REDO);
        if (is_text_change && is_history_step && doc->claim_eol_recount()) {
            QTimer::singleShot(0, view, [this, doc] {
                doc->recount_eols();
                refresh_status();
            });
        }

        // A paste can carry the text past the wrap limit, and wrap turns off once the change is complete
        if (is_text_change && doc->is_wrap_over_limit()) {
            QTimer::singleShot(0, view, [this, doc] {
                if (doc->limit_wrap()) {
                    refresh_status();
                }
            });
        }

        // Checked once the change is complete, so a reload that ends unmodified says nothing
        if (is_text_change && doc->is_read_only()) {
            QTimer::singleShot(0, view, [this, doc] {
                if (doc->is_modified() && doc->claim_read_only_note()) {
                    show_banner(doc, { banner_kind::READ_ONLY, doc->display_name(),
                                       QStringLiteral(" is read-only. Your edits save to a new file."),
                                       { { QStringLiteral("Save As..."), [this, doc] { save_document_as(doc); } } } });
                }
            });
        }
    });

    connect(view, &ScintillaEditBase::marginClicked, view, [view](Scintilla::Position position) {
        view->send(SCI_GOTOPOS, position);
    });

    connect(view, &ScintillaEditBase::updateUi, this, [this, doc](Scintilla::Update updated) {
        if (static_cast<int>(updated) & (SC_UPDATE_CONTENT | SC_UPDATE_V_SCROLL)) {
            doc->refresh_function_names();
        }

        if (static_cast<int>(updated) & (SC_UPDATE_CONTENT | SC_UPDATE_SELECTION)) {
            highlight_brackets(doc->view(), doc->mode() == edit_mode::CODE && !doc->is_large());
        }

        if (doc == current_document()) {
            refresh_status();
        }
    });

    connect(view, &ScintillaEditBase::resized, this, [doc] {
        doc->fit_text_width();
        doc->refresh_function_names();
    });

    // Ctrl and the wheel zoom one view, which the window then passes to every tab
    connect(view, &ScintillaEditBase::zoom, this, [this, view](int zoom) {
        fit_line_numbers(view);
        set_zoom(zoom);
    });

    view->send(SCI_SETZOOM, m_zoom);
    connect(view, &ScintillaEditBase::charAdded, this, [doc](int character) {
        continue_line(doc->view(), character, doc->mode() == edit_mode::CODE);
    });

    view->installEventFilter(this);

    {
        QSignalBlocker blocker(tabs());
        tabs()->insertTab(index, doc->display_name());
    }

    refresh_tab(doc);
    if (!m_open_sheet || tabs()->currentIndex() == index) {
        select_tab(index);
    }

    return doc;
}

document* editor_window::current_document() const {
    int index = tabs()->currentIndex();
    return index >= 0 && index < static_cast<int>(m_documents.size()) ? m_documents.at(index).get() : nullptr;
}

int editor_window::index_of(const document* doc) const {
    for (size_t index = 0; index < m_documents.size(); index++) {
        if (m_documents[index].get() == doc) {
            return static_cast<int>(index);
        }
    }

    return -1;
}

// Matched after resolving symlinks, so one file opened by two paths gets one tab
document* editor_window::find_open(const QString& path) const {
    QString canonical = QFileInfo(path).canonicalFilePath();
    for (const std::unique_ptr<document>& doc : m_documents) {
        const QString& open = doc->file().path;
        bool is_same = canonical.isEmpty() ? open == path : QFileInfo(open).canonicalFilePath() == canonical;
        if (!open.isEmpty() && is_same) {
            return doc.get();
        }
    }

    return nullptr;
}

tab_strip* editor_window::tabs() const {
    return m_tab_row->tabs();
}

// New and opened tabs go to the right of the current one, so related files stay together
// Behind a sheet new tabs join the end of the strip, so the tab an open flow holds by index stays put
int editor_window::insertion_index() const {
    return m_open_sheet ? tabs()->count() : tabs()->currentIndex() + 1;
}

bool editor_window::is_untitled_number_taken(int number) const {
    for (const std::unique_ptr<document>& doc : m_documents) {
        if (doc->is_untitled() && doc->untitled_number() == number) {
            return true;
        }
    }

    return false;
}

int editor_window::lowest_free_untitled_number() const {
    int number = 1;
    while (is_untitled_number_taken(number)) {
        number++;
    }

    return number;
}

void editor_window::remember_recent_file(const QString& path) {
    m_recent_opened.insert(path, QDateTime::currentDateTime());
    m_recent_files.removeAll(path);
    m_recent_files.prepend(path);
    while (m_recent_files.size() > RECENT_FILE_LIMIT) {
        m_recent_files.removeLast();
    }
}

QList<recent_file> editor_window::recent_files() const {
    QList<recent_file> files;
    for (const QString& path : m_recent_files) {
        files.append({ path, m_recent_opened.value(path) });
    }

    return files;
}

void editor_window::new_document() {
    add_document(std::make_unique<document>(m_stack, lowest_free_untitled_number(), m_settings), insertion_index());
}

void editor_window::choose_files_to_open(const QString& folder) {
    document* current = current_document();
    QString current_path = current ? current->file().path : QString();

    file_sheet chooser(this, file_sheet_mode::OPEN, folder, recent_files());
    open_request request;
    set_open_sheet(&chooser);
    bool is_chosen = chooser.choose_to_open(&request, current_path);
    set_open_sheet(nullptr);
    if (!is_chosen) {
        return;
    }

    m_last_sheet_folder = QFileInfo(request.paths.first()).absolutePath();
    for (const QString& path : request.paths) {
        open_path(path, request.encoding, insertion_index());
    }
}

// A file opened while the current tab is an untouched untitled one takes that tab's place
bool editor_window::open_path(const QString& path, const QByteArray& encoding, int index) {
    QFileInfo info(path);
    QString absolute = info.absoluteFilePath();
    if (document* open = find_open(absolute)) {
        if (!m_open_sheet) {
            select_tab(index_of(open));
        }

        return true;
    }

    document* current = current_document();
    if (info.isDir()) {
        choose_files_to_open(absolute);
        return false;
    }

    QString refusal = info.isFile() ? QString() : refusal_by_kind(absolute);
    if (!refusal.isEmpty()) {
        if (current) {
            show_banner(current, { banner_kind::FAILURE, QStringLiteral("Couldn't open %1").arg(absolute),
                                   QStringLiteral(": %1.").arg(refusal), {} });
        }

        return false;
    }

    if (info.isFile() && info.size() >= ASYNC_LOAD_BYTES) {
        return load_in_background(absolute, info.size(), encoding, index);
    }

    QByteArray bytes;
    if (info.exists()) {
        QFile file(absolute);
        if (!file.open(QIODevice::ReadOnly) || (bytes = file.readAll(), file.error() != QFileDevice::NoError)) {
            if (current) {
                show_banner(current, { banner_kind::FAILURE, QStringLiteral("Couldn't open %1").arg(absolute),
                                       QStringLiteral(": %1.").arg(file.errorString().toLower()), {} });
            }

            return false;
        }
    }

    document* pristine = current && current->is_pristine() && !m_open_sheet ? current : nullptr;
    auto doc = std::make_unique<document>(m_stack, lowest_free_untitled_number(), m_settings);
    if (!info.exists()) {
        doc->set_new_path(absolute);
    } else if (encoding.isEmpty() && looks_binary(bytes)) {
        hold_binary(doc.get(), absolute, bytes);
    } else if (!doc->load(absolute, bytes, encoding)) {
        if (current) {
            QString reason = QStringLiteral(" is not valid %1, so it was not opened.").arg(encoding_label(encoding));
            show_banner(current, { banner_kind::FAILURE, info.fileName(), reason, {} });
        }

        return false;
    }

    remember_recent_file(absolute);
    add_document(std::move(doc), index);
    if (pristine) {
        remove_document(index_of(pristine), false);
    }

    return true;
}

// A large file reads on a worker thread behind its tab's progress, and one that may not fit in memory asks first.
// Before the window shows there is nothing to ask over, so a file named at launch opens
bool editor_window::load_in_background(const QString& path, qint64 size, const QByteArray& encoding, int index) {
    QString name = QFileInfo(path).fileName();
    qint64 free_bytes = free_memory();
    if (free_bytes > 0 && size > free_bytes / 2 && isVisible() && !m_open_sheet) {
        QString title = QStringLiteral("%1 is %2 and %3 of memory is free").arg(name, size_label(size),
                                                                                 size_label(free_bytes));
        prompt_sheet question(this, title, QStringLiteral("Opening it could run Stellux out of memory."),
                              QStringLiteral("Open Anyway"), QString(), false);
        if (static_cast<prompt_answer>(run_sheet(question)) != prompt_answer::PRIMARY) {
            return false;
        }
    }

    document* current = current_document();
    document* pristine = current && current->is_pristine() && !m_open_sheet ? current : nullptr;

    auto owned = std::make_unique<document>(m_stack, lowest_free_untitled_number(), m_settings);
    document* doc = owned.get();
    auto* placeholder = new loading_placeholder(m_stack, name, size);
    m_stack->addWidget(placeholder);
    file_loader* loader = doc->begin_load(path, size, encoding, placeholder);

    placeholder->on_cancel = [this, doc] { close_document(index_of(doc)); };
    loader->on_finished = [this, doc](const load_result& result) { finish_loading(doc, result); };
    loader->on_progress = [this, doc, placeholder](qint64 done, qint64 total) {
        placeholder->set_progress(done, total);
        tabs()->set_tab_progress(index_of(doc), static_cast<int>(done * PERMILLE / total));
    };

    remember_recent_file(path);
    add_document(std::move(owned), index);
    if (pristine) {
        remove_document(index_of(pristine), false);
    }

    tabs()->set_tab_progress(index_of(doc), 0);
    loader->start();
    return true;
}

// A file that cannot be read or decoded closes its tab and says why over the tab that takes its place
void editor_window::finish_loading(document* doc, const load_result& result) {
    int index = index_of(doc);
    tabs()->set_tab_progress(index, -1);
    QString path = doc->file().path;
    if (result.outcome == load_outcome::TEXT) {
        doc->finish_load(result);
    } else if (result.outcome == load_outcome::BINARY) {
        doc->end_load();
        hold_binary(doc, path, result.bytes);
    } else {
        bool is_undecodable = result.outcome == load_outcome::UNDECODABLE;
        QString subject = is_undecodable ? QFileInfo(path).fileName() : QStringLiteral("Couldn't open %1").arg(path);
        QString reason = is_undecodable
                       ? QStringLiteral(" is not valid %1, so it was not opened.").arg(encoding_label(result.encoding))
                       : QStringLiteral(": %1.").arg(result.error.toLower());
        remove_document(index, false);
        if (document* current = current_document()) {
            show_banner(current, { banner_kind::FAILURE, subject, reason, {} });
        }

        return;
    }

    refresh_tab(doc);
    if (doc == current_document()) {
        show_current_document();
    }
}

// The file's bytes wait behind a placeholder in its tab until the user picks how to read them
void editor_window::hold_binary(document* doc, const QString& path, const QByteArray& bytes) {
    auto* placeholder = new binary_placeholder(m_stack, QFileInfo(path).fileName(), guess_utf16(bytes));
    m_stack->addWidget(placeholder);
    doc->hold(path, bytes, placeholder);
    placeholder->on_close = [this, doc] { close_document(index_of(doc)); };
    placeholder->on_open_as = [this, doc](const QByteArray& encoding) {
        if (!doc->open_held(encoding)) {
            show_banner(doc, { banner_kind::FAILURE, doc->display_name(),
                               QStringLiteral(" is not valid %1.").arg(encoding_label(encoding)), {} });
            return;
        }

        show_current_document();
    };
}

// A clean tab whose file is on disk has nothing to write, and a file changed elsewhere asks before it is replaced
// A never-saved or read-only file saves through Save As, in its own folder
bool editor_window::save_document(document* doc) {
    if (doc->is_page()) {
        return save_settings();
    }

    if (doc->is_untitled() || doc->is_read_only()) {
        return save_document_as(doc);
    }

    if (!doc->is_modified() && QFileInfo(doc->file().path).isFile()) {
        return true;
    }

    if (doc->check_disk() == disk_state::CHANGED) {
        select_tab(index_of(doc));
        prompt_sheet conflict(this, QStringLiteral("%1 changed on disk since you opened it").arg(doc->display_name()),
                              QStringLiteral("Saving replaces the changes made outside Sheen."),
                              QStringLiteral("Save Anyway"), QStringLiteral("Reload Theirs"), true);
        prompt_answer answer = static_cast<prompt_answer>(run_sheet(conflict));
        if (answer == prompt_answer::DESTRUCTIVE) {
            revert_document(doc);
            return false;
        }

        if (answer != prompt_answer::PRIMARY) {
            return false;
        }
    }

    return write_document(doc, doc->file().path);
}

bool editor_window::save_document_as(document* doc, const QString& title) {
    QString name = doc->display_name();
    if (doc->is_untitled()) {
        QString first_line = doc->first_line_name();
        name = first_line.isEmpty() ? QStringLiteral("Untitled") : first_line;

        // A first line the tab cut short gives only its whole words
        if (name.endsWith(QChar(0x2026))) {
            name.chop(1);
            qsizetype last_space = name.lastIndexOf(QLatin1Char(' '));
            name = last_space > 0 ? name.left(last_space).trimmed() : name;
        }

        // A sentence's closing mark would meet the extension's dot
        while (name.size() > 1 && QStringLiteral(".,;:!?").contains(name.back())) {
            name.chop(1);
        }

        name += doc->lang() ? QString() : QStringLiteral(".txt");
    }

    QString folder = doc->is_untitled() ? sheet_folder() : QFileInfo(doc->file().path).absolutePath();
    file_sheet chooser(this, file_sheet_mode::SAVE_AS, folder, recent_files());
    if (!title.isEmpty()) {
        chooser.set_title(title);
    }

    if (doc->is_untitled() && !doc->first_line_name().isEmpty()) {
        chooser.set_note(QStringLiteral("never saved, the name comes from its first line"));
    }

    save_request request;
    set_open_sheet(&chooser);
    bool is_chosen = chooser.choose_to_save(name, doc->file().encoding, doc->file().eol_mode, &request);
    set_open_sheet(nullptr);
    if (!is_chosen) {
        return false;
    }

    // Saving over a file another tab holds closes that tab, asking first when it has unsaved changes
    document* other = find_open(request.path);
    other = other == doc ? nullptr : other;
    if (other && other->is_modified()) {
        QString title = QStringLiteral("%1 is open in another tab with unsaved changes").arg(other->display_name());
        prompt_sheet question(this, title, QStringLiteral("Saving over it closes that tab, and its changes are lost."),
                              QStringLiteral("Save Anyway"), QString(), true);
        if (static_cast<prompt_answer>(run_sheet(question)) != prompt_answer::PRIMARY) {
            return false;
        }
    }

    m_last_sheet_folder = QFileInfo(request.path).absolutePath();
    file_record previous = doc->file();
    doc->set_encoding(request.encoding);
    if (request.eol_mode != previous.eol_mode) {
        doc->convert_eols(request.eol_mode);
    }

    if (!write_document(doc, request.path)) {
        doc->set_encoding(previous.encoding);
        return false;
    }

    if (other) {
        remove_document(index_of(other), false);
    }

    remember_recent_file(request.path);
    return true;
}

// A failed write leaves the file as it was and says so in a banner with the ways forward
bool editor_window::write_document(document* doc, const QString& path) {
    QString error;
    unencodable_char unencodable;
    if (doc->save(path, &error, &unencodable)) {
        doc->set_banner(std::nullopt);
        refresh_tab(doc);
        refresh_banner();
        refresh_status();
        if (QFileInfo(path).absoluteFilePath() == QLatin1String(SETTINGS_PATH)) {
            take_settings_from_text();
        }

        return true;
    }

    if (unencodable.line > 0) {
        QString encoding = encoding_label(doc->file().encoding);
        show_banner(doc, { banner_kind::FAILURE, encoding,
                           QStringLiteral(" can't hold %1 on line %2. Nothing was saved.")
                               .arg(character_phrase(unencodable.code))
                               .arg(unencodable.line),
                           { { QStringLiteral("Go to Line"), [doc, line = unencodable.line] {
                                  doc->view()->send(SCI_GOTOLINE, line - 1);
                                  doc->view()->setFocus();
                              } },
                             { QStringLiteral("Save as UTF-8"), [this] { save_with_encoding({ UTF_8, false }); } } } });
        return false;
    }

    QString name = QFileInfo(path).fileName();
    show_banner(doc, { banner_kind::FAILURE, QStringLiteral("Couldn't save %1").arg(name),
                       QStringLiteral(": %1. The file on disk is unchanged.").arg(error.toLower()),
                       { { QStringLiteral("Retry"), [this, doc] { save_document(doc); } },
                         { QStringLiteral("Save As"), [this, doc] { save_document_as(doc); } } } });
    return false;
}

// Saves in strip order and stops at the first failed or cancelled save
// Files that need a name ask for it in turn, each over its own tab, and count off when several do
bool editor_window::save_documents(const QList<document*>& docs, const QString& purpose) {
    QList<document*> unnamed;
    for (document* doc : docs) {
        if (doc->is_untitled() || doc->is_read_only()) {
            unnamed.append(doc);
        }
    }

    for (document* doc : docs) {
        qsizetype place = unnamed.indexOf(doc);
        if (place >= 0) {
            select_tab(index_of(doc));
        }

        QString title = place < 0 || unnamed.size() < 2
                      ? QString()
                      : QStringLiteral("Save As, %1 of %2").arg(place + 1).arg(unnamed.size());
        if (!title.isEmpty() && !purpose.isEmpty()) {
            title += QStringLiteral(" before %1").arg(purpose);
        }

        if (!(place >= 0 ? save_document_as(doc, title) : save_document(doc))) {
            return false;
        }
    }

    return true;
}

// The reload is one undo step, so reverting needs no question
void editor_window::revert_document(document* doc) {
    QString error;
    if (!doc->reload_from_disk(&error)) {
        show_banner(doc, { banner_kind::FAILURE, QStringLiteral("Couldn't reload %1").arg(doc->display_name()),
                           QStringLiteral(": %1.").arg(error.toLower()), {} });
        return;
    }

    doc->set_banner(std::nullopt);
    refresh_tab(doc);
    refresh_banner();
    refresh_status();
}

bool editor_window::close_document(int index) {
    document* doc = m_documents.at(index).get();
    if (!doc->is_modified() || (doc->is_untitled() && doc->view()->send(SCI_GETLENGTH) == 0)) {
        remove_document(index, false);
        return true;
    }

    prompt_answer answer = ask_about_unsaved(doc, QStringLiteral("closing"));
    if (answer == prompt_answer::CANCEL || (answer == prompt_answer::PRIMARY && !save_document(doc))) {
        return false;
    }

    remove_document(index_of(doc), answer == prompt_answer::DESTRUCTIVE);
    return true;
}

// Clean tabs close at once. One unsaved document then gets the single prompt and several get one checklist.
// Returns whether they all closed
bool editor_window::close_documents(const QList<document*>& docs, const QString& purpose) {
    bool is_quitting = purpose == QLatin1String("quitting");
    QList<document*> unsaved;
    for (document* doc : docs) {
        if (doc->is_modified() && !(doc->is_untitled() && doc->view()->send(SCI_GETLENGTH) == 0)) {
            unsaved.append(doc);
        } else if (!is_quitting) {
            remove_document(index_of(doc), false);
        }
    }

    QList<document*> discarded;
    if (unsaved.size() == 1) {
        prompt_answer answer = ask_about_unsaved(unsaved.first(), purpose);
        if (answer == prompt_answer::CANCEL || (answer == prompt_answer::PRIMARY && !save_document(unsaved.first()))) {
            return false;
        }

        if (answer == prompt_answer::DESTRUCTIVE) {
            discarded.append(unsaved.first());
        }
    } else if (unsaved.size() > 1) {
        QList<unsaved_entry> entries;
        for (document* doc : unsaved) {
            QString place = doc->is_page()     ? QFileInfo(QString::fromLatin1(SETTINGS_PATH)).absolutePath()
                          : doc->is_untitled() ? QStringLiteral("never saved, Save asks where")
                                               : QFileInfo(doc->file().path).absolutePath();
            entries.append({ doc->display_name(), place });
        }

        QString body = is_quitting
                     ? QStringLiteral("Changes you don't save are lost, since nothing persists across a restart yet.")
                     : QStringLiteral("Changes you don't save are lost when the tabs close.");
        QString ending = is_quitting ? QStringLiteral("quitting") : QStringLiteral("closing them");
        QString title = QStringLiteral("Save changes to %1 files before %2?").arg(unsaved.size()).arg(ending);
        checklist_sheet checklist(this, title, body, entries);

        QList<bool> checked;
        set_open_sheet(&checklist);
        prompt_answer answer = checklist.ask(&checked);
        set_open_sheet(nullptr);
        if (answer == prompt_answer::CANCEL) {
            return false;
        }

        QList<document*> to_save;
        for (int index = 0; index < unsaved.size(); index++) {
            bool saves = answer == prompt_answer::PRIMARY && checked.at(index);
            (saves ? to_save : discarded).append(unsaved.at(index));
        }

        if (!save_documents(to_save, purpose)) {
            return false;
        }
    }

    if (is_quitting) {
        return true;
    }

    for (document* doc : unsaved) {
        remove_document(index_of(doc), discarded.contains(doc));
    }

    return true;
}

// Save, Don't Save or Cancel for one document, with Enter saving and Ctrl D not saving
prompt_answer editor_window::ask_about_unsaved(document* doc, const QString& purpose) {
    select_tab(index_of(doc));
    QString name = doc->display_name();
    QString title = doc->is_untitled() ? QStringLiteral("Save \"%1\" before %2?").arg(name, purpose)
                                       : QStringLiteral("Save changes to %1 before %2?").arg(name, purpose);
    QString body = doc->is_page()     ? QStringLiteral("Your unsaved settings are lost if you don't save them.")
                 : doc->is_untitled() ? QStringLiteral("It has never been saved.")
                 : doc->is_deleted()  ? QStringLiteral("It was deleted on disk, so its text exists only here.")
                                      : QStringLiteral("Your changes to %1 will be lost if you don't save them.")
                                           .arg(lines_phrase(qMax(1, doc->changed_line_count())));
    prompt_sheet prompt(this, title, body, QStringLiteral("Save"), QStringLiteral("Don't Save"), true);
    return static_cast<prompt_answer>(run_sheet(prompt));
}

// A closed tab joins the reopen stack, keeping its Scintilla document when its text exists nowhere else, except a
// large file's, which keeps only its path
void editor_window::remove_document(int index, bool keeps_text) {
    document* doc = m_documents.at(index).get();
    bool has_only_copy = keeps_text || (doc->is_untitled() && doc->view()->send(SCI_GETLENGTH) > 0);
    has_only_copy = has_only_copy && !doc->is_large();
    closed_tab closed = { doc->file(), doc->lang(), doc->current_view_state(), index, doc->untitled_number(),
                          has_only_copy ? doc->keep_scintilla_document() : 0 };
    if (!doc->is_page() && (!doc->is_untitled() || closed.scintilla_document)) {
        m_closed_tabs.prepend(closed);
    }

    while (m_closed_tabs.size() > CLOSED_TAB_LIMIT) {
        closed_tab dropped = m_closed_tabs.takeLast();
        if (dropped.scintilla_document) {
            doc->view()->send(SCI_RELEASEDOCUMENT, 0, dropped.scintilla_document);
        }
    }

    auto is_in_closing_tab = [doc](const caret_place& place) { return place.doc == doc; };
    m_back_places.removeIf(is_in_closing_tab);
    m_forward_places.removeIf(is_in_closing_tab);

    // The most recently used tab takes over, as Ctrl Tab would reach it
    m_recent_tabs.removeAll(doc);
    document* next = m_recent_tabs.isEmpty() ? nullptr : m_recent_tabs.first();
    m_stack->removeWidget(doc->view());
    std::unique_ptr<document> owned = std::move(m_documents[index]);
    m_documents.erase(m_documents.begin() + index);
    {
        QSignalBlocker blocker(tabs());
        tabs()->removeTab(index);
    }

    if (next) {
        select_tab(index_of(next));
    }

    refresh_tab_names();
    if (m_documents.empty()) {
        m_find_bar->set_view(nullptr);
        m_find_bar->hide();
        show_empty_state();
        setWindowTitle(QStringLiteral("Sheen"));
        refresh_banner();
        refresh_status();
    }

    refresh_commands();
}

void editor_window::reopen_closed_tab() {
    if (m_closed_tabs.isEmpty()) {
        return;
    }

    closed_tab closed = m_closed_tabs.takeFirst();
    if (!closed.scintilla_document) {
        if (!QFileInfo(closed.file.path).isFile()) {
            if (document* current = current_document()) {
                QString subject = QStringLiteral("Couldn't reopen %1").arg(QFileInfo(closed.file.path).fileName());
                show_banner(current, { banner_kind::FAILURE, subject, QStringLiteral(": it no longer exists."), {} });
            }

            refresh_commands();
            return;
        }

        if (open_path(closed.file.path, closed.file.encoding.name, closed.index)) {
            current_document()->restore_view_state(closed.view);
        }

        refresh_commands();
        return;
    }

    // An untitled tab opened since the close may hold the number, and then the lowest free one is taken
    int number = closed.untitled_number;
    if (is_untitled_number_taken(number)) {
        number = lowest_free_untitled_number();
    }

    auto doc = std::make_unique<document>(m_stack, number, m_settings);
    doc->adopt(closed.scintilla_document, closed.file, closed.lang);
    doc->restore_view_state(closed.view);
    add_document(std::move(doc), closed.index);
    refresh_commands();
}

// A clean tab follows its file quietly, an unsaved one keeps the user's text and says what happened
// A held file has no text yet to fall behind its file
void editor_window::check_disk(document* doc) {
    if (doc->placeholder()) {
        return;
    }

    if (doc->refresh_read_only()) {
        refresh_tab(doc);
        refresh_status();
    }

    disk_state state = doc->check_disk();
    if (state == disk_state::SAME) {
        return;
    }

    QString name = doc->display_name();
    if (state == disk_state::DELETED) {
        if (doc->is_deleted()) {
            return;
        }

        doc->set_deleted(true);
        show_banner(doc, { banner_kind::CHANGE, name, QStringLiteral(" was deleted on disk. Saving writes it again."),
                           { { QStringLiteral("Save"), [this, doc] { save_document(doc); } },
                             { QStringLiteral("Close Tab"), [this, doc] { close_document(index_of(doc)); } } } });
        refresh_tab(doc);
        return;
    }

    if (!doc->is_modified()) {
        QString error;
        if (doc->reload_from_disk(&error)) {
            refresh_tab(doc);
            if (doc == current_document()) {
                show_status_note(QStringLiteral("Reloaded, changed on disk"));
            }
        }

        return;
    }

    show_banner(doc, { banner_kind::CHANGE, name, QStringLiteral(" changed on disk. Your unsaved edits are kept."),
                       { { QStringLiteral("Reload"), [this, doc] { revert_document(doc); } },
                         { QStringLiteral("Keep Mine"), [this, doc] {
                              doc->accept_disk_state();
                              doc->set_banner(std::nullopt);
                              refresh_banner();
                          } } } });
}

// The empty state's rows run the same commands as their menu rows, so their keycaps follow the shortcuts
void editor_window::show_empty_state() {
    QList<std::pair<command, QString>> rows = { { command::NEW_FILE, QStringLiteral("New File") },
                                                { command::OPEN_FILE, QStringLiteral("Open File...") } };
    if (!m_closed_tabs.isEmpty()) {
        rows.append({ command::REOPEN_CLOSED_TAB, QStringLiteral("Reopen Closed Tab") });
    }

    rows.append({ command::COMMAND_PALETTE, QStringLiteral("Command Palette") });
    QList<empty_action> actions;
    for (const auto& [id, label] : rows) {
        actions.append({ label, action_keys(m_actions.value(id)), [this, id] { run(id); }, empty_glyph(id) });
    }

    m_empty_state->set_entries(actions, m_recent_files);
    m_stack->setCurrentWidget(m_empty_state);
    m_empty_state->setFocus();
}

// The zoom belongs to the window, so every tab shows its text at one size, and it is never saved
void editor_window::set_zoom(int zoom) {
    m_zoom = qBound(MIN_ZOOM, zoom, MAX_ZOOM);
    for (const std::unique_ptr<document>& open : m_documents) {
        if (open->view()->send(SCI_GETZOOM) != m_zoom) {
            open->view()->send(SCI_SETZOOM, m_zoom);
        }
    }

    refresh_status();
}

void editor_window::select_tab(int index) {
    if (index < 0 || index >= tabs()->count()) {
        return;
    }

    if (tabs()->currentIndex() == index) {
        show_current_document();
        return;
    }

    tabs()->setCurrentIndex(index);
}

// A first Ctrl Tab picks the previous tab and a second while Ctrl stays held shows the list. Ctrl coming up opens
// the lit tab, so a quick tap flips between two files
void editor_window::step_recent_tab(int step) {
    int count = static_cast<int>(m_recent_tabs.size());
    if (count < 2) {
        return;
    }

    if (!m_is_switching_tabs) {
        m_is_switching_tabs = true;
        m_switch_index = step > 0 ? 1 : count - 1;
        qApp->installEventFilter(this);
        return;
    }

    m_switch_index = (m_switch_index + step + count) % count;
    QStringList names;
    QList<bool> unsaved;
    for (document* doc : m_recent_tabs) {
        names.append(doc->display_name());
        unsaved.append(doc->is_modified());
    }

    int top = m_tab_row->mapTo(this, QPoint(0, m_tab_row->height())).y() + SWITCHER_GAP;
    m_switcher->show_list(names, unsaved, m_switch_index, top);
}

void editor_window::finish_tab_switch(bool opens_lit_tab) {
    qApp->removeEventFilter(this);
    m_is_switching_tabs = false;
    m_switcher->hide();
    if (opens_lit_tab && m_switch_index < m_recent_tabs.size()) {
        select_tab(index_of(m_recent_tabs.at(m_switch_index)));
    }
}

// Watches each text for Enter, and the whole application while Ctrl Tab is held for Ctrl coming up, Esc or a click
bool editor_window::eventFilter(QObject* watched, QEvent* event) {
    auto* view = qobject_cast<ScintillaEditBase*>(watched);
    if (view && event->type() == QEvent::KeyPress && type_line_end(view, static_cast<QKeyEvent*>(event))) {
        return true;
    }

    if (!m_is_switching_tabs) {
        return QMainWindow::eventFilter(watched, event);
    }

    bool is_ctrl_up = event->type() == QEvent::KeyRelease && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Control;
    bool is_escape = event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape;
    if (is_ctrl_up || is_escape || event->type() == QEvent::MouseButtonPress) {
        finish_tab_switch(is_ctrl_up);
        return is_escape;
    }

    return QMainWindow::eventFilter(watched, event);
}

void editor_window::show_tab_list() {
    QMenu menu(this);
    menu.setStyle(m_menu_style);
    for (int index = 0; index < tabs()->count(); index++) {
        document* doc = m_documents[index].get();
        QString label = doc->display_name();
        if (doc->is_modified()) {
            label += QStringLiteral("  ") + BULLET;
        }

        QAction* action = menu.addAction(label, [this, index] { select_tab(index); });
        action->setCheckable(true);
        action->setChecked(index == tabs()->currentIndex());
    }

    menu.addSeparator();
    menu.addAction(QStringLiteral("Close Saved Tabs"), [this] { close_saved_tabs(); });
    menu.addAction(QStringLiteral("Close All Tabs"), [this] { run(command::CLOSE_ALL_TABS); });

    QWidget* button = m_tab_row->tab_list();
    m_tab_row->tab_list()->set_open(true);
    menu.exec(button->mapToGlobal(QPoint(button->width() - menu.sizeHint().width(), button->height())));
    m_tab_row->tab_list()->set_open(false);
}

void editor_window::close_saved_tabs() {
    QList<document*> saved;
    for (const std::unique_ptr<document>& open : m_documents) {
        if (!open->is_modified()) {
            saved.append(open.get());
        }
    }

    close_documents(saved, QStringLiteral("closing"));
}

void editor_window::show_current_document() {
    document* doc = current_document();
    if (!doc) {
        return;
    }

    if (!m_recent_tabs.isEmpty() && m_recent_tabs.first() != doc) {
        record_jump(m_recent_tabs.first());
    }

    m_recent_tabs.removeAll(doc);
    m_recent_tabs.prepend(doc);

    QWidget* shown = doc->shown_widget();
    m_stack->setCurrentWidget(shown);
    m_find_bar->set_view(doc->placeholder() ? nullptr : doc->view());
    doc->refresh_function_names();
    if (!m_open_sheet) {
        shown->setFocus();
        check_disk(doc);
    }

    setWindowTitle(doc->display_name());
    refresh_banner();
    refresh_commands();
    refresh_status();
}

void editor_window::refresh_tab(document* doc) {
    int index = index_of(doc);
    if (index < 0) {
        return;
    }

    refresh_tab_names();
    tabs()->setTabToolTip(index, doc->is_page()     ? QString::fromLatin1(SETTINGS_PATH)
                                 : doc->is_untitled() ? QStringLiteral("Never saved")
                                 : doc->is_deleted()  ? QStringLiteral("Deleted on disk")
                                                      : doc->file().path);
    tabs()->set_tab_modified(index, doc->is_modified());
    tabs()->updateGeometry();
    if (doc == current_document()) {
        setWindowTitle(doc->display_name());
    }
}

// Tabs that share a name each add the folder that tells them apart, so a name's change can reach every tab
void editor_window::refresh_tab_names() {
    for (int index = 0; index < static_cast<int>(m_documents.size()); index++) {
        document* doc = m_documents[index].get();
        QStringList twins;
        for (const std::unique_ptr<document>& other : m_documents) {
            if (other.get() != doc && !other->is_untitled() && other->display_name() == doc->display_name()) {
                twins.append(other->file().path);
            }
        }

        QString hint = doc->is_untitled() || twins.isEmpty() ? QString() : telling_folder(doc->file().path, twins);
        tabs()->set_tab_locked(index, doc->is_read_only());
        tabs()->set_tab_struck(index, doc->is_deleted());
        tabs()->set_tab_name(index, doc->display_name(), hint);
    }
}

void editor_window::refresh_status() {
    document* doc = current_document();
    QStringList notes = { m_status_note };
    if (doc && doc->is_read_only()) {
        notes.prepend(QStringLiteral("Read-only"));
    }

    if (doc && doc->is_large()) {
        notes.prepend(QStringLiteral("Large file: no highlighting, wrap or folding"));
    }

    if (doc && doc->is_wrap_limited()) {
        notes.prepend(QStringLiteral("Unwrapped at this size, Alt Z wraps it"));
    }

    notes.removeAll(QString());
    m_status->set_message(notes.join(NOTE_SEPARATOR));
    if (!doc || doc->placeholder()) {
        m_status->set_items({}, {});
        return;
    }

    ScintillaEditBase* view = doc->view();
    sptr_t caret = view->send(SCI_GETCURRENTPOS);
    sptr_t line = view->send(SCI_LINEFROMPOSITION, caret) + 1;
    sptr_t column = view->send(SCI_GETCOLUMN, caret) + 1;
    QString position = QStringLiteral("Ln %1, Col %2").arg(line).arg(column);
    QList<status_item> left = { { status_kind::POSITION, position, {}, true } };

    sptr_t selections = view->send(SCI_GETSELECTIONS);
    sptr_t start = view->send(SCI_GETSELECTIONSTART);
    sptr_t end = view->send(SCI_GETSELECTIONEND);
    if (selections > 1) {
        left.append({ status_kind::SELECTION, QStringLiteral("%1 carets").arg(selections), {}, false });
    } else if (end > start) {
        sptr_t count = view->send(SCI_COUNTCHARACTERS, start, end);
        left.append({ status_kind::SELECTION, QStringLiteral("%1 selected").arg(count), {}, false });
    }

    bool uses_tabs = view->send(SCI_GETUSETABS) != 0;
    QString indent = uses_tabs ? QStringLiteral("Tabs: %1").arg(view->send(SCI_GETTABWIDTH))
                               : QStringLiteral("Spaces: %1").arg(view->send(SCI_GETINDENT));
    left.append({ status_kind::INDENTATION, indent, {}, true });
    if (view->send(SCI_GETOVERTYPE)) {
        QString tooltip = QStringLiteral("Typing replaces text (Insert)");
        left.append({ status_kind::OVERTYPE, QStringLiteral("OVR"), tooltip, false });
    }

    const file_record& file = doc->file();
    QString eol = eol_label(file.eol_mode);
    if (file.has_mixed_eols) {
        eol = QStringLiteral("%1, mixed").arg(eol);
    }

    QString encoding_note = file.encoding.name.startsWith("UTF-") ? QString()
                          : QStringLiteral("Not valid UTF-8, opened as %1").arg(encoding_label(file.encoding));
    QString mode = doc->mode() == edit_mode::CODE ? QStringLiteral("Code") : QStringLiteral("Text");
    QList<status_item> right = {
        { status_kind::MODE, mode, {}, !doc->is_large() },
        { status_kind::LANGUAGE, language_name(doc->lang()), {}, !doc->is_large() },
        { status_kind::ENCODING, encoding_label(file.encoding), encoding_note, true },
        { status_kind::LINE_ENDING, eol, {}, true },
    };

    // Away from 100 percent the zoom shows, and a click puts it back
    if (m_zoom != 0) {
        QString zoom = QStringLiteral("%1%").arg(zoom_percent(m_zoom));
        right.prepend({ status_kind::ZOOM, zoom, QStringLiteral("A click resets the zoom (Ctrl 0)"), true });
    }

    m_status->set_items(left, right);
}

void editor_window::refresh_banner() {
    document* doc = current_document();
    if (!doc || !doc->banner()) {
        m_banner->hide();
        return;
    }

    m_banner->show_message(*doc->banner());
    m_banner->show();
}

void editor_window::show_banner(document* doc, const banner_message& message) {
    doc->set_banner(message);
    if (doc == current_document()) {
        refresh_banner();
    }
}

void editor_window::show_status_note(const QString& note) {
    m_status_note = note;
    refresh_status();
    QTimer::singleShot(STATUS_NOTE_MS, this, [this, note] {
        if (m_status_note == note) {
            m_status_note.clear();
            refresh_status();
        }
    });
}

// Each status button's menu opens upward from it, and the button shows as open meanwhile
// Ctrl G previews each line as its digits arrive, Enter moves the caret there and Esc puts the view back
void editor_window::go_to_line(bool is_from_palette) {
    document* doc = current_document();
    if (!doc || doc->placeholder() || m_open_sheet) {
        return;
    }

    QPointer<ScintillaEditBase> view = doc->view();
    view_place before = { view->send(SCI_GETANCHOR), view->send(SCI_GETCURRENTPOS), view->send(SCI_GETFIRSTVISIBLELINE),
                          view->send(SCI_GETXOFFSET) };
    sptr_t lines = view->send(SCI_GETLINECOUNT);
    QString help = QStringLiteral("A line from 1 to %1, or line:column").arg(lines);
    QString from = QStringLiteral("Ln %1").arg(view->send(SCI_LINEFROMPOSITION, before.caret) + 1);
    QString aside = QStringLiteral("from %1, Esc returns").arg(picker::code(from));

    define_line_tint(view, GOTO_MARKER, STLXTHEME_MATCH_MARK);

    // Puts back the caret, selection and scroll the preview moved
    auto restore = [view, before] {
        if (!view) {
            return;
        }

        view->send(SCI_MARKERDELETEALL, GOTO_MARKER);
        view->send(SCI_SETSEL, before.anchor, before.caret);
        view->send(SCI_SETFIRSTVISIBLELINE, before.first_line);
        view->send(SCI_SETXOFFSET, before.x_offset);
    };

    m_picker->on_changed = [this, view, lines, help, is_from_palette, restore](const QString& query) {
        sptr_t line = 0;
        sptr_t column = 0;
        if (!view) {
            return;
        }

        // From the palette, > or @ in place of a line goes back to its lists
        if (is_from_palette && (query.startsWith(QLatin1Char('>')) || query.startsWith(QLatin1Char('@')))) {
            restore();
            open_palette(query);
            return;
        }

        if (!read_line_target(query, &line, &column)) {
            view->send(SCI_MARKERDELETEALL, GOTO_MARKER);
            m_picker->set_help(help);
            return;
        }

        preview_line(view, qMin(line, lines));
        m_picker->set_help(line_target_help(query, line, column, lines));
    };

    m_picker->on_accepted = [this, doc, view, lines](const QString& query) {
        sptr_t line = 0;
        sptr_t column = 0;
        if (!view) {
            return;
        }

        view->send(SCI_MARKERDELETEALL, GOTO_MARKER);
        if (read_line_target(query, &line, &column)) {
            record_jump(doc);
            view->send(SCI_GOTOPOS, view->send(SCI_FINDCOLUMN, qMin(line, lines) - 1, column - 1));
        }

        view->setFocus();
    };

    m_picker->on_cancelled = [view, restore] {
        restore();
        if (view) {
            view->setFocus();
        }
    };

    m_picker->open(QStringLiteral(":"), help, aside);
    if (is_from_palette) {
        m_picker->on_prefix_erased = [this, restore] {
            restore();
            open_palette(QString());
        };
    }
}

// Each status button's menu opens upward from it, and the button shows as open meanwhile
void editor_window::show_status_menu(status_kind kind, const QPoint& anchor) {
    document* doc = current_document();
    if (!doc || m_open_sheet) {
        return;
    }

    if (kind == status_kind::POSITION) {
        go_to_line(false);
        return;
    }

    if (kind == status_kind::ZOOM) {
        set_zoom(0);
        return;
    }

    QMenu menu(this);
    menu.setStyle(m_menu_style);
    switch (kind) {
        case status_kind::MODE:
            menu.addAction(m_actions.value(command::CODE_MODE));
            menu.addAction(m_actions.value(command::TEXT_MODE));
            break;
        case status_kind::INDENTATION:
            fill_indentation_menu(doc, &menu);
            break;
        case status_kind::LANGUAGE:
            fill_submenu(command::LANGUAGE, &menu);
            break;
        case status_kind::ENCODING:
            fill_encoding_menu(doc, &menu);
            break;
        case status_kind::LINE_ENDING:
            fill_line_ending_menu(doc, &menu);
            break;
        default:
            return;
    }

    // A menu that would cross the window's right edge slides left to stay over it
    QSize size = menu.sizeHint();
    int window_right = mapToGlobal(QPoint(width(), 0)).x();
    m_status->set_open_item(&kind);
    menu.exec(QPoint(qMin(anchor.x(), window_right - size.width()), anchor.y() - size.height()));
    m_status->set_open_item(nullptr);
}

void editor_window::fill_indentation_menu(document* doc, QMenu* menu) {
    ScintillaEditBase* view = doc->view();
    bool uses_tabs = view->send(SCI_GETUSETABS) != 0;
    int width = static_cast<int>(view->send(SCI_GETINDENT));
    QAction* spaces = menu->addAction(QStringLiteral("Indent Using Spaces"), [this, doc, width] {
        doc->set_indentation({ false, width });
        refresh_status();
    });
    spaces->setCheckable(true);
    spaces->setChecked(!uses_tabs);

    QAction* tabs_row = menu->addAction(QStringLiteral("Indent Using Tabs"), [this, doc, width] {
        doc->set_indentation({ true, width });
        refresh_status();
    });
    tabs_row->setCheckable(true);
    tabs_row->setChecked(uses_tabs);

    menu->addSeparator();
    for (int option : INDENT_WIDTHS) {
        QAction* row = menu->addAction(QStringLiteral("Width %1").arg(option), [this, doc, uses_tabs, option] {
            doc->set_indentation({ uses_tabs, option });
            refresh_status();
        });
        row->setCheckable(true);
        row->setChecked(option == width);
    }

    menu->addSeparator();
    menu->addAction(QStringLiteral("Detect from Content"), [this, doc] {
        if (std::optional<indentation> indent = doc->detect_indentation()) {
            doc->set_indentation(*indent);
        }

        refresh_status();
    });

    menu->addAction(QStringLiteral("Convert Indentation to Spaces"), [this, doc] {
        doc->convert_indentation(false);
        refresh_status();
    });

    menu->addAction(QStringLiteral("Convert Indentation to Tabs"), [this, doc] {
        doc->convert_indentation(true);
        refresh_status();
    });
}

// Headed by the encoding the file was opened as, with how that was decided underneath
void editor_window::fill_encoding_menu(document* doc, QMenu* menu) {
    const file_encoding& encoding = doc->file().encoding;
    QString heading = doc->is_untitled() ? QStringLiteral("Saves as %1") : QStringLiteral("Opened as %1");
    menu->addAction(heading.arg(encoding_label(encoding)))->setEnabled(false);
    menu->addSeparator();

    QAction* reopen = menu->addAction(QStringLiteral("Reopen with Encoding..."),
                                      [this] { pick_encoding(command::REOPEN_WITH_ENCODING); });
    reopen->setEnabled(!doc->is_untitled() && QFileInfo(doc->file().path).isFile());
    menu->addAction(QStringLiteral("Save with Encoding..."), [this] { pick_encoding(command::SAVE_WITH_ENCODING); });
    if (doc->is_untitled()) {
        return;
    }

    QString reason = encoding.has_bom                 ? QStringLiteral("Read from its byte order mark")
                   : encoding.name.startsWith("UTF-") ? QStringLiteral("The file is valid UTF-8")
                                                      : QStringLiteral("Not valid UTF-8, so read byte for byte");
    menu->addSeparator();
    menu->addAction(reason)->setEnabled(false);
}

void editor_window::fill_line_ending_menu(document* doc, QMenu* menu) {
    const file_record& file = doc->file();
    if (file.has_mixed_eols) {
        QStringList counts;
        for (const auto& [count, label] : { std::pair { file.eols.lf, "LF" }, std::pair { file.eols.crlf, "CRLF" },
                                            std::pair { file.eols.cr, "CR" } }) {
            if (count > 0) {
                counts.append(QStringLiteral("%1 %2").arg(count).arg(QString::fromLatin1(label)));
            }
        }

        menu->addAction(QStringLiteral("Mixed: %1").arg(counts.join(QStringLiteral(", "))))->setEnabled(false);
        menu->addSeparator();
    }

    for (const eol_choice& choice : EOL_CHOICES) {
        QAction* action = menu->addAction(QString::fromLatin1(choice.label), [this, doc, choice] {
            doc->convert_eols(choice.mode);
            refresh_status();
        });
        action->setCheckable(true);
        action->setChecked(choice.mode == file.eol_mode && !file.has_mixed_eols);
    }

    menu->addSeparator();
    ScintillaEditBase* view = doc->view();
    QAction* shown = menu->addAction(QStringLiteral("Show Line Endings"), [view](bool is_shown) {
        view->send(SCI_SETVIEWEOL, is_shown);
    });
    shown->setCheckable(true);
    shown->setChecked(view->send(SCI_GETVIEWEOL) != 0);
}

// The text's own menu for the pointer's place or the caret's: undo, the clipboard and the selection
void editor_window::show_text_menu(const QPoint& anchor) {
    document* doc = current_document();
    if (!doc || !doc->view()->isVisible() || m_open_sheet) {
        return;
    }

    ScintillaEditBase* view = doc->view();
    QMenu menu(this);
    menu.setStyle(m_menu_style);
    menu.addAction(m_actions.value(command::UNDO));
    menu.addAction(m_actions.value(command::REDO));
    menu.addSeparator();

    for (command id : { command::CUT, command::COPY, command::PASTE }) {
        menu.addAction(m_actions.value(id));
    }

    QAction* erase = menu.addAction(QStringLiteral("Delete\tDel"), [view] { view->send(SCI_CLEAR); });
    erase->setEnabled(!view->send(SCI_GETSELECTIONEMPTY));
    menu.addSeparator();
    menu.addAction(m_actions.value(command::SELECT_ALL));
    menu.addSeparator();
    menu.addAction(m_actions.value(command::TOGGLE_COMMENT));

    QString find_keys = m_actions.value(command::FIND)->shortcut().toString(QKeySequence::NativeText);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Find Selection\t") + find_keys, [this] { run(command::FIND); });
    menu.exec(anchor);
}

void editor_window::show_tab_menu(int index, const QPoint& anchor) {
    if (m_open_sheet) {
        return;
    }

    document* doc = m_documents.at(index).get();
    QMenu menu(this);
    menu.setStyle(m_menu_style);
    menu.addAction(QStringLiteral("Close\tCtrl+W"), [this, doc] { close_document(index_of(doc)); });

    menu.addAction(QStringLiteral("Close Others"), [this, doc] {
        QList<document*> others;
        for (const std::unique_ptr<document>& open : m_documents) {
            if (open.get() != doc) {
                others.append(open.get());
            }
        }

        close_documents(others, QStringLiteral("closing"));
    });

    menu.addAction(QStringLiteral("Close to the Right"), [this, doc] {
        QList<document*> right;
        for (size_t open = index_of(doc) + 1; open < m_documents.size(); open++) {
            right.append(m_documents[open].get());
        }

        close_documents(right, QStringLiteral("closing"));
    });

    menu.addAction(QStringLiteral("Close Saved"), [this] { close_saved_tabs(); });

    menu.addSeparator();
    QAction* copy_path = menu.addAction(QStringLiteral("Copy Path"), [doc] {
        QGuiApplication::clipboard()->setText(doc->file().path);
    });
    copy_path->setEnabled(!doc->is_untitled());
    menu.addAction(QStringLiteral("Copy Name"), [doc] { QGuiApplication::clipboard()->setText(doc->display_name()); });

    menu.addSeparator();
    QAction* reopen = menu.addAction(QStringLiteral("Reopen Closed Tab\tCtrl+Shift+T"),
                                     [this] { reopen_closed_tab(); });
    reopen->setEnabled(!m_closed_tabs.isEmpty());
    menu.exec(anchor);
}

document* editor_window::settings_document() const {
    for (const std::unique_ptr<document>& open : m_documents) {
        if (open->is_page()) {
            return open.get();
        }
    }

    return nullptr;
}

// Keeps where the caret stood before a jump, for Back, and a new jump drops the way Forward
void editor_window::record_jump(document* doc) {
    if (m_is_stepping_jumps || !doc || doc->placeholder()) {
        return;
    }

    caret_place here = caret_place_of(doc);
    if (m_back_places.isEmpty() || m_back_places.last() != here) {
        m_back_places.append(here);
    }

    if (m_back_places.size() > JUMP_LIMIT) {
        m_back_places.removeFirst();
    }

    m_forward_places.clear();
}

// Each step keeps where the caret was for the other direction, passing places it already stands at
void editor_window::step_jump(bool is_back) {
    QList<caret_place>& from = is_back ? m_back_places : m_forward_places;
    QList<caret_place>& to = is_back ? m_forward_places : m_back_places;
    document* doc = current_document();
    bool has_caret = doc && !doc->placeholder();
    while (has_caret && !from.isEmpty() && from.last() == caret_place_of(doc)) {
        from.removeLast();
    }

    if (from.isEmpty()) {
        return;
    }

    caret_place target = from.takeLast();
    if (has_caret) {
        to.append(caret_place_of(doc));
    }

    m_is_stepping_jumps = true;
    select_tab(index_of(target.doc));
    m_is_stepping_jumps = false;

    ScintillaEditBase* view = target.doc->view();
    sptr_t line = qMin(target.line, view->send(SCI_GETLINECOUNT) - 1);
    view->send(SCI_GOTOPOS, view->send(SCI_FINDCOLUMN, line, target.column));
    view->setFocus();
}

// Ctrl Comma opens the Settings tab or focuses the one already open, turned to `section` such as Shortcuts if named
void editor_window::open_settings(const QString& section) {
    document* doc = settings_document();
    if (!doc) {
        auto owned = std::make_unique<document>(m_stack, lowest_free_untitled_number(), m_settings);
        auto* page = new settings_page(m_settings, m_stack);
        m_stack->addWidget(page);
        owned->show_page(page, QStringLiteral("Settings"));
        doc = add_document(std::move(owned), insertion_index());

        page->on_changed = [this, doc, page] {
            doc->set_page_modified(!page->changes().isEmpty());
            refresh_tab(doc);
            refresh_commands();
        };

        page->on_save = [this] { save_settings(); };
        page->on_open_as_text = [this] { open_path(QString::fromLatin1(SETTINGS_PATH), QByteArray(), insertion_index()); };
        show_settings_problems(doc);
    }

    select_tab(index_of(doc));
    if (!section.isEmpty()) {
        static_cast<settings_page*>(doc->placeholder())->show_section(section);
    }
}

// Only the changed keys' lines are rewritten, then the file is read again so every tab takes what it now says
bool editor_window::save_settings() {
    document* doc = settings_document();
    auto* page = static_cast<settings_page*>(doc->placeholder());
    if (page->changes().isEmpty()) {
        return true;
    }

    QString error;
    if (!write_settings(QString::fromLatin1(SETTINGS_PATH), page->changes(), &error)) {
        show_banner(doc, { banner_kind::FAILURE, QStringLiteral("Couldn't save sheen.conf"),
                           QStringLiteral(": %1. The file on disk is unchanged.").arg(error.toLower()), {} });
        return false;
    }

    reread_settings();
    return true;
}

// sheen.conf saved from its text applies at once, unless the Settings tab holds changes of its own and asks first
void editor_window::take_settings_from_text() {
    document* doc = settings_document();
    auto* page = doc ? static_cast<settings_page*>(doc->placeholder()) : nullptr;
    if (!page || page->changes().isEmpty()) {
        reread_settings();
        return;
    }

    show_banner(doc, { banner_kind::CHANGE, QStringLiteral("sheen.conf"),
                       QStringLiteral(" changed on disk. Your unsaved settings are kept."),
                       { { QStringLiteral("Reload"), [this] { reread_settings(); } },
                         { QStringLiteral("Keep Mine"), [this, doc] {
                              doc->set_banner(std::nullopt);
                              refresh_banner();
                          } } } });
}

// Every tab, the status bar and the Settings tab take what sheen.conf now says, dropping the page's changes
void editor_window::reread_settings() {
    sheen_settings saved;
    m_settings_problems = read_settings(QString::fromLatin1(SETTINGS_PATH), &saved);
    m_status_note = problems_note(m_settings_problems);
    apply_settings(saved);

    document* doc = settings_document();
    if (!doc) {
        return;
    }

    static_cast<settings_page*>(doc->placeholder())->reset(m_settings);
    doc->set_banner(std::nullopt);
    show_settings_problems(doc);
    refresh_banner();
}

void editor_window::apply_settings(const sheen_settings& settings) {
    m_settings = settings;
    set_text_size(m_settings.font_size);
    for (const std::unique_ptr<document>& open : m_documents) {
        if (!open->is_page()) {
            open->apply_settings();
        }
    }

    refresh_status();
}

// The first problem sheen.conf has, such as an unknown key, with Open as Text going to its line
void editor_window::show_settings_problems(document* doc) {
    if (m_settings_problems.isEmpty()) {
        return;
    }

    QString problem = m_settings_problems.first();
    QString place = problem.section(QLatin1Char(' '), 0, 0);
    int line = place.section(QLatin1Char(':'), 1, 1).toInt();
    QString rest = QStringLiteral(" %1, the default is used.").arg(problem.section(QLatin1Char(' '), 1));
    show_banner(doc, { banner_kind::FAILURE, place, rest, { { QStringLiteral("Open as Text"), [this, line] {
                                                               QString path = QString::fromLatin1(SETTINGS_PATH);
                                                               open_path(path, QByteArray(), insertion_index());
                                                               if (document* text = current_document()) {
                                                                   text->view()->send(SCI_GOTOLINE, line - 1);
                                                               }
                                                           } } } });
}

// Rereading from disk drops unsaved edits, so they get the usual question first
// `>` lists the actions and `@` the open tabs and this session's files, and a query starting with `:` goes to line
void editor_window::open_palette(const QString& query) {
    if (m_open_sheet) {
        return;
    }

    m_picker->on_accepted = nullptr;
    m_picker->on_changed = [this](const QString& typed) { fill_palette(typed); };
    m_picker->on_chosen = [this](int row) {
        if (row < m_palette_targets.size()) {
            std::function<void()> target = m_palette_targets.at(row);
            target();
        }
    };

    m_picker->on_cancelled = [this] { focus_current_document(); };

    QString gap = QStringLiteral("&nbsp;&nbsp;&nbsp;");
    QString help = picker::code(QStringLiteral(">")) + QStringLiteral(" actions") + gap + picker::code(QStringLiteral(":"))
                 + QStringLiteral(" go to line") + gap + picker::code(QStringLiteral("@")) + QStringLiteral(" open tab");
    m_picker->open_list(QString(), {}, query, help, QStringLiteral("Enter runs, Esc closes"));
}

void editor_window::fill_palette(const QString& query) {
    if (query.startsWith(QLatin1Char(':'))) {
        m_picker->set_rows({});
        go_to_line(true);
        return;
    }

    bool is_actions = query.startsWith(QLatin1Char('>'));
    bool has_prefix = is_actions || query.startsWith(QLatin1Char('@'));
    QList<picker_row> rows;
    QList<std::function<void()>> targets;
    if (is_actions) {
        list_palette_actions(&rows, &targets);
    } else {
        list_palette_files(&rows, &targets);
    }

    QList<picker_row> shown;
    m_palette_targets.clear();
    for (const auto& [index, row] : rank_rows(rows, (has_prefix ? query.mid(1) : query).trimmed())) {
        shown.append(row);
        m_palette_targets.append(targets.at(index));
    }

    m_picker->set_rows(shown);
}

// Every action that can run now, under its menu's name with its keys, the ones chosen here before first
void editor_window::list_palette_actions(QList<picker_row>* rows, QList<std::function<void()>>* targets) {
    QList<const command_entry*> entries;
    for (command id : m_recent_commands) {
        for (const command_entry& entry : COMMANDS) {
            if (entry.id == id) {
                entries.append(&entry);
            }
        }
    }

    for (const command_entry& entry : COMMANDS) {
        if (!m_recent_commands.contains(entry.id)) {
            entries.append(&entry);
        }
    }

    for (const command_entry* entry : entries) {
        QAction* action = m_actions.value(entry->id);
        bool is_listed = entry->kind != command_kind::SUBMENU && entry->id != command::COMMAND_PALETTE;
        if (!is_listed || !action->isEnabled()) {
            continue;
        }

        bool is_recent = m_recent_commands.contains(entry->id);
        QString section = m_recent_commands.isEmpty() ? QString()
                        : is_recent                   ? QStringLiteral("Recently used")
                                                      : QStringLiteral("All actions");
        QString detail = palette_category(*entry) + QStringLiteral(": ");
        rows->append({ QString::fromLatin1(entry->label), section, false, detail, action_keys(action), {}, {} });

        command id = entry->id;
        targets->append([this, id] {
            m_recent_commands.removeAll(id);
            m_recent_commands.prepend(id);
            while (m_recent_commands.size() > RECENT_COMMAND_LIMIT) {
                m_recent_commands.removeLast();
            }

            m_actions.value(id)->trigger();
        });
    }
}

// The open tabs in strip order, then this session's files that are no longer open, each with its folder
void editor_window::list_palette_files(QList<picker_row>* rows, QList<std::function<void()>>* targets) {
    QStringList open_paths_now;
    for (const std::unique_ptr<document>& open : m_documents) {
        document* doc = open.get();
        QString folder = doc->is_untitled() ? QStringLiteral("never saved") : QFileInfo(doc->file().path).absolutePath();
        rows->append({ doc->display_name(), QStringLiteral("Open tabs"), false, {}, {}, folder, {} });
        targets->append([this, doc] { select_tab(index_of(doc)); });
        open_paths_now.append(doc->file().path);
    }

    for (const QString& path : m_recent_files) {
        if (open_paths_now.contains(path)) {
            continue;
        }

        QFileInfo file(path);
        rows->append({ file.fileName(), QStringLiteral("Closed this session"), false, {}, {}, file.absolutePath(), {} });
        targets->append([this, path] { open_paths({ path }); });
    }
}

// Every encoding Sheen reads and writes, those chosen this session first. Saving offers UTF-8 with a BOM as well
void editor_window::pick_encoding(command id) {
    document* doc = current_document();
    if (!doc || doc->placeholder() || m_open_sheet) {
        return;
    }

    bool is_save = id == command::SAVE_WITH_ENCODING;
    QList<std::pair<QString, file_encoding>> choices;
    for (const QByteArray& name : available_encodings()) {
        file_encoding encoding = is_save ? encoding_for_save(name) : file_encoding { name, false };
        choices.append({ encoding_label(encoding), encoding });
        if (is_save && name == UTF_8) {
            choices.append({ encoding_label(file_encoding { UTF_8, true }), file_encoding { UTF_8, true } });
        }
    }

    QList<std::pair<QString, file_encoding>> ordered;
    for (const QString& label : m_recent_encodings) {
        for (const auto& choice : choices) {
            if (choice.first == label) {
                ordered.append(choice);
            }
        }
    }

    for (const auto& choice : choices) {
        if (!m_recent_encodings.contains(choice.first)) {
            ordered.append(choice);
        }
    }

    QString current = encoding_label(doc->file().encoding);
    QList<picker_row> rows;
    for (const auto& [label, encoding] : ordered) {
        bool is_recent = m_recent_encodings.contains(label);
        QString section = m_recent_encodings.isEmpty() ? QString()
                        : is_recent                    ? QStringLiteral("Recently used")
                                                       : QStringLiteral("All encodings");
        rows.append({ label, section, label == current, {}, {}, {}, {} });
    }

    QPointer<ScintillaEditBase> view = doc->view();
    m_picker->on_changed = nullptr;
    m_picker->on_accepted = nullptr;
    m_picker->on_cancelled = [view] {
        if (view) {
            view->setFocus();
        }
    };

    m_picker->on_chosen = [this, is_save, ordered](int row) {
        const auto& [label, encoding] = ordered.at(row);
        m_recent_encodings.removeAll(label);
        m_recent_encodings.prepend(label);
        is_save ? save_with_encoding(encoding) : reopen_with_encoding(encoding.name);
    };

    QString placeholder = is_save ? QStringLiteral("Save with encoding, %1 known")
                                  : QStringLiteral("Reopen with encoding, %1 known");
    m_picker->open_list(placeholder.arg(ordered.size()), rows);
}

void editor_window::reopen_with_encoding(const QByteArray& name) {
    document* doc = current_document();
    if (doc->is_modified()) {
        prompt_answer answer = ask_about_unsaved(doc, QStringLiteral("reopening"));
        if (answer == prompt_answer::CANCEL || (answer == prompt_answer::PRIMARY && !save_document(doc))) {
            return;
        }
    }

    QString error;
    if (!doc->reload_as(name, &error)) {
        QString reason = error + QStringLiteral(", so it was kept as it was.");
        show_banner(doc, { banner_kind::FAILURE, doc->display_name(), reason, {} });
        return;
    }

    doc->set_banner(std::nullopt);
    refresh_tab(doc);
    refresh_banner();
    refresh_status();
}

// The file keeps its old encoding when the save fails, such as when the new one cannot hold every line
void editor_window::save_with_encoding(const file_encoding& encoding) {
    document* doc = current_document();
    file_encoding previous = doc->file().encoding;
    doc->set_encoding(encoding);
    if (doc->is_untitled() ? !save_document_as(doc) : !write_document(doc, doc->file().path)) {
        doc->set_encoding(previous);
    }

    refresh_status();
}

void editor_window::show_about() {
    prompt_sheet about(this, QStringLiteral("Sheen"),
                       QStringLiteral("The Stellux text editor, built on Scintilla and Lexilla with Qt. The license "
                                      "texts are in /usr/share/licenses/sheen, and "
                                      "/usr/share/doc/sheen/BUILDINFO names every version."),
                       QStringLiteral("Close"), QString(), false);
    run_sheet(about);
}

// The current file's folder, else the folder of the most recent tab with one, else the last a sheet showed
QString editor_window::sheet_folder() const {
    for (document* doc : m_recent_tabs) {
        if (!doc->is_untitled()) {
            return QFileInfo(doc->file().path).absolutePath();
        }
    }

    return m_last_sheet_folder.isEmpty() ? QDir::rootPath() : m_last_sheet_folder;
}
