#include "editor_window.h"
#include "banner.h"
#include "chrome.h"
#include "empty_state.h"
#include "file_loader.h"
#include "file_sheet.h"
#include "find_bar.h"
#include "kiln.h"
#include "menu_style.h"
#include "picker.h"
#include "placeholder.h"
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
#include <QScreen>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <ScintillaEditBase.h>

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

// Wide enough that each menu's chevron stands apart from its name, with room for the keycaps under the hairline
static constexpr int CASCADE_WIDTH = 272;
static constexpr int STATUS_NOTE_MS = 4000;

static constexpr int SWITCHER_GAP = 8;
static constexpr int GOTO_MARKER = 1;
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
    command::DELETE_LINE, command::DUPLICATE, command::MOVE_LINES_UP, command::MOVE_LINES_DOWN,
    command::INSERT_LINE_BELOW, command::INSERT_LINE_ABOVE, command::INDENT, command::OUTDENT, command::UPPER_CASE,
    command::LOWER_CASE, command::SELECT_ALL, command::SELECT_LINE, command::ADD_NEXT_MATCH,
    command::SELECT_ALL_MATCHES, command::FIND, command::REPLACE, command::FIND_NEXT, command::FIND_PREVIOUS,
    command::FIND_WORD, command::GO_TO_LINE, command::CODE_MODE, command::TEXT_MODE, command::LANGUAGE, command::WORD_WRAP,
    command::LINE_NUMBERS, command::INDENT_GUIDES, command::COLUMN_EDGE, command::WHITESPACE, command::MATCHING_BRACKET,
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

// The empty state's glyph for each of its actions, as mock 09 draws them
static row_glyph empty_glyph(command id) {
    switch (id) {
        case command::OPEN_FILE:
            return row_glyph::FOLDER;
        case command::REOPEN_CLOSED_TAB:
            return row_glyph::CLOCK;
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

    // The menu bar has no row of its own for Settings, so File holds it above Quit
    if (m_settings.has_menu_bar) {
        QMenu* file = m_menus.at(static_cast<int>(menu::FILE));
        QAction* quit = m_actions.value(command::QUIT);
        file->insertAction(quit, m_actions.value(command::SETTINGS));
        file->insertSeparator(quit);
        for (QMenu* submenu : m_menus) {
            if (!submenu->isEmpty()) {
                menuBar()->addMenu(submenu);
            }
        }

        m_tab_row->menu()->hide();
    }

    if (!problems.isEmpty()) {
        QString more = QStringLiteral("%1 and %2 more").arg(problems.first()).arg(problems.size() - 1);
        m_status_note = problems.size() == 1 ? problems.first() : more;
    }

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
        } else {
            connect(action, &QAction::triggered, this, [this, id] { run(id); });
            addAction(action);
        }

        m_actions.insert(id, action);
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

    if (id == command::SETTINGS) {
        open_path(QString::fromLatin1(SETTINGS_PATH), QByteArray(), insertion_index());
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
        case command::SELECT_ALL:
            view->send(SCI_SELECTALL);
            break;
        case command::SELECT_LINE:
            select_line(view);
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
            go_to_line();
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
        case command::ZOOM_IN:
            view->send(SCI_ZOOMIN);
            break;
        case command::ZOOM_OUT:
            view->send(SCI_ZOOMOUT);
            break;
        case command::ZOOM_RESET:
            view->send(SCI_SETZOOM, 0);
            break;
        case command::MATCHING_BRACKET:
            go_to_matching_bracket(view);
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

    m_actions.value(command::REOPEN_CLOSED_TAB)->setEnabled(is_free && !m_closed_tabs.isEmpty());
    m_actions.value(command::REVERT)->setEnabled(is_free && doc && !doc->is_untitled() && doc->is_modified());
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
    doc ? doc->view()->setFocus() : m_empty_state->setFocus();
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

        if (doc == current_document()) {
            refresh_status();
        }
    });

    connect(view, &ScintillaEditBase::resized, this, [doc] {
        doc->fit_text_width();
        doc->refresh_function_names();
    });

    connect(view, &ScintillaEditBase::zoom, this, [view] { fit_line_numbers(view); });

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

