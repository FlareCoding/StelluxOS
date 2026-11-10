#ifndef STLXSETTINGS_SETTINGS_HPP
#define STLXSETTINGS_SETTINGS_HPP

/* What the pages share with the window around them: the configuration as edited, which Save writes, and the calls a
 * page makes. A page builder fills a widget whose layout is a QVBoxLayout. */
#include <stlxconf/conf.h>
#include <stlxwin/stlxwin.h>

#include <QImage>
#include <QString>
#include <QStringList>

#include <sys/utsname.h>

#include <cstddef>
#include <cstring>
#include <functional>

class card;
class list_row;
class push_button;
class section_head;
class QPixmap;
class QWidget;

extern stlxconf_t g_conf;

/* The configuration as last loaded or saved, which the pending changes count against */
const stlxconf_t& saved_conf();

/* Recounts the unsaved changes and shows their count in the footer, naming the change when there is only one. About
 * drops Save and Revert while nothing is pending and says there is nothing to save. A page must call it after each edit
 * of `g_conf`. */
void show_pending_changes();

/* Shows `text` in the footer as a sentence whatever case it starts in, until the next message or recount of the changes
 * replaces it */
void set_status(const QString& text);

/* Rebuilds the shown page once the event that asked for it is done, since the rebuild deletes that
 * event's widget. The page takes no input until then, so no click reaches a widget the rebuild replaces */
void rebuild_page_later();

/* Rebuilds the Displays page the same way, and only while it is the page shown */
void rebuild_displays_page_later();

/* Copies text as UTF-8 into a configuration char array, cut on a byte to fit. `capacity` must be at least 1 */
void copy_string(char* destination, size_t capacity, const QString& text);

/* Adds a card for a section's settings under a head with `caption` at its right, such as the config section
 * they write */
card* add_section(QWidget* page, const QString& title, const QString& caption);

/* Adds a head the page built, such as one it updates in place, and the card under it */
card* add_section(QWidget* page, section_head* head);

/* Whether held keys repeat, which takes a nonzero delay and interval, as the display manager reads them */
bool is_key_repeat_on(const stlxconf_t& conf);

/* A row whose text field writes through to one of the configuration's char arrays */
QWidget* make_field_row(
    const QString& label,
    const QString& key,
    char* destination,
    size_t capacity,
    std::function<void()> on_edit
);

/* Gives a list row the menu that moves or removes its entry, among the first `owned` entries of its list.
 * The menu then recounts the unsaved changes and rebuilds the page, so `move` and `remove` only edit the list */
void add_entry_menu(
    list_row* row,
    uint32_t index,
    uint32_t owned,
    std::function<void(uint32_t, uint32_t)> move,
    std::function<void(uint32_t)> remove
);

/* Shows `row` open while `*open_index` is `index`, and makes a click on it toggle `*open_index` between `index`
 * and -1, then rebuild the page. The page builder must add the entry's editor under an open row */
void make_openable(list_row* row, int index, int* open_index);

/* Shows `row` read only, with a chip naming the drop-in file that holds its entry `[<prefix>:<name>]`, or conf.d
 * when no readable .conf file holds it. The caller must not give the row a menu or an editor */
void lock_drop_in_row(list_row* row, const char* prefix, const char* name);

/* A list footer's small add button, with a plus glyph. It runs `add` and is disabled unless `is_room_left` */
push_button* make_add_button(const QString& text, bool is_room_left, std::function<void()> add);

/* The box an open entry's field rows go in, under its row */
QWidget* make_editor();

/* A launcher's icon, or the default icon when it names none or its icon file does not load, as the dock shows it */
QPixmap launcher_icon(const stlxconf_pin_t& pin);

/* A program's name on screen: its launcher's label if set, else the file it runs, which is its first argument
 * when that is a path, or "New program" while it has no path */
QString program_name(const char* path, const char* args);

/* How many of the user's own entries lead a list. Drop-in entries follow them in every list, so edits,
 * moves and additions stay among these */
template <typename item_t>
uint32_t owned_count(const item_t* items, uint32_t count) {
    uint32_t owned = 0;
    while (owned < count && !items[owned].from_drop_in) {
        owned++;
    }

    return owned;
}

/* Opens a zeroed entry at `at` by shifting the entries from `at` on up by one, and raises `count` by one.
 * `items` must have room for `count + 1` entries */
template <typename item_t>
item_t* insert_entry(item_t* items, uint32_t& count, uint32_t at) {
    for (uint32_t i = count; i > at; i--) {
        items[i] = items[i - 1];
    }

    memset(&items[at], 0, sizeof(items[at]));
    count++;

    return &items[at];
}

/* Removes the entry at `at` by shifting the entries after it down by one, and lowers `count` by one */
template <typename item_t>
void remove_entry(item_t* items, uint32_t& count, uint32_t at) {
    for (uint32_t i = at; i + 1 < count; i++) {
        items[i] = items[i + 1];
    }

    count--;
}

/* The index of the entry named `name`, or -1 when no entry has it */
template <typename item_t>
int find_entry(const item_t* items, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(items[i].name, name) == 0) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

/* The first section name `<prefix><n>` that neither the saved nor the edited list has, so an added entry never
 * reuses the name of an entry removed since the last save */
template <typename item_t>
QString unused_entry_name(
    const char* prefix,
    const item_t* saved,
    uint32_t saved_count,
    const item_t* edited,
    uint32_t edited_count
) {
    uint32_t n = 0;
    while (true) {
        QByteArray name = QStringLiteral("%1%2").arg(QLatin1String(prefix)).arg(n).toUtf8();
        bool is_saved = find_entry(saved, saved_count, name.constData()) >= 0;
        bool is_edited = find_entry(edited, edited_count, name.constData()) >= 0;
        if (!is_saved && !is_edited) {
            return QString::fromUtf8(name);
        }

        n++;
    }
}

/* The configured wallpaper scaled to cover `size`, or the fallback color when it cannot be read */
QImage wallpaper_picture(const QSize& size);

/* The display layout is stlxdm's to store, so Save sends it apart from the file, Revert sets it back to the places
 * stlxdm lists, and `note_layout_changes` adds its unsaved changes to the list the footer counts */
void note_layout_changes(QStringList& changes);
void save_layout();
void revert_layout();

/* A display mode's keep prompt, which the window must place above the page, so a trial stays answerable from
 * every page */
QWidget* make_keep_prompt();

/* The monitor's maker and name, or "Display" and `index` + 1 when it reports no identity */
QString display_name(const stlxwin_output& output, uint32_t index);

/* The output's width by height, then its refresh rate in whole hertz when the output reports one */
QString mode_text(const stlxwin_output& output);

/* The system's name and release before its build suffix, Stellux 3.0.1 of 3.0.1-x86_64-release */
QString system_title(const utsname& system);

void build_about_page(QWidget* page);
void build_appearance_page(QWidget* page);
void build_displays_page(QWidget* page);
void build_dock_page(QWidget* page);
void build_input_page(QWidget* page);
void build_startup_page(QWidget* page);

#endif /* STLXSETTINGS_SETTINGS_HPP */
