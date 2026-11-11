#include "kit.hpp"
#include "settings.hpp"

#include <QLayout>
#include <QPixmap>

#include <utility>

constexpr const char* AUTOSTART_NAME_PREFIX = "program";
constexpr const char* SHORTCUT_NAME_PREFIX = "shortcut";
constexpr const char* EXEC_ACTION = "exec";
constexpr const char* AUTOSTART_CARD = "Autostart";
constexpr const char* SHORTCUTS_CARD = "Shortcuts";

/* The indexes of the entries whose editors show under their rows, or -1 for none */
static int g_open_autostart_index = -1;
static int g_open_shortcut_index = -1;

/* The first pinned launcher that runs `path` with `args`, or null when none does or `path` is empty */
static const stlxconf_pin_t* pin_for(const char* path, const char* args) {
    if (!path[0]) {
        return nullptr;
    }

    for (uint32_t i = 0; i < g_conf.pin_count; i++) {
        const stlxconf_pin_t& pin = g_conf.pins[i];
        bool is_same_command = strcmp(pin.path, path) == 0 && strcmp(pin.args, args) == 0;
        if (is_same_command) {
            return &pin;
        }
    }

    return nullptr;
}

QString program_name(const char* path, const char* args) {
    const stlxconf_pin_t* pin = pin_for(path, args);
    if (pin && pin->label[0]) {
        return QString::fromUtf8(pin->label);
    }

    QString first_arg = QString::fromUtf8(args).section(QLatin1Char(' '), 0, 0);
    bool has_program_arg = path[0] && first_arg.startsWith(QLatin1Char('/'));
    QString program_path = has_program_arg ? first_arg : QString::fromUtf8(path);
    QString file = program_path.section(QLatin1Char('/'), -1);

    return file.isEmpty() ? QStringLiteral("New program") : file;
}

static QString shortcut_name(const stlxconf_shortcut_t& shortcut) {
    const char* label = stlxconf_action_label(stlxconf_action_kind(shortcut.action));
    if (label) {
        return QString::fromUtf8(label);
    }

    if (!shortcut.path[0]) {
        return QStringLiteral("New shortcut");
    }

    return QStringLiteral("Open %1").arg(program_name(shortcut.path, shortcut.args));
}

/* The config section an entry is written under, which also names its row for search */
static QString autostart_section(const stlxconf_autostart_t& entry) {
    return QStringLiteral("autostart:%1").arg(QString::fromUtf8(entry.name));
}

static QString shortcut_section(const stlxconf_shortcut_t& entry) {
    return QStringLiteral("shortcut:%1").arg(QString::fromUtf8(entry.name));
}

static void show_program_icon(list_row* row, const char* path, const char* args) {
    const stlxconf_pin_t* pin = pin_for(path, args);
    row->set_icon(pin ? launcher_icon(*pin) : QPixmap());
}

static void move_autostart(uint32_t from, uint32_t to) {
    std::swap(g_conf.autostart[from], g_conf.autostart[to]);
    g_open_autostart_index = -1;
}

static void remove_autostart(uint32_t at) {
    remove_entry(g_conf.autostart, g_conf.autostart_count, at);
    g_open_autostart_index = -1;
}

static void move_shortcut(uint32_t from, uint32_t to) {
    std::swap(g_conf.shortcuts[from], g_conf.shortcuts[to]);
    g_open_shortcut_index = -1;
}

static void remove_shortcut(uint32_t at) {
    remove_entry(g_conf.shortcuts, g_conf.shortcut_count, at);
    g_open_shortcut_index = -1;
}

/* An added entry opens in its editor, since it starts with no program */
static void add_autostart() {
    if (g_conf.autostart_count >= STLXCONF_MAX_AUTOSTART) {
        return;
    }

    const stlxconf_t& saved = saved_conf();
    uint32_t owned = owned_count(g_conf.autostart, g_conf.autostart_count);
    QString name = unused_entry_name(
        AUTOSTART_NAME_PREFIX,
        saved.autostart,
        saved.autostart_count,
        g_conf.autostart,
        g_conf.autostart_count
    );
    stlxconf_autostart_t* entry = insert_entry(g_conf.autostart, g_conf.autostart_count, owned);
    copy_string(entry->name, sizeof(entry->name), name);

    g_open_autostart_index = static_cast<int>(owned);
    show_pending_changes();
    rebuild_page_later();
}

static void add_shortcut() {
    if (g_conf.shortcut_count >= STLXCONF_MAX_SHORTCUTS) {
        return;
    }

    const stlxconf_t& saved = saved_conf();
    uint32_t owned = owned_count(g_conf.shortcuts, g_conf.shortcut_count);
    QString name = unused_entry_name(
        SHORTCUT_NAME_PREFIX,
        saved.shortcuts,
        saved.shortcut_count,
        g_conf.shortcuts,
        g_conf.shortcut_count
    );
    stlxconf_shortcut_t* entry = insert_entry(g_conf.shortcuts, g_conf.shortcut_count, owned);
    copy_string(entry->name, sizeof(entry->name), name);
    copy_string(entry->action, sizeof(entry->action), QString::fromLatin1(EXEC_ACTION));

    g_open_shortcut_index = static_cast<int>(owned);
    show_pending_changes();
    rebuild_page_later();
}

static QWidget* make_autostart_editor(stlxconf_autostart_t* entry, list_row* row) {
    auto refresh_row = [entry, row]() {
        QString name = program_name(entry->path, entry->args);
        row->set_text(name, QString::fromUtf8(entry->path), QString::fromUtf8(entry->args));
        show_program_icon(row, entry->path, entry->args);
        show_pending_changes();
    };

    QWidget* editor = make_editor();
    editor->layout()->addWidget(make_field_row(
        QStringLiteral("Program"),
        QStringLiteral("path"),
        entry->path,
        sizeof(entry->path),
        refresh_row
    ));
    editor->layout()->addWidget(make_field_row(
        QStringLiteral("Arguments"),
        QStringLiteral("args"),
        entry->args,
        sizeof(entry->args),
        refresh_row
    ));

    return editor;
}

