#ifndef SHEEN_EXTENSIONS_EXTENSION_HOST_H
#define SHEEN_EXTENSIONS_EXTENSION_HOST_H

#include "app/shared_service.h"
#include "workspace/process_task.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QWidget;

namespace sheen {

struct picker_choice {
    QString id;
    QString label;
    QString detail; // rendered in Plex Mono TEXT_DIM
    QString badge;  // optional warning or status text at right
    bool is_action = false;
    bool is_struck = false;
};

struct picker_spec {
    QString id;
    QString label;
    QString detail;
    QString search_placeholder;
    bool is_enabled = true;
    std::function<QList<picker_choice>()> list_choices;
    std::function<void(const QString& choice_id)> on_selected;
};

struct button_spec {
    QString id;
    QString label;
    QString tooltip;
    bool is_enabled = true;
    bool is_checked = false; // draws subtle surface-on ring and 6 px amber dot when active
    std::function<void()> on_clicked;
};

struct readout_spec {
    QString id;
    QString label;
    int progress_percent = -1; // 0..100 draws the 2 px bottom progress line, -1 hides it
    bool is_danger = false;
};

struct strip_spec {
    QList<picker_spec> pickers;
    QList<button_spec> buttons;
    QList<readout_spec> readouts;
    QStringList focus_extensions; // file suffixes such as {".ino", ".h"} that auto-select this strip
};

struct sidebar_section_spec {
    QString id;
    QString title;
    QWidget* widget = nullptr; // alien QWidget owned via parent container while registered
};

struct dock_panel_spec {
    QString id;
    QString title;
    QWidget* widget = nullptr; // alien QWidget, never call winId() on it or its children
    bool has_activity_dot = false;
    int error_badge_count = 0;
};

struct extension_command_spec {
    QString id;
    QString menu_title; // menu between Go and Help, such as "Embedded" or "Python"
    QString label;
    QString shortcut;
    bool starts_group = false;
    std::function<bool()> is_enabled;
    std::function<void()> on_triggered;
};

enum class diagnostic_severity { WARNING, ERROR };

struct diagnostic_fix {
    QString label;
    std::function<void()> apply;
};

struct diagnostic_item {
    QString file_path;
    int line = 1;   // 1-based line number
    int column = 0; // 1-based column, 0 highlights the quoted identifier or whole line
    diagnostic_severity severity = diagnostic_severity::ERROR;
    QString message;
    diagnostic_fix fix;
};

/* Custom non-text editor tab, such as a Flame Graph, ktrace timeline, or Source + Disassembly view. */
struct custom_tab_spec {
    QString id;
    QString title;
    QWidget* widget = nullptr; // owned by editor_core tab until closed or extension disabled
};

/* Per-line profiling or binary size annotation rendered in editor_core, such as "31.2%". */
struct line_badge_item {
    QString file_path;
    int line = 1;
    QString label;
    bool is_hot = false;
};

class extension_session {
public:
    virtual ~extension_session() = default;

    /* Called synchronously before deferred deletion on disable so tasks and listeners stop immediately. */
    virtual void shutdown() {}
    virtual void on_active_file_changed(const QString& file_path) { (void)file_path; }
};

/* Implemented per session by extension_host_adapter, scoping every mutation to that extension_id. */
class extension_host {
public:
    virtual ~extension_host() = default;

    virtual QString workspace_root() const = 0;
    virtual QString active_file_path() const = 0;

    /* Declarative 3-slot UI updates, scoped to this extension and idempotent on repeated calls. */
    virtual void set_strip(const strip_spec& strip) = 0;
    virtual void set_sidebar_sections(const QList<sidebar_section_spec>& sections) = 0;
    virtual void set_dock_panels(const QList<dock_panel_spec>& panels) = 0;
    virtual void update_dock_state(const QString& panel_id, bool has_activity_dot, int error_count) = 0;
    virtual void reveal_dock_panel(const QString& panel_id) = 0;

    /* Custom editor tabs, scoped to this extension. */
    virtual void open_custom_tab(const custom_tab_spec& tab) = 0;
    virtual void close_custom_tabs() = 0;

    /* Commands, palette entries, and workspace state, scoped to this extension. */
    virtual void set_commands(const QList<extension_command_spec>& commands) = 0;
    virtual void show_status_note(const QString& note) = 0;

    /* Editor document hooks, diagnostics, and line badges, scoped to this extension. */
    virtual bool save_modified_files() = 0;
    virtual void open_file_at_line(const QString& path, int line, int column = 0) = 0;
    virtual void set_diagnostics(const QList<diagnostic_item>& diagnostics) = 0;
    virtual void clear_diagnostics() = 0;
    virtual void set_line_badges(const QList<line_badge_item>& badges) = 0;
    virtual void clear_line_badges() = 0;

    /* Process-wide ref-counted service lookup. */
    virtual std::shared_ptr<shared_service> acquire_service(
        const QString& key,
        const std::function<std::shared_ptr<shared_service>()>& factory) = 0;
};

} // namespace sheen

#endif
