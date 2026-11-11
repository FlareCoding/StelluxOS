#ifndef SHEEN_SETTINGS_H
#define SHEEN_SETTINGS_H

/* Sheen's settings: a compiled table of every key with its default, read over by `/etc/sheen/sheen.conf` when Sheen
 * starts. A key the table does not know or a value of the wrong kind is reported and leaves the default. */
#include <QByteArray>
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

inline constexpr const char* SETTINGS_PATH = "/etc/sheen/sheen.conf";

/* Reads `path` over `settings`. Returns each problem as `sheen.conf:12: unknown key tab_wdth`, none for a missing
 * file, which leaves every default */
QStringList read_settings(const QString& path, sheen_settings* settings);

#endif
