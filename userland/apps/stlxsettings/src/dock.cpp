#include "kit.hpp"
#include "settings.hpp"

#include <QLayout>
#include <QPixmap>

#include <utility>

constexpr const char* PIN_NAME_PREFIX = "pin";

/* The index of the pin whose editor shows under its row, or -1 for none */
static int g_open_pin_index = -1;

static void move_pin(uint32_t from, uint32_t to) {
    std::swap(g_conf.pins[from], g_conf.pins[to]);
    g_open_pin_index = -1;
}

static void remove_pin(uint32_t at) {
    remove_entry(g_conf.pins, g_conf.pin_count, at);
    g_open_pin_index = -1;
}

/* An added pin opens in its editor, since it starts with no program */
static void add_pin() {
    if (g_conf.pin_count >= STLXCONF_MAX_TASKBAR) {
        return;
    }

    const stlxconf_t& saved = saved_conf();
    uint32_t owned = owned_count(g_conf.pins, g_conf.pin_count);
    QString name = unused_entry_name(PIN_NAME_PREFIX, saved.pins, saved.pin_count, g_conf.pins, g_conf.pin_count);
    stlxconf_pin_t* pin = insert_entry(g_conf.pins, g_conf.pin_count, owned);
    copy_string(pin->name, sizeof(pin->name), name);
    copy_string(pin->label, sizeof(pin->label), QStringLiteral("New app"));

    g_open_pin_index = static_cast<int>(owned);
    show_pending_changes();
    rebuild_page_later();
}

static QWidget* make_pin_editor(stlxconf_pin_t* pin, list_row* row) {
    auto refresh_row = [pin, row]() {
        row->set_text(QString::fromUtf8(pin->label), QString::fromUtf8(pin->path), QString::fromUtf8(pin->args));
        row->set_icon(launcher_icon(*pin));
        show_pending_changes();
    };

    QWidget* editor = make_editor();
    editor->layout()->addWidget(make_field_row(
        QStringLiteral("Name"),
        QStringLiteral("label"),
        pin->label,
        sizeof(pin->label),
        refresh_row
    ));
    editor->layout()->addWidget(make_field_row(
        QStringLiteral("Program"),
        QStringLiteral("path"),
        pin->path,
        sizeof(pin->path),
        refresh_row
    ));
    editor->layout()->addWidget(make_field_row(
        QStringLiteral("Arguments"),
        QStringLiteral("args"),
        pin->args,
        sizeof(pin->args),
        refresh_row
    ));
    editor->layout()->addWidget(make_field_row(
        QStringLiteral("Icon"),
        QStringLiteral("icon"),
        pin->icon_path,
        sizeof(pin->icon_path),
        refresh_row
    ));

    return editor;
}

void build_dock_page(QWidget* page) {
    QString columns = QStringLiteral("[taskbar:<id>]  label  path  args  icon");
    card* pins = add_section(page, QStringLiteral("Pinned apps"), columns);
    auto* list = new list_box;
    uint32_t owned = owned_count(g_conf.pins, g_conf.pin_count);

    for (uint32_t i = 0; i < g_conf.pin_count; i++) {
        stlxconf_pin_t* pin = &g_conf.pins[i];
        QString label = QString::fromUtf8(pin->label);
        auto* row = new list_row(label, QString::fromUtf8(pin->path), QString::fromUtf8(pin->args));
        row->set_icon(launcher_icon(*pin));
        list->add_row(row);

        if (pin->from_drop_in) {
            lock_drop_in_row(row, "taskbar", pin->name);
            continue;
        }

        add_entry_menu(row, i, owned, move_pin, remove_pin);
        make_openable(row, static_cast<int>(i), &g_open_pin_index);
        if (static_cast<int>(i) == g_open_pin_index) {
            list->add_entry(make_pin_editor(pin, row));
        }
    }

    bool is_room_left = g_conf.pin_count < STLXCONF_MAX_TASKBAR;
    push_button* add = make_add_button(QStringLiteral("Add app"), is_room_left, add_pin);
    QString drop_in_dir = code_text(QString::fromLatin1(STLXCONF_DROP_IN_DIR));
    list->set_footer(add, QStringLiteral("Click an app to edit it. Drop-ins come from %1").arg(drop_in_dir));
    pins->add_row(list);
}