static QWidget* make_shortcut_editor(stlxconf_shortcut_t* entry, list_row* row, keycaps* chord) {
    auto refresh_row = [entry, row, chord]() {
        row->set_text(shortcut_name(*entry), QString::fromUtf8(entry->path), QString::fromUtf8(entry->args));
        chord->set_chord(QString::fromUtf8(entry->key));
        show_pending_changes();
    };

    QWidget* editor = make_editor();
    editor->layout()->addWidget(make_field_row(
        QStringLiteral("Keys"),
        QStringLiteral("key"),
        entry->key,
        sizeof(entry->key),
        refresh_row
    ));

    bool runs_program = stlxconf_action_kind(entry->action) == STLXCONF_ACTION_EXEC;
    if (!runs_program) {
        return editor;
    }

    editor->layout()->addWidget(make_field_row(
        QStringLiteral("Program"),
        QStringLiteral("path"),
        entry->path,
        sizeof(entry->path),
        refresh_row
    ));
    editor->layout()->addWidget(make_field_row(
        QStringLiteral("Arguments"),
        QStringLiteral("args"),
        entry->args,
        sizeof(entry->args),
        refresh_row
    ));

    return editor;
}

static void add_autostart_list(QWidget* page) {
    card* section = add_section(page, QLatin1String(AUTOSTART_CARD), QStringLiteral("[autostart:<id>]  path  args"));
    auto* list = new list_box;
    uint32_t owned = owned_count(g_conf.autostart, g_conf.autostart_count);

    for (uint32_t i = 0; i < g_conf.autostart_count; i++) {
        stlxconf_autostart_t* entry = &g_conf.autostart[i];
        QString path = QString::fromUtf8(entry->path);
        auto* row = new list_row(program_name(entry->path, entry->args), path, QString::fromUtf8(entry->args));
        row->setObjectName(autostart_section(*entry));
        show_program_icon(row, entry->path, entry->args);
        list->add_row(row);

        if (entry->from_drop_in) {
            lock_drop_in_row(row, autostart_section(*entry));
            continue;
        }

        add_entry_menu(row, i, owned, move_autostart, remove_autostart);
        make_openable(row, static_cast<int>(i), &g_open_autostart_index);
        if (static_cast<int>(i) == g_open_autostart_index) {
            list->add_entry(make_autostart_editor(entry, row));
        }
    }

    bool is_room_left = g_conf.autostart_count < STLXCONF_MAX_AUTOSTART;
    push_button* add = make_add_button(QStringLiteral("Add program"), is_room_left, add_autostart);
    list->set_footer(add, QStringLiteral("Starts in this order, after the desktop is up"));
    section->add_row(list);
}

static void add_shortcut_list(QWidget* page) {
    QString columns = QStringLiteral("[shortcut:<id>]  key  action  path  args");
    card* section = add_section(page, QLatin1String(SHORTCUTS_CARD), columns);
    auto* list = new list_box;
    uint32_t owned = owned_count(g_conf.shortcuts, g_conf.shortcut_count);

    for (uint32_t i = 0; i < g_conf.shortcut_count; i++) {
        stlxconf_shortcut_t* entry = &g_conf.shortcuts[i];
        QString path = QString::fromUtf8(entry->path);
        auto* row = new list_row(shortcut_name(*entry), path, QString::fromUtf8(entry->args));
        row->setObjectName(shortcut_section(*entry));

        auto* chord = new keycaps(QString::fromUtf8(entry->key));
        row->set_leading(chord);
        list->add_row(row);

        if (entry->from_drop_in) {
            lock_drop_in_row(row, shortcut_section(*entry));
            continue;
        }

        add_entry_menu(row, i, owned, move_shortcut, remove_shortcut);
        make_openable(row, static_cast<int>(i), &g_open_shortcut_index);
        if (static_cast<int>(i) == g_open_shortcut_index) {
            list->add_entry(make_shortcut_editor(entry, row, chord));
        }
    }

    bool is_room_left = g_conf.shortcut_count < STLXCONF_MAX_SHORTCUTS;
    push_button* add = make_add_button(QStringLiteral("Add shortcut"), is_room_left, add_shortcut);
    QString hint = QStringLiteral("Keys joined by +, such as %1").arg(code_text(QStringLiteral("ctrl+alt+t")));
    list->set_footer(add, hint);
    section->add_row(list);
}

void build_startup_page(QWidget* page) {
    add_autostart_list(page);
    add_shortcut_list(page);
}

/* A program shows its path, and a shortcut its chord, where a setting shows its key */
void add_startup_entries(QList<search_entry>& entries) {
    for (uint32_t i = 0; i < g_conf.autostart_count; i++) {
        const stlxconf_autostart_t& entry = g_conf.autostart[i];
        entries.append({ STARTUP_PAGE, QLatin1String(AUTOSTART_CARD), program_name(entry.path, entry.args),
                         QString::fromUtf8(entry.path), QString(), autostart_section(entry) });
    }

    for (uint32_t i = 0; i < g_conf.shortcut_count; i++) {
        const stlxconf_shortcut_t& entry = g_conf.shortcuts[i];
        entries.append({ STARTUP_PAGE, QLatin1String(SHORTCUTS_CARD), shortcut_name(entry),
                         QString::fromUtf8(entry.key), QString(), shortcut_section(entry) });
    }
}
