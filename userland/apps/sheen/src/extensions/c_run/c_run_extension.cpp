#include "extensions/c_run/c_run_extension.h"

#include "extensions/c_run/compiler_diagnostics.h"
#include "extensions/diagnostic_convert.h"
#include "extensions/extension_host.h"
#include "ui/ui_kit.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

namespace sheen {

static constexpr const char* STELLUX_GCC_PATH = "/usr/gcc/bin/gcc";
static constexpr const char* STELLUX_GXX_PATH = "/usr/gcc/bin/g++";
static constexpr const char* STELLUX_USR_INCLUDE = "/usr/include";
static constexpr const char* STELLUX_LIBSTLX_ARCHIVE = "/usr/lib/libstlx.a";
static constexpr const char* ALL_SOURCES_CHOICE_ID = "__c_run_all_sources__";
static constexpr const char* DOCK_PANEL_ID = "c_run";

static constexpr int CONSOLE_MAX_LINES = 100000;
static constexpr int CONSOLE_FLUSH_INTERVAL_MS = 16;

// The Stellux gcc package bundles its own assembler, linker, and C/C++ libraries under /usr/gcc.
static constexpr package_requirement C_RUN_PACKAGES[] = {
    { "gcc", STELLUX_GXX_PATH },
};

static constexpr dependency_group C_RUN_DEPS[] = {
    {
        "c-cxx-toolchain",
        "C and C++ Toolchain",
        "Native GCC C and C++ compilers, system headers, and linker.",
        C_RUN_PACKAGES,
        {},
        nullptr,
        event_protocol::NONE,
        true,
    },
};

static compiler_environment probe_compiler_environment(const QString& workspace_root) {
    compiler_environment env;
    env.workspace_root = workspace_root.toStdString();

    bool has_stellux_gcc = ::access(STELLUX_GCC_PATH, X_OK) == 0;
    bool has_stellux_gxx = ::access(STELLUX_GXX_PATH, X_OK) == 0;
    env.c_compiler = has_stellux_gcc ? std::string(STELLUX_GCC_PATH) : std::string("cc");
    env.cxx_compiler = has_stellux_gxx ? std::string(STELLUX_GXX_PATH) : std::string("c++");

    env.has_src_dir = QFileInfo(QDir(workspace_root).filePath(QStringLiteral("src"))).isDir();
    env.has_include_dir = QFileInfo(QDir(workspace_root).filePath(QStringLiteral("include"))).isDir();
    env.uses_stellux_gcc = has_stellux_gcc || has_stellux_gxx;
    env.has_stellux_usr_include = QFileInfo(QString::fromLatin1(STELLUX_USR_INCLUDE)).isDir();
    env.has_stellux_libstlx = ::access(STELLUX_LIBSTLX_ARCHIVE, R_OK) == 0;

    return env;
}

static QStringList to_qstring_arguments(const std::vector<std::string>& raw_arguments) {
    QStringList converted;
    converted.reserve(static_cast<qsizetype>(raw_arguments.size()));
    for (const std::string& item : raw_arguments) {
        converted.append(QString::fromStdString(item));
    }

    return converted;
}

static bool ensure_safe_build_directory(const QString& build_dir) {
    std::filesystem::path fs_build(build_dir.toStdString());
    std::error_code ec;
    if (std::filesystem::is_symlink(fs_build, ec)) {
        return false;
    }

    if (!QDir().mkpath(build_dir)) {
        return false;
    }

    return !std::filesystem::is_symlink(fs_build, ec) && !ec;
}

static void remove_temporary_objects(const std::vector<std::string>& object_files) {
    for (const std::string& obj_path : object_files) {
        QFile::remove(QString::fromStdString(obj_path));
    }
}

namespace {

enum class run_phase {
    IDLE,
    BUILDING,
    RUNNING,
};

class c_run_session final : public extension_session {
public:
    explicit c_run_session(extension_host& host)
        : m_host(host),
          m_workspace_root(host.workspace_root()),
          m_parser(m_workspace_root.toStdString(), true),
          m_console(new ui::ui_log_panel({
              CONSOLE_MAX_LINES,
              CONSOLE_FLUSH_INTERVAL_MS,
              true,
              QStringLiteral("Send input to program..."),
              { QStringLiteral("LF") },
          })) {
        m_console->set_workspace_root(m_workspace_root);
        select_initial_target();
        wire_console_and_tasks();

        m_host.set_dock_panels({
            { QString::fromLatin1(DOCK_PANEL_ID), QStringLiteral("Run"), m_console, false, 0 },
        });
        register_commands();
        refresh_strip();
    }

