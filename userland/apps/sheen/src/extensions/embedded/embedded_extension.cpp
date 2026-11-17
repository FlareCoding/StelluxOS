#include "extensions/embedded/embedded_extension.h"

#include "extensions/embedded/embedded_protocol.h"
#include "extensions/extension_host.h"
#include "ui/chrome.h"
#include "ui/ui_kit.h"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace sheen {

static constexpr const char* MCUFW_PROGRAM = "/bin/mcufw";
static constexpr const char* MCUFW_LIBRARIES_DIR = "/var/lib/mcufw/libraries";
static constexpr const char* MCUFW_HARDWARE_DIR = "/var/lib/mcufw/hardware";
static constexpr const char* INSTALL_CORE_ACTION_ID = "__install_core__";
static constexpr const char* INSTALL_SAVED_BOARD_ACTION_ID = "__install_saved_board__";
static constexpr const char* MISSING_LIB_ROW_PREFIX = "missing:";

static constexpr int TARGET_POPOVER_WIDTH = 352;
static constexpr int TARGET_POPOVER_HEIGHT = 46;
static constexpr int TARGET_POPOVER_INSET_X = 10;
static constexpr int TARGET_POPOVER_GAP = 8;

static constexpr package_requirement EMBEDDED_PACKAGES[] = {
    { "python", EMBEDDED_PYTHON_PROBE_PATH },
    { "esptool", EMBEDDED_ESPTOOL_PROBE_PATH },
    { "xtensa-esp32-elf", EMBEDDED_XTENSA_GPP_PROBE_PATH },
};

static constexpr const char* const CORE_INSTALL_ARGV[] = {
    MCUFW_PROGRAM, "install", EMBEDDED_DEFAULT_CORE_TARGET, "--events",
};

static constexpr dependency_group EMBEDDED_DEPS[] = {
    { "esp32-tools", "ESP32 Toolchain and Flasher",
      "Cross-compiler, Python runtime, and serial flash utility.",
      EMBEDDED_PACKAGES, {}, nullptr, event_protocol::NONE, true },
    { "esp32-core", "ESP32 Arduino Core",
      "Board definitions, SDK libraries, and partition tables.",
      {}, CORE_INSTALL_ARGV, EMBEDDED_ESP32_LIBS_PROBE_PATH, event_protocol::TSV_MCUFW, true },
};

enum class active_run_kind { NONE, BUILD, INSTALL, CLEAN };

namespace {

/* Compact Razor target-entry popover for `mcufw install <target>` with focus rule and Escape dismissal. */
class install_target_popover : public ui::ui_popover {
public:
    install_target_popover(QWidget* anchor, const QString& placeholder)
        : ui::ui_popover(anchor, TARGET_POPOVER_WIDTH, TARGET_POPOVER_HEIGHT),
          m_input(new QLineEdit(this)),
          m_install_button(new ui::ui_pill_button(QStringLiteral("Install"), this)) {
        setAttribute(Qt::WA_DeleteOnClose, true);

        m_input->setFont(ui_font(STLXTHEME_BODY_SIZE));
        m_input->setFrame(false);
        m_input->setPlaceholderText(placeholder);
        m_input->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; color: %1; border: none; }")
                                   .arg(token(STLXTHEME_TEXT).name()));
        m_input->installEventFilter(this);

        m_install_button->setEnabled(false);

        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(TARGET_POPOVER_INSET_X, 1, TARGET_POPOVER_INSET_X, STLXTHEME_FOCUS_RULE_WIDTH + 1);
        row->setSpacing(TARGET_POPOVER_GAP);
        row->addWidget(m_input, 1);
        row->addWidget(m_install_button, 0, Qt::AlignVCenter);

        QObject::connect(m_input, &QLineEdit::textChanged, this, [this](const QString& text) {
            m_install_button->setEnabled(!text.trimmed().isEmpty());
        });

        QObject::connect(m_input, &QLineEdit::returnPressed, this, [this] { accept_target(); });
        QObject::connect(m_install_button, &QAbstractButton::clicked, this, [this] { accept_target(); });

