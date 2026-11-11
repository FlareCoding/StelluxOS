#ifndef SHEEN_SETTINGS_H
#define SHEEN_SETTINGS_H

/* Sheen's settings: a compiled table of every key with its default and its control on the Settings tab, read over by
 * `/etc/sheen/sheen.conf`. A key it does not know or a value of the wrong kind is reported and keeps the default. */
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <Scintilla.h>

/* What a tab in one mode starts with, before View changes it for that tab */
struct mode_defaults {
    bool line_numbers;
    bool indent_guides;
    int edge_column; // 0 hides the edge
    bool word_wrap;
    int wrap_column; // the reading width in Text mode, 0 wraps at the window's edge
};

struct sheen_settings {
    float font_size = 15.5f;
    int tab_width = 4;
    bool insert_spaces = true;
    bool detect_indentation = true;
    int caret_blink_ms = 500;
    mode_defaults code = { true, true, 120, false, 0 };
    mode_defaults text = { false, false, 0, true, 80 };
    QByteArray encoding = QByteArrayLiteral("UTF-8");
    int line_ending = SC_EOL_LF;
    int large_file_mb = 64;
    bool has_menu_bar = false;
};

enum class setting_control { TOGGLE, CHOICE, NUMBER, ENCODING };

/* How the Settings tab shows one key: the rail page and group it sits under, its words, and its control. A choice
 * lists its values as sheen.conf writes them and its labels, both space separated, and a number its range and unit */
struct setting_field {
    const char* section;
    const char* key;
    const char* page;
    const char* group;
    const char* label;
    const char* note;
    setting_control control;
    const char* choices;
    const char* choice_labels;
    double minimum;
    double maximum;
    double step;
    const char* unit;
};

inline constexpr const char* SETTINGS_PATH = "/etc/sheen/sheen.conf";

/* Reads `path` over `settings`. Returns each problem as `sheen.conf:12: unknown key tab_wdth`, none for a missing
 * file, which leaves every default */
QStringList read_settings(const QString& path, sheen_settings* settings);

/* Every key in the order the Settings tab and a new sheen.conf list them */
QList<setting_field> setting_fields();

/* The value of `section` and `key` in `settings` as sheen.conf writes it, such as `4` or `true` */
QString setting_value(const sheen_settings& settings, const QString& section, const QString& key);

/* Rewrites only the lines of the keys in `changes`, named `section.key`, keeping every other line, comment and the
 * order. A key the file lacks joins its section, and a missing file is first written with every key at its default */
bool write_settings(const QString& path, const QHash<QString, QString>& changes, QString* error);

#endif