    ~c_run_session() override {
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

        m_compile_task.on_stdout = nullptr;
        m_compile_task.on_stderr = nullptr;
        m_compile_task.on_finished = nullptr;

        m_run_task.on_stdout = nullptr;
        m_run_task.on_stderr = nullptr;
        m_run_task.on_finished = nullptr;

        m_compile_task.stop();
        m_run_task.stop();
        remove_temporary_objects(m_build_plan.object_files);
    }

    void on_active_file_changed(const QString& file_path) override {
        if (m_compile_all_sources) {
            return;
        }

        std::string resolved = resolve_workspace_file_path(
            m_workspace_root.toStdString(), file_path.toStdString(), true);
        if (resolved.empty() || !is_c_or_cpp_source_path(resolved)) {
            return;
        }

        QString candidate = QString::fromStdString(resolved);
        if (m_target_path == candidate) {
            return;
        }

        m_target_path = candidate;
        if (m_phase == run_phase::IDLE) {
            refresh_strip();
        }
    }

private:
    bool is_busy() const {
        return m_phase != run_phase::IDLE;
    }

    void select_initial_target() {
        std::string root_std = m_workspace_root.toStdString();
        QString active = m_host.active_file_path();
        std::string resolved_active = resolve_workspace_file_path(root_std, active.toStdString(), true);
        if (!resolved_active.empty() && is_c_or_cpp_source_path(resolved_active)) {
            m_target_path = QString::fromStdString(resolved_active);
            return;
        }

        std::vector<std::string> discovered = discover_c_run_targets(root_std);
        if (!discovered.empty()) {
            m_target_path = QString::fromStdString(discovered.front());
        }
    }

    void wire_console_and_tasks() {
        m_console->on_location_clicked = [this](const QString& path, int line) {
            open_diagnostic_location(path, line);
        };

        m_console->on_input_submitted = [this](const QByteArray& bytes) {
            if (m_phase == run_phase::RUNNING && m_run_task.is_running()) {
                m_run_task.write_stdin(bytes);
            }
        };

        m_compile_task.on_stdout = [this](QByteArrayView chunk) {
            handle_compiler_output(chunk);
        };

        m_compile_task.on_stderr = [this](QByteArrayView chunk) {
            handle_compiler_output(chunk);
        };

        m_compile_task.on_finished = [this](const process_exit& exit_result) {
            handle_compile_finished(exit_result);
        };

        m_run_task.on_stdout = [this](QByteArrayView chunk) {
            handle_program_output(chunk);
        };

        m_run_task.on_stderr = [this](QByteArrayView chunk) {
            handle_program_output(chunk);
        };

        m_run_task.on_finished = [this](const process_exit& exit_result) {
            handle_run_finished(exit_result);
        };
    }

    void register_commands() {
        m_host.set_commands({
            {
                QStringLiteral("c_run.build_and_run"),
                QStringLiteral("C & C++"),
                QStringLiteral("Build and Run"),
                QStringLiteral("Ctrl+R"),
                false,
                [this] { return !is_busy(); },
                [this] { start_build(true); },
            },
            {
                QStringLiteral("c_run.build"),
                QStringLiteral("C & C++"),
                QStringLiteral("Build"),
                QStringLiteral("Ctrl+Shift+B"),
                false,
                [this] { return !is_busy(); },
                [this] { start_build(false); },
            },
            {
                QStringLiteral("c_run.stop"),
                QStringLiteral("C & C++"),
                QStringLiteral("Stop"),
                QStringLiteral("Ctrl+Shift+R"),
                true,
                [this] { return is_busy(); },
                [this] { stop_active_task(); },
            },
        });
    }

