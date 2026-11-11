#ifndef SHEEN_SETTINGS_PAGE_H
#define SHEEN_SETTINGS_PAGE_H

/* The Settings tab: a rail of pages built from the settings' key table, each row a control over one key, with every
 * change held until Save writes it into sheen.conf, as Stellux Settings holds its changes */
#include "settings.h"

#include <QHash>
#include <QList>
#include <QWidget>

#include <functional>

class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class setting_row;

class settings_page : public QWidget {
public:
    settings_page(const sheen_settings& settings, QWidget* parent);

    /* The changed keys as `section.key`, each with its new value */
    QHash<QString, QString> changes() const;

    /* Takes `settings` as the saved values and drops every change, as after Save or Revert */
    void reset(const sheen_settings& settings);

    /* Shows the rail's page named `name`, such as Editor */
    void show_section(const QString& name);

    std::function<void()> on_changed;
    std::function<void()> on_save;
    std::function<void()> on_open_as_text;

private:
    QWidget* make_page(const QString& name, const QString& subtitle, const QList<setting_field>& fields);
    void refresh_footer();

    QListWidget* m_rail;
    QStackedWidget* m_pages;
    QList<setting_row*> m_rows;
    QLabel* m_footer_note;
    QPushButton* m_revert;
    QPushButton* m_save;
    sheen_settings m_saved;
};

#endif