        m_input->setFocus();
    }

    std::function<void(const QString& target)> on_target_chosen;

protected:
    void paintEvent(QPaintEvent* event) override {
        ui::ui_popover::paintEvent(event);
        if (m_input->hasFocus()) {
            QPainter painter(this);
            painter.fillRect(QRect(TARGET_POPOVER_INSET_X, height() - STLXTHEME_FOCUS_RULE_WIDTH - 1,
                                   width() - TARGET_POPOVER_INSET_X * 2, STLXTHEME_FOCUS_RULE_WIDTH),
                             token(STLXTHEME_ACCENT));
        }
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_input && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
            update();
        }

        if (watched == m_input && event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) {
                close();
                return true;
            }
        }

        return ui::ui_popover::eventFilter(watched, event);
    }

private:
    void accept_target() {
        QString target = m_input->text().trimmed();
        if (target.isEmpty()) {
            return;
        }

        std::function<void(const QString&)> callback = on_target_chosen;
        close();
        if (callback) {
            callback(target);
        }
    }

    QLineEdit* m_input = nullptr;
    ui::ui_pill_button* m_install_button = nullptr;
};

class embedded_session : public extension_session {
public:
    explicit embedded_session(extension_host& host)
        : m_host(host),
          m_libraries(new ui::ui_section_list(QStringLiteral("Install..."))),
          m_build_log(new ui::ui_log_panel({})),
          m_run_events(event_protocol::TSV_MCUFW) {
        m_sketch_dir = QString::fromStdString(resolve_sketch_directory(m_host.workspace_root().toStdString()));
        m_build_log->set_workspace_root(m_sketch_dir);
        load_saved_sketch_defaults();

        wire_panels_and_tasks();

        m_host.set_sidebar_sections({ { QStringLiteral("libraries"), QStringLiteral("Libraries"), m_libraries } });
        m_host.set_dock_panels({ { QStringLiteral("build"), QStringLiteral("Build"), m_build_log, false, 0 } });

        register_commands();
        refresh_libraries_sidebar();
        refresh_board_catalog();
        refresh_strip();
    }

    ~embedded_session() override {
        shutdown();
    }

    void shutdown() override {
        if (m_is_shutting_down) {
            return;
        }

        m_is_shutting_down = true;
        m_rebuild_after_install = false;

        if (m_open_popover) {
            m_open_popover->close();
            m_open_popover.clear();
        }

        m_boards_task.on_stdout = nullptr;
        m_boards_task.on_stderr = nullptr;
        m_boards_task.on_finished = nullptr;

        m_run_task.on_stdout = nullptr;
        m_run_task.on_stderr = nullptr;
        m_run_task.on_finished = nullptr;

        m_libraries = nullptr;
        m_build_log = nullptr;

        m_boards_task.stop();
        m_run_task.stop();
    }

    void on_active_file_changed(const QString& file_path) override {
        if (!file_path.endsWith(QLatin1String(".ino")) && !file_path.endsWith(QLatin1String(".pde"))) {
            return;
        }

        if (is_run_busy()) {
            return;
        }

        QString candidate = QString::fromStdString(resolve_sketch_directory(file_path.toStdString()));
        if (candidate.isEmpty() || candidate == m_sketch_dir) {
            return;
        }

        if (!is_path_within_workspace(candidate.toStdString(), m_host.workspace_root().toStdString())) {
            return;
        }

        m_sketch_dir = candidate;
        if (m_build_log != nullptr) {
            m_build_log->set_workspace_root(m_sketch_dir);
        }

        m_board_fqbn.clear();
        m_board_name.clear();
        m_has_explicit_board = false;
        m_diagnostics.clear();
        m_build_libraries.clear();
        m_memory_usage = {};

        m_status_summary.clear();
        m_last_error_summary.clear();
        m_host.clear_diagnostics();
        m_host.update_dock_state(QStringLiteral("build"), false, 0);

        load_saved_sketch_defaults();
        sync_board_name_from_catalog();
        refresh_libraries_sidebar();
        refresh_strip();
    }

private:
    bool is_run_busy() const {
        return m_run_task.is_running();
    }