    bool prepare_build_plan(compiler_build_plan* plan, QString* display_label, QString* error_message) {
        std::string root_std = m_workspace_root.toStdString();
        std::vector<std::string> sources;

        if (m_compile_all_sources) {
            sources = collect_project_c_cxx_sources(root_std);
        } else {
            if (m_target_path.isEmpty() || !QFileInfo::exists(m_target_path)) {
                m_target_path.clear();
                select_initial_target();
            }

            if (!m_target_path.isEmpty()) {
                sources.push_back(m_target_path.toStdString());
            }
        }

        if (sources.empty()) {
            *error_message = QStringLiteral("No C or C++ source files found in workspace.");
            return false;
        }

        compiler_environment env = probe_compiler_environment(m_workspace_root);
        *plan = build_compiler_plan(env, sources, m_compile_all_sources);
        if (plan->steps.empty()) {
            *error_message = QStringLiteral("Cannot construct compiler build plan.");
            return false;
        }

        QString build_dir = QString::fromStdString(plan->build_dir);
        if (!ensure_safe_build_directory(build_dir)) {
            *error_message = QStringLiteral("Cannot prepare safe build directory %1.").arg(build_dir);
            return false;
        }

        if (m_compile_all_sources) {
            *display_label = QStringLiteral("project (%1 files)").arg(sources.size());
        } else {
            *display_label = QString::fromStdString(
                relative_workspace_display_path(root_std, m_target_path.toStdString()));
        }

        return true;
    }

    void start_build(bool run_after_build) {
        if (m_is_shutting_down || !m_console || is_busy() || !m_host.save_modified_files()) {
            return;
        }

        compiler_build_plan plan;
        QString display_label;
        QString prep_error;
        if (!prepare_build_plan(&plan, &display_label, &prep_error)) {
            m_host.show_status_note(prep_error);
            return;
        }

        QFile::remove(QString::fromStdString(plan.output_binary));
        remove_temporary_objects(plan.object_files);

        m_compile_filter.reset();
        m_run_filter.reset();
        m_parser.reset();
        m_host.clear_diagnostics();
        m_console->clear_log();

        m_build_plan = std::move(plan);
        m_build_step_index = 0;
        m_output_binary = QString::fromStdString(m_build_plan.output_binary);
        m_run_after_build = run_after_build;
        m_status_readout.clear();
        m_status_is_danger = false;

        m_console->append_mark(QStringLiteral("Building %1").arg(display_label), false);
        m_phase = run_phase::BUILDING;
        m_host.reveal_dock_panel(QString::fromLatin1(DOCK_PANEL_ID));
        m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), true, 0);

        if (!start_current_compile_step()) {
            return;
        }

        refresh_strip();
    }

