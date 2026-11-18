#include "extensions/python/python_extension.h"

#include "extensions/diagnostic_convert.h"
#include "extensions/extension_host.h"
#include "extensions/python/python_traceback.h"
#include "ui/ui_kit.h"

#include <QFileInfo>

#include <algorithm>
#include <string>
#include <vector>
#include <unistd.h>

namespace sheen {

static constexpr const char* PYTHON_BINARY_STELLUX = "/bin/python3";
static constexpr const char* PYTHON_PROGRAM_NAME = "python3";
static constexpr const char* REPL_CHOICE_ID = "__python_repl__";
static constexpr const char* DOCK_PANEL_ID = "python_run";

static constexpr int CONSOLE_MAX_LINES = 50000;
static constexpr int CONSOLE_FLUSH_INTERVAL_MS = 16;

// The Stellux python package installs /bin/python3 pointing to the bundled interpreter binary.
static constexpr package_requirement PYTHON_PACKAGES[] = {
    { "python", PYTHON_BINARY_STELLUX },
};

static constexpr dependency_group PYTHON_DEPS[] = {
    {
        "python-runtime",
        "Python Runtime",
        "CPython interpreter and standard library.",
        PYTHON_PACKAGES,
        {},
        nullptr,
        event_protocol::NONE,
        true,
    },
};

static QString resolve_python_program() {
    if (::access(PYTHON_BINARY_STELLUX, X_OK) == 0) {
        return QString::fromLatin1(PYTHON_BINARY_STELLUX);
    }

    return QString::fromLatin1(PYTHON_PROGRAM_NAME);
}

namespace {

class python_session final : public extension_session {
public:
    explicit python_session(extension_host& host)
        : m_host(host),
          m_workspace_root(host.workspace_root()),
          m_parser(m_workspace_root.toStdString(), true),
          m_console(new ui::ui_log_panel({
              CONSOLE_MAX_LINES,
              CONSOLE_FLUSH_INTERVAL_MS,
              true,
              QStringLiteral("Send input to Python..."),
              { QStringLiteral("LF") },
          })) {
        m_console->set_workspace_root(m_workspace_root);
        select_initial_target();
        wire_console_and_task();

        m_host.set_dock_panels({
            { QString::fromLatin1(DOCK_PANEL_ID), QStringLiteral("Run"), m_console, false, 0 },
        });
        register_commands();
        refresh_strip();
    }

    ~python_session() override {
        shutdown();
    }

    void shutdown() override {
        if (m_is_shutting_down) {
            return;
        }

        m_is_shutting_down = true;
        if (m_console) {
            m_console->on_location_clicked = nullptr;
            m_console->on_input_submitted = nullptr;
            m_console = nullptr;
        }

        m_task.on_stdout = nullptr;
        m_task.on_stderr = nullptr;
        m_task.on_finished = nullptr;
        m_task.stop();
    }

    void on_active_file_changed(const QString& file_path) override {
        std::string resolved = resolve_workspace_file_path(
            m_workspace_root.toStdString(), file_path.toStdString(), true);
        if (resolved.empty() || !is_python_source_path(resolved)) {
            return;
        }

        QString candidate = QString::fromStdString(resolved);
        if (m_target_script == candidate && !m_prefers_pure_repl) {
            return;
        }

        m_target_script = candidate;
        m_prefers_pure_repl = false;
        if (!m_task.is_running()) {
            refresh_strip();
        }
    }

private:
    void select_initial_target() {
        std::string root_std = m_workspace_root.toStdString();
        QString active = m_host.active_file_path();
        std::string resolved_active = resolve_workspace_file_path(root_std, active.toStdString(), true);
        if (!resolved_active.empty() && is_python_source_path(resolved_active)) {
            m_target_script = QString::fromStdString(resolved_active);
            return;
        }

        std::vector<std::string> discovered = discover_python_targets(root_std);
        if (!discovered.empty()) {
            m_target_script = QString::fromStdString(discovered.front());
        }
    }