    void load_saved_sketch_defaults() {
        sketch_defaults defaults = read_sketch_defaults(m_sketch_dir.toStdString());
        m_has_explicit_board = false;
        if (!defaults.fqbn.empty()) {
            m_board_fqbn = QString::fromStdString(defaults.fqbn);
        }
    }

    void wire_panels_and_tasks() {
        m_libraries->set_empty_text(QStringLiteral("No libraries installed"));
        m_libraries->on_action_clicked = [this] { open_library_install_popover(); };
        m_libraries->on_row_activated = [this](const QString& row_id) { activate_library_row(row_id); };
        m_build_log->on_location_clicked = [this](const QString& p, int l) { open_resolved_location(p, l); };

        m_boards_task.on_stdout = [this](QByteArrayView chunk) {
            if (!m_is_shutting_down) {
                m_board_catalog.feed_stdout(std::string_view(chunk.data(), static_cast<size_t>(chunk.size())));
            }
        };

        m_boards_task.on_finished = [this](const process_exit& result) {
            if (!m_is_shutting_down && m_board_catalog.finish_refresh(result.exit_code, result.was_stopped)) {
                sync_board_name_from_catalog();
                refresh_strip();
            }
        };

        m_run_task.on_stdout = [this](QByteArrayView chunk) {
            if (!m_is_shutting_down) {
                m_run_events.feed(std::string_view(chunk.data(), static_cast<size_t>(chunk.size())),
                                  [this](const std::vector<std::string>& fields) { handle_mcufw_event(fields); });
            }
        };

        m_run_task.on_stderr = [this](QByteArrayView chunk) {
            if (!m_is_shutting_down && m_build_log != nullptr) {
                m_build_log->append_bytes(chunk);
            }
        };

        m_run_task.on_finished = [this](const process_exit& result) { on_run_finished(result); };
    }

    void register_commands() {
        m_host.set_commands({
            { QStringLiteral("embedded.build"), QStringLiteral("Embedded"), QStringLiteral("Build"),
              QStringLiteral("Ctrl+B"), false, [this] { return !is_run_busy(); }, [this] { run_build(); } },
            { QStringLiteral("embedded.stop"), QStringLiteral("Embedded"), QStringLiteral("Stop"),
              QStringLiteral("Shift+F5"), true, [this] { return is_run_busy(); }, [this] { cancel_active_run(); } },
            { QStringLiteral("embedded.install_library"), QStringLiteral("Embedded"),
              QStringLiteral("Install Library..."), QString(), true,
              [this] { return !is_run_busy(); }, [this] { open_library_install_popover(); } },
            { QStringLiteral("embedded.install_core"), QStringLiteral("Embedded"),
              QStringLiteral("Install Core..."), QString(), false,
              [this] { return !is_run_busy(); }, [this] { open_core_install_popover(false); } },
            { QStringLiteral("embedded.clean"), QStringLiteral("Embedded"),
              QStringLiteral("Clean Build"), QString(), false,
              [this] { return !is_run_busy(); }, [this] { run_clean_build(); } },
        });
    }

    void open_resolved_location(const QString& raw_path, int line) {
        int target_line = std::max(1, line);
        if (QFileInfo(raw_path).isRelative()) {
            QString candidate = QDir(m_sketch_dir).filePath(raw_path);
            if (QFileInfo::exists(candidate)) {
                m_host.open_file_at_line(candidate, target_line);
                return;
            }
        }

        m_host.open_file_at_line(raw_path, target_line);
    }

    void refresh_board_catalog() {
        if (m_boards_task.is_running() || m_is_shutting_down) {
            return;
        }

        m_board_catalog.begin_refresh();
        process_spec spec = { QString::fromLatin1(MCUFW_PROGRAM),
                              { QStringLiteral("boards"), QStringLiteral("--events") },
                              m_sketch_dir, process_io_mode::PIPES, false };
        QString ignored_error;
        m_boards_task.start(spec, &ignored_error);
    }

    void sync_board_name_from_catalog() {
        if (m_board_fqbn.isEmpty()) {
            m_board_name.clear();
            return;
        }

        const board_entry* found = m_board_catalog.find_by_fqbn(m_board_fqbn.toStdString());
        if (found != nullptr) {
            m_board_name = QString::fromStdString(found->name);
        }
    }

