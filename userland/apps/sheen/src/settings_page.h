#ifndef SHEEN_SETTINGS_PAGE_H
#define SHEEN_SETTINGS_PAGE_H

/* The Settings tab: pages built from the settings' key table whose changes wait for Save into sheen.conf, as Stellux
 * Settings holds its changes, and a last page listing every action's keys */
#include "settings.h"

#include <QHash>
#include <QList>
#include <QWidget>

#include <functional>
#include <utility>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class setting_row;
class shortcut_row;

class settings_page : public QWidget {
public:
    settings_page(const sheen_settings& settings, QWidget* parent);

    /* The changed keys as `section.key`, each with its new value */
    QHash<QString, QString> changes() const;

    /* Takes `settings` as the saved values and drops every change, as after Save or Revert */
    void reset(const sheen_settings& settings);

    /* Shows the rail's page named `name`, such as Editor, and on Shortcuts puts the caret in its filter */
    void show_section(const QString& name);

    std::function<void()> on_changed;
    std::function<void()> on_save;
    std::function<void()> on_open_as_text;

private:
    QWidget* make_page(const QString& name, const QString& subtitle, const QList<setting_field>& fields);
    QWidget* make_shortcuts_page();
    void filter_shortcuts(const QString& query);
    void refresh_footer();

    QListWidget* m_rail;
    QStackedWidget* m_pages;
    QList<setting_row*> m_rows;
    QLineEdit* m_filter;
    QList<std::pair<QWidget*, QWidget*>> m_shortcut_groups; // each menu's caption and card
    QList<shortcut_row*> m_shortcut_rows;
    QLabel* m_no_match;
    QLabel* m_footer_note;
    QPushButton* m_revert;
    QPushButton* m_save;
    sheen_settings m_saved;
};

#endif