    void wire_console_and_task() {
        m_console->on_location_clicked = [this](const QString& path, int line) {
            open_diagnostic_location(path, line);
        };

        m_console->on_input_submitted = [this](const QByteArray& bytes) {
            if (m_task.is_running()) {
                m_task.write_stdin(bytes);
            }
        };

        m_task.on_stdout = [this](QByteArrayView chunk) {
            handle_process_output(chunk);
        };

        m_task.on_stderr = [this](QByteArrayView chunk) {
            handle_process_output(chunk);
        };

        m_task.on_finished = [this](const process_exit& exit_result) {
            handle_process_finished(exit_result);
        };
    }

    void register_commands() {
        m_host.set_commands({
            {
                QStringLiteral("python.run"),
                QStringLiteral("Python"),
                QStringLiteral("Run Script"),
                QStringLiteral("Ctrl+F5"),
                false,
                [this] { return !m_task.is_running(); },
                [this] { start_python(false); },
            },
            {
                QStringLiteral("python.repl"),
                QStringLiteral("Python"),
                QStringLiteral("Start Interactive REPL"),
                QStringLiteral("Alt+F5"),
                false,
                [this] { return !m_task.is_running(); },
                [this] { start_python(true); },
            },
            {
                QStringLiteral("python.stop"),
                QStringLiteral("Python"),
                QStringLiteral("Stop"),
                QStringLiteral("Ctrl+Shift+F5"),
                true,
                [this] { return m_task.is_running(); },
                [this] { stop_python(); },
            },
        });
    }

    void start_python(bool interactive_repl) {
        if (m_is_shutting_down || !m_console || m_task.is_running() || !m_host.save_modified_files()) {
            return;
        }

        if (!interactive_repl && !m_prefers_pure_repl && m_target_script.isEmpty()) {
            select_initial_target();
        }

        QString script_to_load = m_prefers_pure_repl ? QString() : m_target_script;
        if (!script_to_load.isEmpty() && !QFileInfo::exists(script_to_load)) {
            m_target_script.clear();
            select_initial_target();
            script_to_load = m_target_script;
        }

        bool run_as_repl = interactive_repl || m_prefers_pure_repl || script_to_load.isEmpty();

        m_output_filter.reset();
        m_parser.reset();
        m_host.clear_diagnostics();
        m_console->clear_log();
        m_status_readout.clear();
        m_status_is_danger = false;

        QStringList arguments = { QStringLiteral("-u") };
        if (run_as_repl) {
            arguments << QStringLiteral("-i");
        }

        if (!script_to_load.isEmpty()) {
            arguments << script_to_load;
        }

        process_spec spec;
        spec.program = resolve_python_program();
        spec.arguments = arguments;
        spec.working_directory = m_workspace_root;
        spec.io_mode = process_io_mode::PTY;
        spec.opens_stdin = true;

        QString start_error;
        if (!m_task.start(spec, &start_error)) {
            m_status_readout = QStringLiteral("Launch failed");
            m_status_is_danger = true;
            m_console->append_mark(start_error, true);

            m_host.reveal_dock_panel(QString::fromLatin1(DOCK_PANEL_ID));
            m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), false, 1);
            refresh_strip();
            return;
        }

        m_is_repl_run = run_as_repl;
        m_console->append_mark(build_launch_caption(run_as_repl, script_to_load), false);