    bool is_saved_board_in_catalog() const {
        return m_board_fqbn.isEmpty() || m_board_catalog.find_by_fqbn(m_board_fqbn.toStdString()) != nullptr;
    }

    QString board_button_label() const {
        if (!m_board_name.isEmpty()) {
            return m_board_name;
        }

        return !m_board_fqbn.isEmpty() ? m_board_fqbn : QStringLiteral("Select Board");
    }

    QList<picker_choice> build_board_choices() const {
        QList<picker_choice> choices;
        if (!m_board_fqbn.isEmpty() && !is_saved_board_in_catalog()) {
            choices.push_back({ QString::fromLatin1(INSTALL_SAVED_BOARD_ACTION_ID),
                                QStringLiteral("Install Core for %1").arg(m_board_fqbn),
                                QStringLiteral("mcufw install"), QStringLiteral("Missing"), true, false });
        }

        for (const board_entry& entry : m_board_catalog.boards()) {
            QString fqbn = QString::fromStdString(entry.fqbn);
            QString badge = entry.needs.empty()
                ? QString()
                : QStringLiteral("needs %1").arg(QString::fromStdString(entry.needs));
            choices.push_back({ fqbn, QString::fromStdString(entry.name), fqbn, badge, false, false });
        }

        choices.push_back({ QString::fromLatin1(INSTALL_CORE_ACTION_ID), QStringLiteral("+ Install Core..."),
                            QStringLiteral("mcufw install"), QString(), true, false });
        return choices;
    }

    void select_board_choice(const QString& choice_id) {
        if (choice_id == QLatin1String(INSTALL_CORE_ACTION_ID)) {
            open_core_install_popover(false);
            return;
        }

        if (choice_id == QLatin1String(INSTALL_SAVED_BOARD_ACTION_ID)) {
            if (!m_board_fqbn.isEmpty()) {
                start_install_target(m_board_fqbn, false);
            }

            return;
        }

        m_board_fqbn = choice_id;
        m_has_explicit_board = true;
        sync_board_name_from_catalog();
        refresh_strip();
    }

    readout_spec build_status_readout() const {
        if (is_run_busy()) {
            QString label = m_progress_label.isEmpty() ? QStringLiteral("Running...") : m_progress_label;
            return { QStringLiteral("status"), label, m_progress_percent, false };
        }

        if (!m_last_error_summary.isEmpty()) {
            return { QStringLiteral("status"), m_last_error_summary, -1, true };
        }

        std::string memory_text = format_memory_summary(m_memory_usage);
        if (!memory_text.empty()) {
            return { QStringLiteral("status"), QString::fromStdString(memory_text), -1,
                     is_memory_usage_danger(m_memory_usage) };
        }

        QString summary = m_status_summary.isEmpty() ? QStringLiteral("Ready") : m_status_summary;
        return { QStringLiteral("status"), summary, -1, false };
    }

    void refresh_strip() {
        if (m_is_shutting_down) {
            return;
        }

        bool busy = is_run_busy();
        QString board_detail = !m_board_name.isEmpty() ? m_board_fqbn : QString();
        QList<button_spec> buttons = {
            { QStringLiteral("build"), QStringLiteral("Build"), QStringLiteral("Compile sketch (Ctrl+B)"),
              !busy, busy && m_active_run == active_run_kind::BUILD, [this] { run_build(); } },
        };

        if (busy) {
            buttons.push_back({ QStringLiteral("stop"), QStringLiteral("Stop"),
                                QStringLiteral("Stop active task (Shift+F5)"), true, false,
                                [this] { cancel_active_run(); } });
        }

        strip_spec strip;
        strip.pickers = {
            { QStringLiteral("board"), board_button_label(), board_detail,
              QStringLiteral("Filter boards by name or FQBN..."), !busy,
              [this] { return build_board_choices(); },
              [this](const QString& id) { select_board_choice(id); } },
        };

        strip.buttons = std::move(buttons);
        strip.readouts = { build_status_readout() };
        strip.focus_extensions = { QStringLiteral(".ino"), QStringLiteral(".pde") };
        m_host.set_strip(strip);
    }