int editor_window::lowest_free_untitled_number() const {
    int number = 1;
    bool is_taken = true;
    while (is_taken) {
        is_taken = false;
        for (const std::unique_ptr<document>& doc : m_documents) {
            if (doc->is_untitled() && doc->untitled_number() == number) {
                is_taken = true;
                number++;
                break;
            }
        }
    }

    return number;
}

void editor_window::remember_recent_file(const QString& path) {
    m_recent_files.removeAll(path);
    m_recent_files.prepend(path);
    while (m_recent_files.size() > RECENT_FILE_LIMIT) {
        m_recent_files.removeLast();
    }
}

void editor_window::new_document() {
    add_document(std::make_unique<document>(m_stack, lowest_free_untitled_number(), m_settings), insertion_index());
}

void editor_window::choose_files_to_open(const QString& folder) {
    document* current = current_document();
    QString current_path = current ? current->file().path : QString();

    file_sheet chooser(this, file_sheet_mode::OPEN, folder, m_recent_files);
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
    file_sheet chooser(this, file_sheet_mode::SAVE_AS, folder, m_recent_files);
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
            QString place = doc->is_untitled() ? QStringLiteral("never saved, Save asks where")
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
    QString body = doc->is_untitled() ? QStringLiteral("It has never been saved.")
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
    if (!doc->is_untitled() || closed.scintilla_document) {
        m_closed_tabs.prepend(closed);
    }

    while (m_closed_tabs.size() > CLOSED_TAB_LIMIT) {
        closed_tab dropped = m_closed_tabs.takeLast();
        if (dropped.scintilla_document) {
            doc->view()->send(SCI_RELEASEDOCUMENT, 0, dropped.scintilla_document);
        }
    }

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

    auto doc = std::make_unique<document>(m_stack, closed.untitled_number, m_settings);
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

    QList<empty_action> actions;
    for (const auto& [id, label] : rows) {
        QString keys = m_actions.value(id)->shortcut().toString(QKeySequence::NativeText);
        actions.append({ label, keys.split(QLatin1Char('+')), [this, id] { run(id); }, empty_glyph(id) });
    }

    m_empty_state->set_entries(actions, m_recent_files);
    m_stack->setCurrentWidget(m_empty_state);
    m_empty_state->setFocus();
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

// Watches the whole application only while Ctrl Tab is held, for Ctrl coming up, Esc or a click
bool editor_window::eventFilter(QObject* watched, QEvent* event) {
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

    m_recent_tabs.removeAll(doc);
    m_recent_tabs.prepend(doc);

    QWidget* shown = doc->placeholder() ? doc->placeholder() : doc->view();
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
    tabs()->setTabToolTip(index, doc->is_untitled() ? QStringLiteral("Never saved")
                                 : doc->is_deleted() ? QStringLiteral("Deleted on disk")
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
void editor_window::go_to_line() {
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

    m_picker->on_changed = [this, view, lines, help](const QString& query) {
        sptr_t line = 0;
        sptr_t column = 0;
        if (!view) {
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

    m_picker->on_accepted = [view, lines](const QString& query) {
        sptr_t line = 0;
        sptr_t column = 0;
        if (!view) {
            return;
        }

        view->send(SCI_MARKERDELETEALL, GOTO_MARKER);
        if (read_line_target(query, &line, &column)) {
            view->send(SCI_GOTOPOS, view->send(SCI_FINDCOLUMN, qMin(line, lines) - 1, column - 1));
        }

        view->setFocus();
    };

    m_picker->on_cancelled = [view, before] {
        if (!view) {
            return;
        }

        view->send(SCI_MARKERDELETEALL, GOTO_MARKER);
        view->send(SCI_SETSEL, before.anchor, before.caret);
        view->send(SCI_SETFIRSTVISIBLELINE, before.first_line);
        view->send(SCI_SETXOFFSET, before.x_offset);
        view->setFocus();
    };

    m_picker->open(QStringLiteral(":"), help, aside);
}

// Each status button's menu opens upward from it, and the button shows as open meanwhile
void editor_window::show_status_menu(status_kind kind, const QPoint& anchor) {
    document* doc = current_document();
    if (!doc || m_open_sheet) {
        return;
    }

    if (kind == status_kind::POSITION) {
        go_to_line();
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

// Rereading from disk drops unsaved edits, so they get the usual question first
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
        rows.append({ label, section, label == current });
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
