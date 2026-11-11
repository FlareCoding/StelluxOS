#include "editor_window.h"
#include "banner.h"
#include "empty_state.h"
#include "file_sheet.h"
#include "kiln.h"
#include "menu_style.h"
#include "tab_strip.h"

#include <QAction>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QScreen>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <ScintillaEditBase.h>

struct eol_choice {
    int mode;
    const char* label;
};

static constexpr int WINDOW_W = 1520;
static constexpr int WINDOW_H = 920;
static constexpr int WINDOW_MIN_W = 720;
static constexpr int WINDOW_MIN_H = 480;
static constexpr int RECENT_FILE_LIMIT = 20;
static constexpr int STATUS_NOTE_MS = 4000;

// Wide enough that each menu's chevron stands apart from its name, with room for the keycaps under the hairline
static constexpr int CASCADE_WIDTH = 272;

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
    command::SELECT_ALL_MATCHES, command::LANGUAGE, command::WORD_WRAP, command::LINE_NUMBERS,
    command::INDENT_GUIDES, command::COLUMN_EDGE, command::WHITESPACE, command::MATCHING_BRACKET,
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

editor_window::editor_window()
    : m_tab_row(new tab_row(this)), m_banner(new banner(this)), m_stack(new QStackedWidget(this)),
      m_empty_state(new empty_state(this)), m_status(new status_bar(this)), m_menu_style(new menu_style),
      m_main_menu(new QMenu(this)) {
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    layout->addWidget(m_tab_row);
    layout->addWidget(m_banner);
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
    m_status->on_fact_clicked = [this](file_fact fact, const QPoint& anchor) { show_fact_menu(fact, anchor); };
    m_banner->on_dismissed = [this] {
        if (document* doc = current_document()) {
            doc->accept_disk_state();
            doc->set_banner(std::nullopt);
            refresh_banner();
        }
    };

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

    if (!folder.isEmpty()) {
        QTimer::singleShot(0, this, [this, folder] { choose_files_to_open(folder); });
    }
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

// Scintilla leaves Esc to the window, which gives back one caret, else dismisses a change banner
void editor_window::keyPressEvent(QKeyEvent* event) {
    document* doc = current_document();
    if (event->key() != Qt::Key_Escape || event->modifiers() != Qt::NoModifier || !doc) {
        QMainWindow::keyPressEvent(event);
        return;
    }

    if (doc->view()->send(SCI_GETSELECTIONS) > 1) {
        doc->view()->send(SCI_CANCEL);
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

    m_main_menu->setStyle(m_menu_style);
    m_main_menu->setMinimumWidth(CASCADE_WIDTH);
    for (QMenu* submenu : m_menus) {
        if (!submenu->isEmpty()) {
            m_main_menu->addMenu(submenu);
        }
    }

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
        case command::REOPEN_WITH_ENCODING:
        case command::SAVE_WITH_ENCODING:
            for (const QByteArray& name : available_encodings()) {
                QAction* action = submenu->addAction(encoding_label(name), [this, id, name] {
                    id == command::REOPEN_WITH_ENCODING ? reopen_with_encoding(name) : save_with_encoding(name);
                });
                action->setCheckable(true);
                action->setChecked(doc && doc->file().encoding.name == name);
            }

            break;
        case command::LANGUAGE: {
            QAction* plain = submenu->addAction(language_name(nullptr), [this] {
                current_document()->set_language(nullptr);
                refresh_status();
            });
            plain->setCheckable(true);
            plain->setChecked(doc && !doc->lang());
            for (const language* lang : all_languages()) {
                QAction* action = submenu->addAction(language_name(lang), [this, lang] {
                    current_document()->set_language(lang);
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

    if (id == command::QUIT) {
        close();
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
        case command::SELECT_ALL_MATCHES:
            view->send(SCI_TARGETWHOLEDOCUMENT);
            view->send(SCI_SETSEARCHFLAGS, SCFIND_MATCHCASE);
            view->send(SCI_MULTIPLESELECTADDEACH);
            break;
        case command::WORD_WRAP:
            view->send(SCI_SETWRAPMODE, is_checked ? SC_WRAP_WORD : SC_WRAP_NONE);
            break;
        case command::LINE_NUMBERS:
            set_line_numbers_shown(view, is_checked);
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
            if (m_recent_tabs.size() > 1) {
                select_tab(index_of(m_recent_tabs.at(1)));
            }

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

    for (command id : DOCUMENT_COMMANDS) {
        m_actions.value(id)->setEnabled(is_free && doc);
    }

    m_actions.value(command::REVERT)->setEnabled(is_free && doc && !doc->is_untitled() && doc->is_modified());
    m_tab_row->menu()->setEnabled(is_free);
    if (!doc) {
        return;
    }

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
    });

    connect(view, &ScintillaEditBase::updateUi, this, [this, doc] {
        if (doc == current_document()) {
            refresh_status();
        }
    });

    connect(view, &ScintillaEditBase::zoom, this, [view] { fit_line_numbers(view); });

    {
        QSignalBlocker blocker(tabs());
        tabs()->insertTab(index, doc->display_name());
    }

    refresh_tab(doc);
    select_tab(index);
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
int editor_window::insertion_index() const {
    return tabs()->currentIndex() + 1;
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
    add_document(std::make_unique<document>(m_stack, lowest_free_untitled_number()), insertion_index());
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
        select_tab(index_of(open));
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

        if (looks_binary(bytes)) {
            prompt_sheet question(this, QStringLiteral("%1 looks binary").arg(info.fileName()),
                                  QStringLiteral("It has NUL bytes in its first 64 KB. As text it shows them as marks, "
                                                 "and saving keeps every byte."),
                                  QStringLiteral("Open as Text"), QString(), false);
            if (static_cast<prompt_answer>(run_sheet(question)) != prompt_answer::PRIMARY) {
                return false;
            }
        }
    }

    document* pristine = current && current->is_pristine() ? current : nullptr;
    auto doc = std::make_unique<document>(m_stack, lowest_free_untitled_number());
    if (!info.exists()) {
        doc->set_new_path(absolute);
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
        remove_document(index_of(pristine));
    }

    return true;
}

// A clean tab whose file is on disk has nothing to write, and a file changed elsewhere asks before it is replaced
bool editor_window::save_document(document* doc) {
    if (doc->is_untitled()) {
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
                             { QStringLiteral("Save as UTF-8"), [this] { save_with_encoding(UTF_8); } } } });
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
        if (doc->is_untitled()) {
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
        remove_document(index);
        return true;
    }

    prompt_answer answer = ask_about_unsaved(doc, QStringLiteral("closing"));
    if (answer == prompt_answer::CANCEL || (answer == prompt_answer::PRIMARY && !save_document(doc))) {
        return false;
    }

    remove_document(index_of(doc));
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
            remove_document(index_of(doc));
        }
    }

    if (unsaved.size() == 1) {
        prompt_answer answer = ask_about_unsaved(unsaved.first(), purpose);
        if (answer == prompt_answer::CANCEL || (answer == prompt_answer::PRIMARY && !save_document(unsaved.first()))) {
            return false;
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
            if (answer == prompt_answer::PRIMARY && checked.at(index)) {
                to_save.append(unsaved.at(index));
            }
        }

        if (!save_documents(to_save, purpose)) {
            return false;
        }
    }

    if (is_quitting) {
        return true;
    }

    for (document* doc : unsaved) {
        remove_document(index_of(doc));
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

void editor_window::remove_document(int index) {
    document* doc = m_documents.at(index).get();

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

    if (m_documents.empty()) {
        show_empty_state();
        setWindowTitle(QStringLiteral("Sheen"));
        refresh_banner();
        refresh_status();
    }

    refresh_commands();
}

// A clean tab follows its file quietly, an unsaved one keeps the user's text and says what happened
void editor_window::check_disk(document* doc) {
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

void editor_window::show_current_document() {
    document* doc = current_document();
    if (!doc) {
        return;
    }

    m_recent_tabs.removeAll(doc);
    m_recent_tabs.prepend(doc);
    m_stack->setCurrentWidget(doc->view());
    if (!m_open_sheet) {
        doc->view()->setFocus();
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

    tabs()->setTabText(index, doc->display_name());
    tabs()->setTabToolTip(index, doc->is_untitled() ? QStringLiteral("Never saved")
                                 : doc->is_deleted() ? QStringLiteral("Deleted on disk")
                                                     : doc->file().path);
    tabs()->set_tab_modified(index, doc->is_modified());
    tabs()->updateGeometry();
    if (doc == current_document()) {
        setWindowTitle(doc->display_name());
    }
}

void editor_window::refresh_status() {
    document* doc = current_document();
    if (!doc) {
        m_status->set_caret_parts({});
        m_status->set_facts({});
        m_status->set_message(QString());
        return;
    }

    ScintillaEditBase* view = doc->view();
    sptr_t caret = view->send(SCI_GETCURRENTPOS);
    sptr_t line = view->send(SCI_LINEFROMPOSITION, caret) + 1;
    sptr_t column = view->send(SCI_GETCOLUMN, caret) + 1;
    QStringList parts = { QStringLiteral("Ln %1, Col %2").arg(line).arg(column) };

    sptr_t selections = view->send(SCI_GETSELECTIONS);
    sptr_t start = view->send(SCI_GETSELECTIONSTART);
    sptr_t end = view->send(SCI_GETSELECTIONEND);
    if (selections > 1) {
        parts.append(QStringLiteral("%1 carets").arg(selections));
    } else if (end > start) {
        parts.append(QStringLiteral("%1 selected").arg(view->send(SCI_COUNTCHARACTERS, start, end)));
    }

    bool uses_tabs = view->send(SCI_GETUSETABS) != 0;
    parts.append(uses_tabs ? QStringLiteral("Tabs: %1").arg(view->send(SCI_GETTABWIDTH))
                           : QStringLiteral("Spaces: %1").arg(view->send(SCI_GETINDENT)));
    if (view->send(SCI_GETOVERTYPE)) {
        parts.append(QStringLiteral("OVR"));
    }

    m_status->set_caret_parts(parts);

    const file_record& file = doc->file();
    QString eol = eol_label(file.eol_mode);
    if (file.has_mixed_eols) {
        eol = QStringLiteral("%1, mixed").arg(eol);
    }

    m_status->set_message(m_status_note);
    m_status->set_facts({ language_name(doc->lang()), encoding_label(file.encoding), eol });
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

void editor_window::show_fact_menu(file_fact fact, const QPoint& anchor) {
    document* doc = current_document();
    if (!doc || m_open_sheet) {
        return;
    }

    QMenu menu(this);
    menu.setStyle(m_menu_style);
    switch (fact) {
        case file_fact::LANGUAGE:
            fill_submenu(command::LANGUAGE, &menu);
            break;
        case file_fact::ENCODING: {
            QMenu* reopen = menu.addMenu(QStringLiteral("Reopen with Encoding"));
            reopen->setStyle(m_menu_style);
            reopen->setEnabled(!doc->is_untitled() && QFileInfo(doc->file().path).isFile());
            fill_submenu(command::REOPEN_WITH_ENCODING, reopen);

            QMenu* save = menu.addMenu(QStringLiteral("Save with Encoding"));
            save->setStyle(m_menu_style);
            fill_submenu(command::SAVE_WITH_ENCODING, save);
            break;
        }
        case file_fact::LINE_ENDING:
            for (const eol_choice& choice : EOL_CHOICES) {
                QAction* action = menu.addAction(QString::fromLatin1(choice.label), [this, doc, choice] {
                    doc->convert_eols(choice.mode);
                    refresh_status();
                });
                action->setCheckable(true);
                action->setChecked(choice.mode == doc->file().eol_mode && !doc->file().has_mixed_eols);
            }

            break;
        case file_fact::COUNT:
            return;
    }

    menu.exec(anchor - QPoint(0, menu.sizeHint().height()));
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

    menu.addAction(QStringLiteral("Close Saved"), [this] {
        QList<document*> saved;
        for (const std::unique_ptr<document>& open : m_documents) {
            if (!open->is_modified()) {
                saved.append(open.get());
            }
        }

        close_documents(saved, QStringLiteral("closing"));
    });

    menu.addSeparator();
    QAction* copy_path = menu.addAction(QStringLiteral("Copy Path"), [doc] {
        QGuiApplication::clipboard()->setText(doc->file().path);
    });
    copy_path->setEnabled(!doc->is_untitled());
    menu.addAction(QStringLiteral("Copy Name"), [doc] { QGuiApplication::clipboard()->setText(doc->display_name()); });
    menu.exec(anchor);
}

// Rereading from disk drops unsaved edits, so they get the usual question first
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
void editor_window::save_with_encoding(const QByteArray& name) {
    document* doc = current_document();
    file_encoding previous = doc->file().encoding;
    doc->set_encoding(encoding_for_save(name));
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