    void refresh_libraries_sidebar() {
        if (m_is_shutting_down || m_libraries == nullptr) {
            return;
        }

        m_installed_libraries = scan_installed_libraries(MCUFW_LIBRARIES_DIR, MCUFW_HARDWARE_DIR);

        QList<ui::section_row> rows;
        QSet<QString> seen_names;
        for (const embedded_diagnostic& diag : m_diagnostics) {
            QString missing_name = QString::fromStdString(diag.fix_library);
            if (!missing_name.isEmpty() && !seen_names.contains(missing_name)) {
                seen_names.insert(missing_name);
                rows.push_back({ QString::fromLatin1(MISSING_LIB_ROW_PREFIX) + missing_name, missing_name,
                                 QStringLiteral("missing"), QStringLiteral("Click to install %1").arg(missing_name) });
            }
        }

        auto append_library_rows = [&](const std::vector<installed_library>& list) {
            for (const installed_library& item : list) {
                QString name = QString::fromStdString(item.name);
                if (!seen_names.contains(name)) {
                    seen_names.insert(name);
                    QString directory = QString::fromStdString(item.directory);
                    rows.push_back({ directory, name, QString::fromStdString(item.version), directory });
                }
            }
        };

        append_library_rows(m_build_libraries);
        append_library_rows(m_installed_libraries);
        m_libraries->set_rows(rows);
    }

    void activate_library_row(const QString& row_id) {
        QString missing_prefix = QString::fromLatin1(MISSING_LIB_ROW_PREFIX);
        if (row_id.startsWith(missing_prefix)) {
            QString lib_name = row_id.mid(missing_prefix.size());
            if (!lib_name.isEmpty()) {
                install_library_and_rebuild(lib_name);
            }

            return;
        }

        QFileInfo props(QDir(row_id).filePath(QStringLiteral("library.properties")));
        if (props.exists()) {
            m_host.open_file_at_line(props.absoluteFilePath(), 1);
            return;
        }

        QFileInfo platform(QDir(row_id).filePath(QStringLiteral("platform.txt")));
        if (platform.exists()) {
            m_host.open_file_at_line(platform.absoluteFilePath(), 1);
        }
    }

    QWidget* resolve_popover_anchor(bool prefer_sidebar) const {
        if (prefer_sidebar && m_libraries != nullptr && m_libraries->isVisible()) {
            return m_libraries;
        }

        QWidget* win = m_build_log != nullptr ? m_build_log->window() : nullptr;
        if (win != nullptr) {
            QString expected = board_button_label();
            QWidget* first_visible_pill = nullptr;
            const QList<QAbstractButton*> buttons = win->findChildren<QAbstractButton*>();
            for (QAbstractButton* button : buttons) {
                if (button == nullptr || !button->isVisible()) {
                    continue;
                }

                if (first_visible_pill == nullptr && button->height() == STLXTHEME_TOOLBAR_CONTROL_HEIGHT) {
                    first_visible_pill = button;
                }

                if (button->text() == expected) {
                    return button;
                }
            }

            if (first_visible_pill != nullptr) {
                return first_visible_pill;
            }
        }

        return m_libraries != nullptr && m_libraries->isVisible() ? m_libraries : win;
    }

    void show_install_popover(QWidget* anchor, const QString& placeholder) {
        if (is_run_busy() || m_is_shutting_down) {
            return;
        }

        if (m_open_popover) {
            m_open_popover->close();
            m_open_popover.clear();
        }

        auto* popover = new install_target_popover(anchor, placeholder);
        m_open_popover = popover;
        popover->on_target_chosen = [this](const QString& target) {
            if (!m_is_shutting_down) {
                start_install_target(target, false);
            }
        };

        popover->open_below_anchor();
    }

    void open_library_install_popover() {
        show_install_popover(resolve_popover_anchor(true), QStringLiteral("Library name or name@version..."));
    }

