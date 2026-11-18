#ifndef SHEEN_EXTENSIONS_PERF_BINARIES_FLAME_GRAPH_VIEW_H
#define SHEEN_EXTENSIONS_PERF_BINARIES_FLAME_GRAPH_VIEW_H

#include "extensions/perf_binaries/ktrace_model.h"
#include "workspace/process_task.h"

#include <QByteArray>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QLabel;
class QScrollBar;

namespace sheen::ui {
class ui_pill_button;
class ui_searchable_picker;
} // namespace sheen::ui

namespace sheen {

class flame_timeline_canvas;

/* Interactive CPU frame timeline, `thread_states` lane, and warm-stone Flame Graph view. */
class flame_graph_view final : public QWidget {
public:
    explicit flame_graph_view(QWidget* parent = nullptr);
    ~flame_graph_view() override;

    void open_trace_path(const QString& path);
    void set_model(ktrace_profile_model model, const QString& trace_title);
    void stop_decode();

    bool has_model() const { return m_has_model; }
    bool is_filtered_to_single_thread() const { return m_filter_tid.has_value(); }
    const QString& trace_path() const { return m_trace_path; }
    const ktrace_profile_model& model() const { return m_model; }
    const ktrace_window_view& current_window() const { return m_window_view; }

    void set_zoom_window(uint64_t start_ns, uint64_t end_ns);
    void reset_zoom();

    std::function<void(const ktrace_profile_model& model, const ktrace_window_view& window,
                       bool scoped_to_single_thread)> on_window_changed;
    std::function<void(const ktrace_flame_span& span, bool open_disasm)> on_span_activated;

private:
    void start_ktrace_decode(const QString& raw_ktrace_path);
    void finish_ktrace_decode(const process_exit& exit_result);
    void cleanup_temp_db();
    uint32_t dominant_workload_tid() const;
    void open_thread_picker();
    void recompute_window();
    void sync_vertical_scrollbar();
    void update_header_and_footer();

    QString m_trace_path;
    QString m_trace_title;
    QString m_temp_db_path;
    ktrace_profile_model m_model;
    ktrace_window_view m_window_view;
    std::vector<ktrace_timeline_bar> m_timeline_bars;
    std::vector<ktrace_thread_frame> m_thread_frames;
    std::vector<ktrace_thread_state> m_lane_states;

    bool m_has_model = false;
    bool m_exclude_recorder = true;
    std::optional<uint32_t> m_filter_tid;
    uint32_t m_lane_tid = 0;
    uint32_t m_max_flame_depth = 0;
    uint64_t m_zoom_start_ns = 0;
    uint64_t m_zoom_end_ns = 0;

    QLabel* m_header_label = nullptr;
    QLabel* m_footer_label = nullptr;
    ui::ui_pill_button* m_thread_button = nullptr;
    ui::ui_pill_button* m_reset_zoom_button = nullptr;
    flame_timeline_canvas* m_canvas = nullptr;
    QScrollBar* m_vertical_scrollbar = nullptr;
    QPointer<ui::ui_searchable_picker> m_active_picker;

    std::unique_ptr<process_task> m_decode_task;
    QByteArray m_decode_stderr;
};

} // namespace sheen

#endif // SHEEN_EXTENSIONS_PERF_BINARIES_FLAME_GRAPH_VIEW_H
