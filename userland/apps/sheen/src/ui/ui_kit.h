#ifndef SHEEN_UI_UI_KIT_H
#define SHEEN_UI_UI_KIT_H

#include "extensions/extension_host.h"
#include "ui/ui_log_panel.h"

#include <QAbstractButton>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>
#include <utility>

class QLabel;
class QLineEdit;
class QListWidget;
class QTreeWidget;
class ScintillaEditBase;

namespace sheen::ui {

struct section_row {
    QString id;
    QString label;
    QString detail; // right-aligned version or metadata in TEXT_DIM
    QString tooltip;
};

struct table_column_spec {
    QString title;
    int width = 0; // 0 stretches column
    bool aligns_right = false;
};

struct table_row {
    QString id;
    QStringList cells;
};

/* Warm-stone 28 px pill button with optional active indicator dot. */
class ui_pill_button : public QAbstractButton {
public:
    explicit ui_pill_button(const QString& label, QWidget* parent = nullptr);

    void set_checked_dot(bool is_active);
    void set_detail(const QString& detail);
    void set_has_chevron(bool has_chevron);
    void set_is_open(bool is_open);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString m_detail;
    bool m_is_active = false;
    bool m_has_chevron = false;
    bool m_is_open = false;
};

/* Compact Razor toggle switch for the Extensions popover. */
class ui_switch : public QAbstractButton {
public:
    explicit ui_switch(QWidget* parent = nullptr);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
};

/* Fixed-geometry Qt::Popup anchored to a control, avoiding QStelluxWindow::setGeometry popup recreation. */
class ui_popover : public QWidget {
public:
    ui_popover(QWidget* anchor, int width, int height);

    void open_below_anchor();

    std::function<void()> on_closed;

protected:
    void paintEvent(QPaintEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QPointer<QWidget> m_anchor;
};

/* Searchable dropdown menu built on ui_popover with instant keyboard filtering. */
class ui_searchable_picker : public ui_popover {
public:
    ui_searchable_picker(QWidget* anchor, const QString& placeholder, const QList<picker_choice>& choices);

    std::function<void(const QString& choice_id)> on_selected;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void filter_choices(const QString& query);
    void activate_row(int row);

    QLineEdit* m_search;
    QListWidget* m_list;
    QList<picker_choice> m_choices;
    QList<int> m_filtered_indices;
};

/* Sidebar list section with a header action button and 30 px Razor rows. */
class ui_section_list : public QWidget {
public:
    explicit ui_section_list(const QString& action_label, QWidget* parent = nullptr);

    void set_rows(const QList<section_row>& rows);
    void set_empty_text(const QString& text);

    std::function<void()> on_action_clicked;
    std::function<void(const QString& row_id)> on_row_activated;

private:
    QListWidget* m_list;
    QLabel* m_empty;
};

/* Inline bar rendered under an editor line or inside Problems with a one-click fix pill. */
class ui_inline_diagnostic : public QWidget {
public:
    explicit ui_inline_diagnostic(const diagnostic_item& item, QWidget* parent = nullptr);

    const diagnostic_item& item() const { return m_item; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    diagnostic_item m_item;
};

/* Multi-column sortable Razor table for ELF sections, symbols, and profiler top-function breakdowns. */
class ui_table_view : public QWidget {
public:
    explicit ui_table_view(const QList<table_column_spec>& columns, QWidget* parent = nullptr);

    void set_rows(const QList<table_row>& rows);

    std::function<void(const QString& row_id)> on_row_activated;

private:
    QList<table_column_spec> m_columns;
    QTreeWidget* m_tree;
};

/* Synchronized dual read-only Scintilla panes for side-by-side Source and Disassembly inspection. */
class ui_split_code_view : public QWidget {
public:
    explicit ui_split_code_view(QWidget* parent = nullptr);

    ScintillaEditBase* left_view() const { return m_left; }
    ScintillaEditBase* right_view() const { return m_right; }
    void set_line_mapping(const QList<std::pair<int, int>>& left_to_right_lines);

private:
    void sync_counterpart(ScintillaEditBase* source, ScintillaEditBase* target, const QHash<int, int>& mapping);

    ScintillaEditBase* m_left;
    ScintillaEditBase* m_right;
    QHash<int, int> m_left_to_right;
    QHash<int, int> m_right_to_left;
    bool m_is_syncing = false;
};

} // namespace sheen::ui

#endif