    void open_core_install_popover(bool prefer_sidebar) {
        show_install_popover(resolve_popover_anchor(prefer_sidebar),
                             QStringLiteral("Core or board FQBN (packager:arch[:board])..."));
    }

    void install_library_and_rebuild(const QString& library_name) {
        start_install_target(library_name, true);
    }

    void start_install_target(const QString& target, bool rebuild_after) {
        QString cleaned = target.trimmed();
        if (cleaned.isEmpty() || is_run_busy() || m_is_shutting_down) {
            return;
        }

        m_rebuild_after_install = rebuild_after;
        m_last_error_summary.clear();
        m_progress_label = QStringLiteral("Installing %1...").arg(cleaned);
        m_progress_percent = 0;

        if (m_build_log != nullptr) {
            m_build_log->clear_log();
            m_build_log->append_mark(QStringLiteral("mcufw install %1").arg(cleaned), false);
        }

        start_mcufw_run(active_run_kind::INSTALL,
                        { QStringLiteral("install"), cleaned, QStringLiteral("--events") });
    }

    void run_clean_build() {
        if (is_run_busy() || m_is_shutting_down) {
            return;
        }

        m_last_error_summary.clear();
        m_memory_usage = {};
        m_status_summary.clear();
        m_progress_label = QStringLiteral("Cleaning...");
        m_progress_percent = -1;

        if (m_build_log != nullptr) {
            m_build_log->clear_log();
        }

        start_mcufw_run(active_run_kind::CLEAN,
                        { QStringLiteral("clean"), m_sketch_dir, QStringLiteral("--events") });
    }

    void run_build() {
        if (is_run_busy() || m_is_shutting_down || !m_host.save_modified_files()) {
            return;
        }

        if (m_board_fqbn.isEmpty()) {
            load_saved_sketch_defaults();
            sync_board_name_from_catalog();
        }

        m_host.clear_diagnostics();
        m_diagnostics.clear();
        m_build_libraries.clear();
        m_memory_usage = {};

        m_last_error_summary.clear();
        m_status_summary.clear();
        m_progress_label = QStringLiteral("Building...");
        m_progress_percent = 0;

        if (m_build_log != nullptr) {
            m_build_log->clear_log();
        }

        refresh_libraries_sidebar();

        QStringList args = { QStringLiteral("build"), m_sketch_dir, QStringLiteral("--events") };
        if (m_has_explicit_board && !m_board_fqbn.isEmpty()) {
            args << QStringLiteral("--board") << m_board_fqbn;
        }

        start_mcufw_run(active_run_kind::BUILD, args);
    }

    void start_mcufw_run(active_run_kind kind, const QStringList& args) {
        m_active_run = kind;
        m_run_events = event_line_stream(event_protocol::TSV_MCUFW);
        m_saw_run_failure = false;

        process_spec spec = { QString::fromLatin1(MCUFW_PROGRAM), args, m_sketch_dir, process_io_mode::PIPES, false };
        QString start_error;
        if (!m_run_task.start(spec, &start_error)) {
            m_active_run = active_run_kind::NONE;
            m_last_error_summary = QStringLiteral("Launch failed");
            append_build_mark(start_error, true);

            m_host.reveal_dock_panel(QStringLiteral("build"));
            m_host.update_dock_state(QStringLiteral("build"), false, 1);
            refresh_strip();
            return;
        }

        m_host.reveal_dock_panel(QStringLiteral("build"));
        m_host.update_dock_state(QStringLiteral("build"), true, 0);
        refresh_strip();
    }

    void cancel_active_run() {
        m_rebuild_after_install = false;
        if (m_run_task.is_running()) {
            m_run_task.stop();
        }
    }

    void sync_host_diagnostics() {
        QList<diagnostic_item> items;
        for (const embedded_diagnostic& diag : m_diagnostics) {
            diagnostic_item item;
            item.file_path = QString::fromStdString(diag.file_path);
            item.line = diag.line;
            item.column = diag.column;

            item.severity = diag.is_warning ? diagnostic_severity::WARNING : diagnostic_severity::ERROR;
            item.message = QString::fromStdString(diag.message);
            if (!diag.fix_library.empty()) {
                QString lib = QString::fromStdString(diag.fix_library);
                item.fix = {
                    QStringLiteral("Install %1").arg(lib),
                    [this, lib] {
                        if (!m_is_shutting_down) {
                            install_library_and_rebuild(lib);
                        }
                    },
                };
            }

            items.push_back(std::move(item));
        }

        m_host.set_diagnostics(items);
    }