        m_host.reveal_dock_panel(QString::fromLatin1(DOCK_PANEL_ID));
        m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), true, 0);
        refresh_strip();
    }

    QString build_launch_caption(bool run_as_repl, const QString& script_path) const {
        if (script_path.isEmpty()) {
            return QStringLiteral("Python REPL");
        }

        QString display = QString::fromStdString(
            relative_workspace_display_path(m_workspace_root.toStdString(), script_path.toStdString()));
        if (run_as_repl) {
            return QStringLiteral("Python REPL (%1)").arg(display);
        }

        return QStringLiteral("Running %1").arg(display);
    }

    void stop_python() {
        if (m_is_shutting_down || !m_task.is_running()) {
            return;
        }

        m_task.stop();
        refresh_strip();
    }

    void handle_process_output(QByteArrayView chunk) {
        if (m_is_shutting_down || !m_console) {
            return;
        }

        std::string_view raw(chunk.data(), static_cast<size_t>(chunk.size()));
        std::string sanitized = m_output_filter.feed(raw);
        if (!sanitized.empty()) {
            m_console->append_bytes(QByteArrayView(sanitized.data(), static_cast<qsizetype>(sanitized.size())));
        }

        if (m_parser.feed(raw)) {
            sync_diagnostics_to_host();
            append_pending_jump_links();
            m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), m_task.is_running(), m_parser.error_count());
        }
    }

    void handle_process_finished(const process_exit& exit_result) {
        if (m_is_shutting_down || !m_console) {
            return;
        }

        if (m_parser.finish()) {
            sync_diagnostics_to_host();
        }

        if (!exit_result.was_stopped) {
            append_pending_jump_links();
        }

        m_is_repl_run = false;
        apply_exit_status(exit_result);
        refresh_strip();
    }

    void append_pending_jump_links() {
        if (!m_console) {
            return;
        }

        std::string jump_lines = m_parser.take_pending_jump_lines();
        if (!jump_lines.empty()) {
            m_console->append_bytes(QByteArrayView(jump_lines.data(), static_cast<qsizetype>(jump_lines.size())));
        }
    }

    void apply_exit_status(const process_exit& exit_result) {
        if (exit_result.was_stopped) {
            m_status_readout = QStringLiteral("Stopped");
            m_status_is_danger = false;
            m_console->append_mark(QStringLiteral("Stopped"), false);
            m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), false, m_parser.error_count());
            return;
        }

        int errors = m_parser.error_count();
        bool failed = exit_result.exit_code != 0 || errors > 0;
        if (errors > 0) {
            m_status_readout = errors == 1 ? QStringLiteral("1 error") : QStringLiteral("%1 errors").arg(errors);
        } else if (exit_result.exit_code != 0) {
            m_status_readout = QStringLiteral("Exit %1").arg(exit_result.exit_code);
        } else {
            m_status_readout = QStringLiteral("Ready");
        }

        m_status_is_danger = failed;
        QString mark_text = exit_result.term_signal > 0
                          ? QStringLiteral("Terminated by signal %1 (exit %2)")
                                .arg(exit_result.term_signal)
                                .arg(exit_result.exit_code)
                          : QStringLiteral("Exited with code %1").arg(exit_result.exit_code);
        m_console->append_mark(mark_text, exit_result.exit_code != 0);

        int badge_count = failed ? std::max(1, errors) : 0;
        m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), false, badge_count);
    }

    void sync_diagnostics_to_host() {
        if (m_parser.diagnostics().empty()) {
            m_host.clear_diagnostics();
            return;
        }

        m_host.set_diagnostics(to_host_diagnostics(m_parser.diagnostics()));
    }

    void open_diagnostic_location(const QString& path, int line) {
        std::string resolved = resolve_workspace_file_path(
            m_workspace_root.toStdString(), path.toStdString(), true);
        std::string lookup_path = resolved.empty() ? path.toStdString() : resolved;
        int column = 0;

        for (const script_diagnostic& diag : m_parser.diagnostics()) {
            if (diag.file_path == lookup_path && diag.line == line && diag.column > 0) {
                column = diag.column;
                break;
            }
        }

        if (column == 0) {
            for (const traceback_frame& frame : m_parser.frames()) {
                if (frame.resolved_path == lookup_path && frame.line == line && frame.column > 0) {
                    column = frame.column;
                    break;
                }
            }
        }

        QString open_target = resolved.empty() ? path : QString::fromStdString(resolved);
        m_host.open_file_at_line(open_target, line, column);
    }

    QList<picker_choice> list_target_choices() const {
        QList<picker_choice> choices;
        std::string root_std = m_workspace_root.toStdString();
        std::vector<std::string> files = discover_python_targets(root_std);

        for (const std::string& full_path : files) {
            std::string rel = relative_workspace_display_path(root_std, full_path);
            QFileInfo relative_file(QString::fromStdString(rel));
            QString parent_dir = relative_file.path() == QLatin1String(".") ? QString() : relative_file.path();

            picker_choice choice;
            choice.id = QString::fromStdString(full_path);
            choice.label = relative_file.fileName();
            choice.detail = parent_dir;
            choices.append(choice);
        }

        picker_choice repl_choice;
        repl_choice.id = QString::fromLatin1(REPL_CHOICE_ID);
        repl_choice.label = QStringLiteral("Interactive REPL");
        repl_choice.detail = QStringLiteral("python3 -i");
        choices.append(repl_choice);

        return choices;
    }

    void select_target_choice(const QString& choice_id) {
        if (choice_id == QLatin1String(REPL_CHOICE_ID)) {
            m_prefers_pure_repl = true;
            m_target_script.clear();
            refresh_strip();
            return;
        }

        m_prefers_pure_repl = false;
        m_target_script = choice_id;
        refresh_strip();
    }

    QString current_target_label() const {
        if (m_prefers_pure_repl || m_target_script.isEmpty()) {
            return QStringLiteral("REPL");
        }

        return QFileInfo(m_target_script).fileName();
    }

    QString current_target_detail() const {
        if (m_prefers_pure_repl || m_target_script.isEmpty()) {
            return {};
        }

        std::string rel = relative_workspace_display_path(
            m_workspace_root.toStdString(), m_target_script.toStdString());
        QFileInfo relative_file(QString::fromStdString(rel));

        return relative_file.path() == QLatin1String(".") ? QString() : relative_file.path();
    }

    QString current_readout_label() const {
        if (m_task.is_running()) {
            return m_is_repl_run ? QStringLiteral("REPL active") : QStringLiteral("Running");
        }

        return m_status_readout.isEmpty() ? QStringLiteral("Ready") : m_status_readout;
    }

    void refresh_strip() {
        if (m_is_shutting_down) {
            return;
        }

        bool running = m_task.is_running();

        picker_spec target_picker;
        target_picker.id = QStringLiteral("target");
        target_picker.label = current_target_label();
        target_picker.detail = current_target_detail();
        target_picker.search_placeholder = QStringLiteral("Filter .py scripts...");

        target_picker.is_enabled = !running;
        target_picker.list_choices = [this] { return list_target_choices(); };
        target_picker.on_selected = [this](const QString& choice_id) { select_target_choice(choice_id); };

        QList<button_spec> buttons = {
            {
                QStringLiteral("run"),
                QStringLiteral("Run"),
                QStringLiteral("Run script (Ctrl+F5)"),
                !running,
                running && !m_is_repl_run,
                [this] { start_python(false); },
            },
            {
                QStringLiteral("repl"),
                QStringLiteral("REPL"),
                QStringLiteral("Start interactive REPL (Alt+F5)"),
                !running,
                running && m_is_repl_run,
                [this] { start_python(true); },
            },
            {
                QStringLiteral("stop"),
                QStringLiteral("Stop"),
                QStringLiteral("Stop Python process (Ctrl+Shift+F5)"),
                running,
                false,
                [this] { stop_python(); },
            },
        };

        readout_spec state_readout;
        state_readout.id = QStringLiteral("state");
        state_readout.label = current_readout_label();
        state_readout.progress_percent = -1;
        state_readout.is_danger = !running && m_status_is_danger;

        strip_spec strip;
        strip.pickers = { target_picker };
        strip.buttons = buttons;
        strip.readouts = { state_readout };
        strip.focus_extensions = { QStringLiteral(".py") };
        m_host.set_strip(strip);
    }

    extension_host& m_host;
    QString m_workspace_root;
    terminal_escape_filter m_output_filter;
    python_traceback_parser m_parser;
    ui::ui_log_panel* m_console = nullptr;
    process_task m_task;
    QString m_target_script;
    QString m_status_readout;
    bool m_prefers_pure_repl = false;
    bool m_is_repl_run = false;
    bool m_status_is_danger = false;
    bool m_is_shutting_down = false;
};

} // namespace

static bool qualifies_python(const workspace_files& files) {
    if (files.contains(QStringLiteral("pyproject.toml")) || files.contains(QStringLiteral("main.py"))) {
        return true;
    }

    return files.has_suffix(QStringLiteral(".py"));
}

static std::unique_ptr<extension_session> create_python_session(extension_host& host) {
    return std::make_unique<python_session>(host);
}

extension_descriptor python_extension() {
    return {
        "python",
        "Python",
        "Run Python scripts and interactive REPL sessions with inline traceback diagnostics.",
        extension_icon::PYTHON,
        nullptr,
        false,
        PYTHON_DEPS,
        {},
        qualifies_python,
        create_python_session,
    };
}

} // namespace sheen
