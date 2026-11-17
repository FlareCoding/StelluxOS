#ifndef SHEEN_UI_UI_LOG_PANEL_H
#define SHEEN_UI_UI_LOG_PANEL_H

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

class QEvent;
class QLineEdit;
class QObject;
class QResizeEvent;
class QShowEvent;
class QTimer;
class ScintillaEditBase;

namespace sheen::ui {

class ui_pill_button;

struct log_panel_options {
    int max_lines = 100000;
    int flush_interval_ms = 16; // coalesces high-rate serial or build chunks to 60 Hz frame boundaries
    bool has_command_input = false;
    QString input_placeholder;
    QStringList line_endings; // such as {"LF", "CRLF", "CR", "None"}, empty hides ending selector
};

/* Read-only Scintilla pocket view with 16 ms batched ring buffer, hairline marks, file:line links, and stdin bar. */
class ui_log_panel : public QWidget {
public:
    explicit ui_log_panel(const log_panel_options& options = {}, QWidget* parent = nullptr);

    void append_bytes(QByteArrayView utf8);
    void append_mark(const QString& caption, bool is_danger = false);
    void clear_log();
    void set_workspace_root(const QString& root_path);

    ScintillaEditBase* view() const { return m_view; }

    std::function<void(const QByteArray& bytes)> on_input_submitted;
    std::function<void(const QString& path, int line)> on_location_clicked;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void configure_scintilla();
    void flush_pending_bytes(bool force_all = false);
    long trim_ring_buffer();
    void mark_links_from_line(long first_line);
    bool resolve_link_at(int x, int y, QString* resolved_path, int* resolved_line) const;
    QString resolve_existing_file(const QString& candidate) const;
    void update_follow_pill();
    void place_follow_pill();
    void submit_input();

    log_panel_options m_options;
    QString m_workspace_root;
    ScintillaEditBase* m_view = nullptr;
    QByteArray m_pending_bytes;
    QTimer* m_flush_timer = nullptr;
    QLineEdit* m_input = nullptr;
    ui_pill_button* m_ending_button = nullptr;
    int m_ending_index = 0;
    ui_pill_button* m_jump_to_bottom = nullptr;
    bool m_is_following = true;
    bool m_suppress_scroll_check = false;
    bool m_held_incomplete_utf8 = false;
};

} // namespace sheen::ui

#endif