    int error_diagnostic_count() const {
        int errors = 0;
        for (const embedded_diagnostic& diag : m_diagnostics) {
            if (!diag.is_warning) {
                errors++;
            }
        }

        return errors;
    }

    void append_build_mark(const QString& caption, bool is_danger) {
        if (!m_is_shutting_down && m_build_log != nullptr) {
            m_build_log->append_mark(caption, is_danger);
        }
    }

    void handle_mcufw_event(const std::vector<std::string>& fields) {
        if (m_is_shutting_down) {
            return;
        }

        mcufw_event event = parse_mcufw_event_fields(fields);
        switch (event.kind) {
        case mcufw_event_kind::BOARD:
            if (m_board_fqbn.isEmpty()) {
                m_board_fqbn = QString::fromStdString(base_fqbn(event.fqbn));
            }

            if (!event.name.empty()) {
                m_board_name = QString::fromStdString(event.name);
            }

            refresh_strip();
            return;

        case mcufw_event_kind::LIBRARY:
            m_build_libraries.push_back({ event.name, event.version, event.folder, false });
            refresh_libraries_sidebar();
            return;

        case mcufw_event_kind::PROBLEM:
            if (append_problem_diagnostic(m_diagnostics, event)) {
                sync_host_diagnostics();
            }

            return;

        case mcufw_event_kind::HINT:
            if (attach_library_hint(m_diagnostics, event, m_sketch_dir.toStdString())) {
                sync_host_diagnostics();
                refresh_libraries_sidebar();
            }

            return;

        case mcufw_event_kind::SIZE:
            record_memory_size(m_memory_usage, event);
            refresh_strip();
            return;

        case mcufw_event_kind::PROGRESS:
            m_progress_percent = compute_progress_percent(event.done, event.total);
            m_progress_label = QString::fromStdString(
                format_progress_label(event.stage, event.done, event.total, m_active_run == active_run_kind::INSTALL));
            refresh_strip();
            return;

        case mcufw_event_kind::START:
            m_progress_percent = 0;
            m_progress_label = QStringLiteral("Installing %1 %2")
                                   .arg(QString::fromStdString(event.name), QString::fromStdString(event.version));
            append_build_mark(m_progress_label, false);
            refresh_strip();
            return;

        case mcufw_event_kind::INSTALLED:
            append_build_mark(
                QStringLiteral("Installed %1 %2")
                    .arg(QString::fromStdString(event.name), QString::fromStdString(event.version)),
                false);
            return;

        case mcufw_event_kind::NEEDS: {
            QString needed = event.version.empty()
                ? QString::fromStdString(event.name)
                : QStringLiteral("%1 %2").arg(QString::fromStdString(event.name),
                                              QString::fromStdString(event.version));
            append_build_mark(QStringLiteral("Needs %1").arg(needed), true);
            return;
        }

        case mcufw_event_kind::LOG:
            if (m_build_log != nullptr) {
                QByteArray line = QByteArray::fromStdString(event.message + "\n");
                m_build_log->append_bytes(line);
            }

            return;

        case mcufw_event_kind::WARNING:
            append_build_mark(QString::fromStdString(event.reason), false);
            return;

        case mcufw_event_kind::BUILT:
            append_build_mark(QStringLiteral("Built %1").arg(QString::fromStdString(event.file_path)), false);
            m_status_summary = QStringLiteral("Built");
            return;

        case mcufw_event_kind::FAILED:
            handle_mcufw_failure_event(event);
            return;

        default:
            return;
        }
    }