    bool start_current_compile_step() {
        const compiler_step& step = m_build_plan.steps.at(m_build_step_index);
        process_spec spec;
        spec.program = QString::fromStdString(step.program);
        spec.arguments = to_qstring_arguments(step.arguments);

        spec.working_directory = m_workspace_root;
        spec.io_mode = process_io_mode::PIPES;
        spec.opens_stdin = false;

        QString start_error;
        if (!m_compile_task.start(spec, &start_error)) {
            remove_temporary_objects(m_build_plan.object_files);
            m_phase = run_phase::IDLE;
            m_run_after_build = false;
            m_status_readout = QStringLiteral("Build failed");
            m_status_is_danger = true;

            m_console->append_mark(start_error, true);
            m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), false, 1);
            refresh_strip();
            return false;
        }

        return true;
    }

    void handle_compiler_output(QByteArrayView chunk) {
        if (m_is_shutting_down || !m_console) {
            return;
        }

        std::string_view raw(chunk.data(), static_cast<size_t>(chunk.size()));
        std::string sanitized = m_compile_filter.feed(raw);
        if (!sanitized.empty()) {
            m_console->append_bytes(QByteArrayView(sanitized.data(), static_cast<qsizetype>(sanitized.size())));
        }

        if (m_parser.feed(raw)) {
            sync_diagnostics_to_host();
            m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), is_busy(), m_parser.error_count());
        }
    }

    void handle_program_output(QByteArrayView chunk) {
        if (m_is_shutting_down || !m_console) {
            return;
        }

        std::string_view raw(chunk.data(), static_cast<size_t>(chunk.size()));
        std::string sanitized = m_run_filter.feed(raw);
        if (!sanitized.empty()) {
            m_console->append_bytes(QByteArrayView(sanitized.data(), static_cast<qsizetype>(sanitized.size())));
        }
    }

    void handle_compile_finished(const process_exit& exit_result) {
        if (m_is_shutting_down || !m_console) {
            return;
        }

        m_parser.finish();
        sync_diagnostics_to_host();

        if (exit_result.was_stopped) {
            remove_temporary_objects(m_build_plan.object_files);
            m_phase = run_phase::IDLE;
            m_run_after_build = false;
            m_status_readout = QStringLiteral("Stopped");
            m_status_is_danger = false;

            m_console->append_mark(QStringLiteral("Build stopped"), false);
            m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), false, m_parser.error_count());
            refresh_strip();
            return;
        }

        if (exit_result.exit_code != 0) {
            remove_temporary_objects(m_build_plan.object_files);
            finish_failed_build();
            return;
        }

        m_build_step_index++;
        if (m_build_step_index < m_build_plan.steps.size()) {
            start_current_compile_step();
            return;
        }

        remove_temporary_objects(m_build_plan.object_files);
        if (!m_run_after_build) {
            finish_build_only();
            return;
        }

        start_compiled_binary();
    }

    void finish_failed_build() {
        m_phase = run_phase::IDLE;
        m_run_after_build = false;

        int errors = m_parser.error_count();
        m_status_readout = errors > 0
                         ? (errors == 1 ? QStringLiteral("1 error") : QStringLiteral("%1 errors").arg(errors))
                         : QStringLiteral("Build failed");
        m_status_is_danger = true;
        m_console->append_mark(QStringLiteral("Build failed (%1)").arg(m_status_readout), true);
        m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), false, std::max(1, errors));
        refresh_strip();
    }

    void finish_build_only() {
        m_phase = run_phase::IDLE;
        int warnings = m_parser.warning_count();
        m_status_readout = warnings > 0
                         ? (warnings == 1 ? QStringLiteral("Built (1 warning)")
                                          : QStringLiteral("Built (%1 warnings)").arg(warnings))
                         : QStringLiteral("Built");
        m_status_is_danger = false;

        QString binary_name = QFileInfo(m_output_binary).fileName();
        m_console->append_mark(QStringLiteral("Built %1").arg(binary_name), false);
        m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), false, 0);
        refresh_strip();
    }

    void start_compiled_binary() {
        m_run_after_build = false;
        m_run_filter.reset();

        QString binary_name = QFileInfo(m_output_binary).fileName();
        m_console->append_mark(QStringLiteral("Running %1").arg(binary_name), false);

        process_spec spec;
        spec.program = m_output_binary;
        spec.working_directory = m_workspace_root;
        spec.io_mode = process_io_mode::PTY;
        spec.opens_stdin = true;

        QString start_error;
        if (!m_run_task.start(spec, &start_error)) {
            m_phase = run_phase::IDLE;
            m_status_readout = QStringLiteral("Run failed");
            m_status_is_danger = true;

            m_console->append_mark(start_error, true);
            m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), false, 1);
            refresh_strip();
            return;
        }

        m_phase = run_phase::RUNNING;
        m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), true, 0);
        refresh_strip();
    }

    void handle_run_finished(const process_exit& exit_result) {
        if (m_is_shutting_down || !m_console) {
            return;
        }

        m_phase = run_phase::IDLE;
        if (exit_result.was_stopped) {
            m_status_readout = QStringLiteral("Stopped");
            m_status_is_danger = false;
            m_console->append_mark(QStringLiteral("Stopped"), false);
            m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), false, 0);
            refresh_strip();
            return;
        }

        bool failed = exit_result.exit_code != 0;
        m_status_is_danger = failed;
        m_status_readout = failed ? QStringLiteral("Exit %1").arg(exit_result.exit_code)
                                  : QStringLiteral("Ready");

        QString mark_text = exit_result.term_signal > 0
                          ? QStringLiteral("Terminated by signal %1 (exit %2)")
                                .arg(exit_result.term_signal)
                                .arg(exit_result.exit_code)
                          : QStringLiteral("Exited with code %1").arg(exit_result.exit_code);
        m_console->append_mark(mark_text, failed);
        m_host.update_dock_state(QString::fromLatin1(DOCK_PANEL_ID), false, failed ? 1 : 0);
        refresh_strip();
    }

    void stop_active_task() {
        if (m_is_shutting_down) {
            return;
        }

        m_run_after_build = false;
        if (m_compile_task.is_running()) {
            m_compile_task.stop();
        }

        if (m_run_task.is_running()) {
            m_run_task.stop();
        }

        refresh_strip();
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

        QString open_target = resolved.empty() ? path : QString::fromStdString(resolved);
        m_host.open_file_at_line(open_target, line, column);
    }

    QList<picker_choice> list_target_choices() const {
        QList<picker_choice> choices;
        std::string root_std = m_workspace_root.toStdString();
        std::vector<std::string> targets = discover_c_run_targets(root_std);

        for (const std::string& full_path : targets) {
            std::string rel = relative_workspace_display_path(root_std, full_path);
            QFileInfo relative_file(QString::fromStdString(rel));

            picker_choice choice;
            choice.id = QString::fromStdString(full_path);
            choice.label = relative_file.fileName();
            if (relative_file.path() != QLatin1String(".")) {
                choice.detail = relative_file.path();
            }

            choices.append(choice);
        }

        std::vector<std::string> project_sources = collect_project_c_cxx_sources(root_std);
        if (!project_sources.empty()) {
            picker_choice all_choice;
            all_choice.id = QString::fromLatin1(ALL_SOURCES_CHOICE_ID);
            all_choice.label = QStringLiteral("All Project Sources");
            all_choice.detail = project_sources.size() == 1
                              ? QStringLiteral("1 file")
                              : QStringLiteral("%1 files").arg(project_sources.size());
            choices.append(all_choice);
        }

        return choices;
    }

    void select_target_choice(const QString& choice_id) {
        if (choice_id == QLatin1String(ALL_SOURCES_CHOICE_ID)) {
            m_compile_all_sources = true;
            refresh_strip();
            return;
        }

        m_compile_all_sources = false;
        m_target_path = choice_id;
        refresh_strip();
    }

    QString current_target_label() const {
        if (m_compile_all_sources) {
            return QStringLiteral("All Project Sources");
        }

        if (m_target_path.isEmpty()) {
            return QStringLiteral("No Target");
        }

        return QFileInfo(m_target_path).fileName();
    }

    QString current_target_detail() const {
        if (m_compile_all_sources || m_target_path.isEmpty()) {
            return {};
        }

        std::string rel = relative_workspace_display_path(
            m_workspace_root.toStdString(), m_target_path.toStdString());
        QFileInfo relative_file(QString::fromStdString(rel));

        return relative_file.path() == QLatin1String(".") ? QString() : relative_file.path();
    }

    QString current_readout_label() const {
        if (m_phase == run_phase::BUILDING) {
            return QStringLiteral("Building...");
        }

        if (m_phase == run_phase::RUNNING) {
            return QStringLiteral("Running");
        }

        return m_status_readout.isEmpty() ? QStringLiteral("Ready") : m_status_readout;
    }

    void refresh_strip() {
        if (m_is_shutting_down) {
            return;
        }

        bool busy = is_busy();

        picker_spec target_picker;
        target_picker.id = QStringLiteral("target");
        target_picker.label = current_target_label();
        target_picker.detail = current_target_detail();
        target_picker.search_placeholder = QStringLiteral("Filter C/C++ targets...");

        target_picker.is_enabled = !busy;
        target_picker.list_choices = [this] { return list_target_choices(); };
        target_picker.on_selected = [this](const QString& choice_id) { select_target_choice(choice_id); };

        QList<button_spec> buttons = {
            {
                QStringLiteral("build_and_run"),
                QStringLiteral("Build & Run"),
                QStringLiteral("Compile and run target (Ctrl+R)"),
                !busy,
                (m_phase == run_phase::BUILDING && m_run_after_build) || m_phase == run_phase::RUNNING,
                [this] { start_build(true); },
            },
            {
                QStringLiteral("build"),
                QStringLiteral("Build"),
                QStringLiteral("Compile target (Ctrl+Shift+B)"),
                !busy,
                m_phase == run_phase::BUILDING && !m_run_after_build,
                [this] { start_build(false); },
            },
            {
                QStringLiteral("stop"),
                QStringLiteral("Stop"),
                QStringLiteral("Stop build or running program (Ctrl+Shift+R)"),
                busy,
                false,
                [this] { stop_active_task(); },
            },
        };

        readout_spec state_readout;
        state_readout.id = QStringLiteral("state");
        state_readout.label = current_readout_label();
        state_readout.progress_percent = -1;
        state_readout.is_danger = !busy && m_status_is_danger;

        strip_spec strip;
        strip.pickers = { target_picker };
        strip.buttons = buttons;
        strip.readouts = { state_readout };
        strip.focus_extensions = {
            QStringLiteral(".c"),
            QStringLiteral(".cpp"),
            QStringLiteral(".cc"),
            QStringLiteral(".cxx"),
            QStringLiteral(".h"),
            QStringLiteral(".hpp"),
        };

        m_host.set_strip(strip);
    }

    extension_host& m_host;
    QString m_workspace_root;
    terminal_escape_filter m_compile_filter;
    terminal_escape_filter m_run_filter;
    compiler_output_parser m_parser;
    ui::ui_log_panel* m_console = nullptr;
    process_task m_compile_task;
    process_task m_run_task;
    compiler_build_plan m_build_plan;
    size_t m_build_step_index = 0;
    QString m_target_path;
    QString m_output_binary;
    QString m_status_readout;
    run_phase m_phase = run_phase::IDLE;
    bool m_compile_all_sources = false;
    bool m_run_after_build = false;
    bool m_status_is_danger = false;
    bool m_is_shutting_down = false;
};

} // namespace

static bool qualifies_c_run(const workspace_files& files) {
    return files.has_suffix(QStringLiteral(".c"))
        || files.has_suffix(QStringLiteral(".cpp"))
        || files.has_suffix(QStringLiteral(".cc"))
        || files.has_suffix(QStringLiteral(".cxx"))
        || files.has_suffix(QStringLiteral(".h"))
        || files.has_suffix(QStringLiteral(".hpp"));
}

static std::unique_ptr<extension_session> create_c_run_session(extension_host& host) {
    return std::make_unique<c_run_session>(host);
}

extension_descriptor c_run_extension() {
    return {
        "c_run",
        "C & C++",
        "Compile and run C and C++ programs with inline compiler diagnostics.",
        extension_icon::CODE,
        nullptr,
        false,
        C_RUN_DEPS,
        {},
        qualifies_c_run,
        create_c_run_session,
    };
}

} // namespace sheen
