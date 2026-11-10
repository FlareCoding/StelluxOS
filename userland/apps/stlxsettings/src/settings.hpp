#ifndef STLXSETTINGS_SETTINGS_HPP
#define STLXSETTINGS_SETTINGS_HPP

/* What the pages share with the window around them: the configuration as edited, which Save writes, and the calls a
 * page makes. A page builder fills a widget whose layout is a QVBoxLayout. */
#include <stlxconf/conf.h>

#include <QString>

#include <cstddef>

class card;
class QWidget;

extern stlxconf_t g_conf;

/* Recounts the unsaved changes and shows their count in the footer, naming the change when there is only one. A page
 * must call it after each edit of `g_conf`. */
void show_pending_changes();

/* Copies text as UTF-8 into a configuration char array, cut on a byte to fit. `capacity` must be at least 1 */
void copy_string(char* destination, size_t capacity, const QString& text);

/* Adds a card under a head with `title` and the config section its settings write, if any */
card* add_section(QWidget* page, const QString& title, const QString& config_section);

void build_appearance_page(QWidget* page);

#endif /* STLXSETTINGS_SETTINGS_HPP */