    void handle_mcufw_failure_event(const mcufw_event& event) {
        m_saw_run_failure = true;
        int errors = error_diagnostic_count();
        if (errors > 0) {
            m_last_error_summary = errors == 1
                ? QStringLiteral("1 error")
                : QStringLiteral("%1 errors").arg(errors);
        } else if (event.failure_kind == "cancelled") {
            m_last_error_summary.clear();
            m_status_summary = QStringLiteral("Cancelled");
        } else {
            m_last_error_summary = QStringLiteral("%1 failed").arg(QString::fromStdString(event.failure_kind));
        }

        append_build_mark(QString::fromStdString(event.reason), event.failure_kind != "cancelled");
    }

    void on_run_finished(const process_exit& result) {
        if (m_is_shutting_down) {
            return;
        }

        m_run_events.flush([this](const std::vector<std::string>& fields) { handle_mcufw_event(fields); });

        active_run_kind finished_kind = m_active_run;
        m_active_run = active_run_kind::NONE;
        m_progress_percent = -1;
        m_progress_label.clear();

        bool succeeded = result.exit_code == 0 && !result.was_stopped && !m_saw_run_failure;
        if (finished_kind == active_run_kind::BUILD && succeeded) {
            m_has_explicit_board = false;
        }

        if (!succeeded && !result.was_stopped && m_last_error_summary.isEmpty()) {
            int errors = error_diagnostic_count();
            m_last_error_summary = errors > 0
                ? (errors == 1 ? QStringLiteral("1 error") : QStringLiteral("%1 errors").arg(errors))
                : QStringLiteral("Failed");
        }

        int badge_errors = (succeeded || result.was_stopped) ? 0 : std::max(1, error_diagnostic_count());
        m_host.update_dock_state(QStringLiteral("build"), false, badge_errors);

        if (finished_kind == active_run_kind::CLEAN && succeeded) {
            m_status_summary = QStringLiteral("Clean");
            append_build_mark(QStringLiteral("Cleaned build cache"), false);
        }

        if (finished_kind == active_run_kind::INSTALL && succeeded) {
            m_status_summary = QStringLiteral("Installed");
            refresh_libraries_sidebar();
            refresh_board_catalog();
            if (m_rebuild_after_install) {
                m_rebuild_after_install = false;
                run_build();
                return;
            }
        }

        m_rebuild_after_install = false;
        refresh_strip();
    }

    extension_host& m_host;
    ui::ui_section_list* m_libraries = nullptr;
    ui::ui_log_panel* m_build_log = nullptr;
    QPointer<QWidget> m_open_popover;

    process_task m_run_task;
    process_task m_boards_task;
    event_line_stream m_run_events;
    board_catalog m_board_catalog;

    QString m_sketch_dir;
    QString m_board_fqbn;
    QString m_board_name;

    std::vector<installed_library> m_installed_libraries;
    std::vector<installed_library> m_build_libraries;
    std::vector<embedded_diagnostic> m_diagnostics;
    memory_usage m_memory_usage;

    active_run_kind m_active_run = active_run_kind::NONE;
    QString m_progress_label;
    int m_progress_percent = -1;
    QString m_status_summary;
    QString m_last_error_summary;

    bool m_has_explicit_board = false;
    bool m_saw_run_failure = false;
    bool m_rebuild_after_install = false;
    bool m_is_shutting_down = false;
};

} // namespace

static bool qualifies_embedded(const workspace_files& files) {
    return files.contains(QStringLiteral("sketch.yaml"))
        || files.contains(QStringLiteral("sketch.yml"))
        || files.contains(files.folder_name + QStringLiteral(".ino"))
        || files.contains(files.folder_name + QStringLiteral(".pde"))
        || files.has_suffix(QStringLiteral(".ino"))
        || files.has_suffix(QStringLiteral(".pde"));
}

static std::unique_ptr<extension_session> create_embedded_session(extension_host& host) {
    return std::make_unique<embedded_session>(host);
}

extension_descriptor embedded_extension() {
    return {
        "embedded",
        "Embedded Development",
        "Build, flash, and monitor microcontroller firmware with mcufw and serial.",
        extension_icon::CHIP,
        "Arduino sketch detected",
        true,
        EMBEDDED_DEPS,
        {},
        qualifies_embedded,
        create_embedded_session,
    };
}

} // namespace sheen
