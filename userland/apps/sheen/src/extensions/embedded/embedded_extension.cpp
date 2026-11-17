#include "extensions/embedded/embedded_extension.h"

#include "extensions/embedded/embedded_protocol.h"
#include "extensions/embedded/serial_port_watch.h"
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
#include <cstdint>
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

static constexpr uint32_t DEFAULT_BAUD_RATE = 115200;
static constexpr uint32_t BAUD_RATES[] = {
    9600, 19200, 38400, 57600, 74880, 115200, 230400, 460800, 921600,
};

static constexpr int TARGET_POPOVER_WIDTH = 352;
static constexpr int TARGET_POPOVER_HEIGHT = 46;
static constexpr int TARGET_POPOVER_INSET_X = 10;
static constexpr int TARGET_POPOVER_GAP = 8;
static constexpr int MONITOR_LOG_MAX_LINES = 100000;
static constexpr int LOG_FLUSH_INTERVAL_MS = 16;
static constexpr int HEADER_BAR_MARGIN_H = 8;
static constexpr int HEADER_BAR_MARGIN_V = 4;
static constexpr int HEADER_BAR_SPACING = 6;

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

enum class active_run_kind { NONE, BUILD, FLASH, INSTALL, CLEAN };

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

/* Alien container pairing Serial Monitor header controls (baud selector + Clear) with `ui_log_panel`. */
class serial_monitor_panel : public QWidget {
public:
    explicit serial_monitor_panel(uint32_t initial_baud, QWidget* parent = nullptr)
        : QWidget(parent),
          m_baud_button(new ui::ui_pill_button(format_baud_label(initial_baud), this)),
          m_clear_button(new ui::ui_pill_button(QStringLiteral("Clear"), this)),
          m_log(new ui::ui_log_panel(make_log_options(), this)),
          m_baud_rate(initial_baud) {
        m_baud_button->set_has_chevron(true);

        auto* header = new QHBoxLayout();
        header->setContentsMargins(HEADER_BAR_MARGIN_H, HEADER_BAR_MARGIN_V, HEADER_BAR_MARGIN_H, HEADER_BAR_MARGIN_V);
        header->setSpacing(HEADER_BAR_SPACING);
        header->addStretch(1);
        header->addWidget(m_baud_button);
        header->addWidget(m_clear_button);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        root->addLayout(header);
        root->addWidget(m_log, 1);

        QObject::connect(m_clear_button, &QAbstractButton::clicked, m_log, [this] { m_log->clear_log(); });
        QObject::connect(m_baud_button, &QAbstractButton::clicked, this, [this] { open_baud_picker(); });
    }

    ui::ui_log_panel* log_panel() const { return m_log; }

    void set_baud_rate(uint32_t baud_rate) {
        m_baud_rate = baud_rate;
        m_baud_button->setText(format_baud_label(baud_rate));
    }

    void close_popups() {
        if (m_open_picker) {
            m_open_picker->close();
            m_open_picker.clear();
        }
    }

    std::function<void(uint32_t baud_rate)> on_baud_selected;

private:
    static QString format_baud_label(uint32_t baud_rate) {
        return QStringLiteral("%1 baud").arg(baud_rate);
    }

    static ui::log_panel_options make_log_options() {
        ui::log_panel_options options;
        options.max_lines = MONITOR_LOG_MAX_LINES;
        options.flush_interval_ms = LOG_FLUSH_INTERVAL_MS;
        options.has_command_input = true;
        options.input_placeholder = QStringLiteral("Send to serial...");

        options.line_endings = {
            QStringLiteral("LF"),
            QStringLiteral("CRLF"),
            QStringLiteral("CR"),
            QStringLiteral("None"),
        };

        return options;
    }

    void open_baud_picker() {
        close_popups();

        QList<picker_choice> choices;
        for (uint32_t rate : BAUD_RATES) {
            QString rate_text = QString::number(rate);
            QString badge = rate == m_baud_rate ? QStringLiteral("Active") : QString();
            choices.push_back({ rate_text, format_baud_label(rate), {}, badge, false, false });
        }

        auto* picker = new ui::ui_searchable_picker(m_baud_button, QStringLiteral("Filter baud rates..."), choices);
        picker->setAttribute(Qt::WA_DeleteOnClose, true);
        m_open_picker = picker;
        picker->on_selected = [this](const QString& choice_id) {
            bool ok = false;
            uint32_t chosen = choice_id.toUInt(&ok);
            if (ok && chosen > 0 && on_baud_selected) {
                on_baud_selected(chosen);
            }
        };

        picker->open_below_anchor();
    }

    ui::ui_pill_button* m_baud_button = nullptr;
    ui::ui_pill_button* m_clear_button = nullptr;
    ui::ui_log_panel* m_log = nullptr;
    QPointer<QWidget> m_open_picker;
    uint32_t m_baud_rate = DEFAULT_BAUD_RATE;
};

class embedded_session : public extension_session {
public:
    explicit embedded_session(extension_host& host)
        : m_host(host),
          m_libraries(new ui::ui_section_list(QStringLiteral("Install..."))),
          m_build_log(new ui::ui_log_panel({})),
          m_monitor_panel(new serial_monitor_panel(DEFAULT_BAUD_RATE)),
          m_monitor_log(m_monitor_panel->log_panel()),
          m_run_events(event_protocol::TSV_MCUFW) {
        m_sketch_dir = QString::fromStdString(resolve_sketch_directory(m_host.workspace_root().toStdString()));
        m_build_log->set_workspace_root(m_sketch_dir);
        m_monitor_log->set_workspace_root(m_sketch_dir);
        load_saved_sketch_defaults();

        m_ports = std::static_pointer_cast<serial_port_watch>(
            m_host.acquire_service(QStringLiteral("serial_port_watch"),
                                   [] { return std::make_shared<serial_port_watch>(); }));
        m_ports->add_listener(this, [this] {
            if (!m_is_shutting_down) {
                refresh_strip();
            }
        });

        wire_panels_and_tasks();

        m_host.set_sidebar_sections({ { QStringLiteral("libraries"), QStringLiteral("Libraries"), m_libraries } });
        m_host.set_dock_panels({
            { QStringLiteral("build"), QStringLiteral("Build"), m_build_log, false, 0 },
            { QStringLiteral("monitor"), QStringLiteral("Serial Monitor"), m_monitor_panel, false, 0 },
        });

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
        m_flash_monitor.reset();
        m_pending_flash_args.clear();

        if (m_open_popover) {
            m_open_popover->close();
            m_open_popover.clear();
        }

        if (m_monitor_panel != nullptr) {
            m_monitor_panel->close_popups();
        }

        if (m_ports) {
            m_ports->remove_listener(this);
            m_ports.reset();
        }

        m_boards_task.on_stdout = nullptr;
        m_boards_task.on_stderr = nullptr;
        m_boards_task.on_finished = nullptr;

        m_run_task.on_stdout = nullptr;
        m_run_task.on_stderr = nullptr;
        m_run_task.on_finished = nullptr;

        m_monitor_task.on_stdout = nullptr;
        m_monitor_task.on_stderr = nullptr;
        m_monitor_task.on_finished = nullptr;

        m_libraries = nullptr;
        m_build_log = nullptr;
        m_monitor_panel = nullptr;
        m_monitor_log = nullptr;

        m_boards_task.stop();
        m_run_task.stop();
        m_monitor_task.stop();
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

        if (m_monitor_log != nullptr) {
            m_monitor_log->set_workspace_root(m_sketch_dir);
        }

        m_board_fqbn.clear();
        m_board_name.clear();
        m_has_explicit_board = false;
        m_port_name.clear();
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
        return m_run_task.is_running() || m_flash_monitor.is_waiting_for_monitor_stop();
    }

    void load_saved_sketch_defaults() {
        sketch_defaults defaults = read_sketch_defaults(m_sketch_dir.toStdString());
        m_has_explicit_board = false;
        if (!defaults.fqbn.empty()) {
            m_board_fqbn = QString::fromStdString(defaults.fqbn);
        }

        if (!defaults.port_name.empty()) {
            m_port_name = QString::fromStdString(defaults.port_name);
        }
    }

    void wire_panels_and_tasks() {
        m_libraries->set_empty_text(QStringLiteral("No libraries installed"));
        m_libraries->on_action_clicked = [this] { open_library_install_popover(); };
        m_libraries->on_row_activated = [this](const QString& row_id) { activate_library_row(row_id); };
        m_build_log->on_location_clicked = [this](const QString& p, int l) { open_resolved_location(p, l); };
        m_monitor_log->on_location_clicked = [this](const QString& p, int l) { open_resolved_location(p, l); };

        m_monitor_log->on_input_submitted = [this](const QByteArray& bytes) {
            if (!m_is_shutting_down && m_monitor_task.is_running()) {
                m_monitor_task.write_stdin(bytes);
            }
        };

        m_monitor_panel->on_baud_selected = [this](uint32_t baud_rate) { set_baud_rate(baud_rate); };

        wire_boards_task();
        wire_run_task();
        wire_monitor_task();
    }

    void wire_boards_task() {
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
    }

    void wire_run_task() {
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

    void wire_monitor_task() {
        m_monitor_task.on_stdout = [this](QByteArrayView chunk) {
            if (m_is_shutting_down) {
                return;
            }

            if (m_monitor_log != nullptr) {
                m_monitor_log->append_bytes(chunk);
            }

            scan_monitor_crash_output(chunk);
        };

        m_monitor_task.on_stderr = [this](QByteArrayView chunk) {
            if (!m_is_shutting_down) {
                handle_monitor_stderr_chunk(chunk);
            }
        };

        m_monitor_task.on_finished = [this](const process_exit& result) { on_monitor_finished(result); };
    }

    void register_commands() {
        m_host.set_commands({
            { QStringLiteral("embedded.build"), QStringLiteral("Embedded"), QStringLiteral("Build"),
              QStringLiteral("Ctrl+B"), false, [this] { return !is_run_busy(); },
              [this] { run_build_or_flash(false); } },
            { QStringLiteral("embedded.flash"), QStringLiteral("Embedded"), QStringLiteral("Flash"),
              QStringLiteral("F5"), false, [this] { return can_flash(); },
              [this] { run_build_or_flash(true); } },
            { QStringLiteral("embedded.monitor"), QStringLiteral("Embedded"), QStringLiteral("Serial Monitor"),
              QStringLiteral("F6"), false, [this] { return m_monitor_task.is_running() || can_open_monitor(); },
              [this] { toggle_monitor(); } },
            { QStringLiteral("embedded.stop"), QStringLiteral("Embedded"), QStringLiteral("Stop"),
              QStringLiteral("Shift+F5"), true, [this] { return is_run_busy(); },
              [this] { cancel_active_run(); } },
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

    resolved_serial_port current_port_resolution() const {
        return m_ports ? m_ports->resolve_port(m_port_name.toStdString(), m_baud_rate) : resolved_serial_port{};
    }

    bool can_flash() const {
        if (is_run_busy()) {
            return false;
        }

        resolved_serial_port port = current_port_resolution();
        return m_port_name.isEmpty() || port.is_present;
    }

    bool can_open_monitor() const {
        if (is_run_busy() && (m_active_run == active_run_kind::FLASH || m_flash_monitor.is_waiting_for_monitor_stop())) {
            return false;
        }

        resolved_serial_port port = current_port_resolution();
        return !port.name.empty() && port.is_present;
    }

    void set_baud_rate(uint32_t baud_rate) {
        if (baud_rate == 0 || baud_rate == m_baud_rate || m_is_shutting_down) {
            return;
        }

        m_baud_rate = baud_rate;
        if (m_monitor_panel != nullptr) {
            m_monitor_panel->set_baud_rate(m_baud_rate);
        }

        if (m_monitor_task.is_running() && !m_flash_monitor.is_waiting_for_monitor_stop()) {
            m_flash_monitor.request_monitor_restart_after_stop();
            m_monitor_task.stop();
        }

        refresh_strip();
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

    QList<picker_choice> build_port_choices() const {
        QList<picker_choice> choices;
        resolved_serial_port resolved = current_port_resolution();
        if (!m_port_name.isEmpty() && !resolved.is_present) {
            choices.push_back({ m_port_name, m_port_name, QStringLiteral("/dev/serial/") + m_port_name,
                                QStringLiteral("Unplugged"), false, true });
        }

        if (m_ports) {
            for (const serial_port_entry& port : m_ports->ports()) {
                QString name = QString::fromStdString(port.name);
                QString detail = QString::fromStdString(format_serial_port_detail(port, m_baud_rate));
                QString badge = name == QString::fromStdString(resolved.name) ? QStringLiteral("Active") : QString();
                choices.push_back({ name, name, detail, badge, false, false });
            }
        }

        return choices;
    }

    void select_port_choice(const QString& choice_id) {
        m_port_name = QString::fromStdString(normalize_serial_port_name(choice_id.toStdString()));
        if (m_monitor_task.is_running() && !m_flash_monitor.is_waiting_for_monitor_stop()) {
            m_flash_monitor.request_monitor_restart_after_stop();
            m_monitor_task.stop();
        }

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
        bool flash_active = busy && (m_active_run == active_run_kind::FLASH
                                     || m_flash_monitor.is_waiting_for_monitor_stop());
        QString board_detail = !m_board_name.isEmpty() ? m_board_fqbn : QString();

        resolved_serial_port port = current_port_resolution();
        QString port_label = !port.name.empty()
            ? QString::fromStdString(port.name)
            : (m_ports && m_ports->ports().size() > 1 ? QStringLiteral("Select Port") : QStringLiteral("No Port"));
        QString port_detail;
        if (!port.name.empty()) {
            port_detail = port.is_present
                ? QString::fromStdString(port.short_description)
                : QStringLiteral("Disconnected");
        }

        QList<button_spec> buttons = {
            { QStringLiteral("build"), QStringLiteral("Build"), QStringLiteral("Compile sketch (Ctrl+B)"),
              !busy, busy && m_active_run == active_run_kind::BUILD, [this] { run_build_or_flash(false); } },
            { QStringLiteral("flash"), QStringLiteral("Flash"), QStringLiteral("Build and upload to board (F5)"),
              can_flash(), flash_active, [this] { run_build_or_flash(true); } },
            { QStringLiteral("monitor"), QStringLiteral("Monitor"), QStringLiteral("Toggle Serial Monitor (F6)"),
              m_monitor_task.is_running() || can_open_monitor(), m_monitor_task.is_running(),
              [this] { toggle_monitor(); } },
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
            { QStringLiteral("port"), port_label, port_detail,
              QStringLiteral("Filter serial ports..."), !busy,
              [this] { return build_port_choices(); },
              [this](const QString& id) { select_port_choice(id); } },
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

    void run_build_or_flash(bool is_flash) {
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
        m_progress_label = is_flash ? QStringLiteral("Preparing flash...") : QStringLiteral("Building...");
        m_progress_percent = 0;

        if (m_build_log != nullptr) {
            m_build_log->clear_log();
        }

        refresh_libraries_sidebar();

        QStringList args = {
            is_flash ? QStringLiteral("flash") : QStringLiteral("build"),
            m_sketch_dir,
            QStringLiteral("--events"),
        };

        if (m_has_explicit_board && !m_board_fqbn.isEmpty()) {
            args << QStringLiteral("--board") << m_board_fqbn;
        }

        if (is_flash && !m_port_name.isEmpty()) {
            args << QStringLiteral("--port") << m_port_name;
        }

        if (is_flash && m_flash_monitor.begin_flash(m_monitor_task.is_running())) {
            m_pending_flash_args = std::move(args);
            append_monitor_mark(QStringLiteral("Pausing Serial Monitor for flash..."), false);
            m_monitor_task.stop();
            refresh_strip();
            return;
        }

        start_mcufw_run(is_flash ? active_run_kind::FLASH : active_run_kind::BUILD, args);
    }

    void start_mcufw_run(active_run_kind kind, const QStringList& args) {
        m_active_run = kind;
        m_run_events = event_line_stream(event_protocol::TSV_MCUFW);
        m_saw_run_failure = false;
        m_saw_flashed_event = false;

        process_spec spec = { QString::fromLatin1(MCUFW_PROGRAM), args, m_sketch_dir, process_io_mode::PIPES, false };
        QString start_error;
        if (!m_run_task.start(spec, &start_error)) {
            m_active_run = active_run_kind::NONE;
            m_last_error_summary = QStringLiteral("Launch failed");
            append_build_mark(start_error, true);

            m_host.reveal_dock_panel(QStringLiteral("build"));
            m_host.update_dock_state(QStringLiteral("build"), false, 1);
            if (kind == active_run_kind::FLASH) {
                apply_flash_finish_action(m_flash_monitor.on_flash_finished(false));
            }

            refresh_strip();
            return;
        }

        m_host.reveal_dock_panel(QStringLiteral("build"));
        m_host.update_dock_state(QStringLiteral("build"), true, 0);
        refresh_strip();
    }

    void cancel_active_run() {
        m_rebuild_after_install = false;
        m_pending_flash_args.clear();

        bool was_waiting_for_monitor = m_flash_monitor.is_waiting_for_monitor_stop();
        flash_cancel_action action = m_flash_monitor.cancel_flash(m_monitor_task.is_running());
        if (was_waiting_for_monitor) {
            m_progress_percent = -1;
            m_progress_label.clear();
            m_status_summary = QStringLiteral("Cancelled");
        }

        if (m_run_task.is_running()) {
            m_run_task.stop();
        }

        if (action == flash_cancel_action::START_MONITOR_NOW && can_open_monitor()) {
            start_monitor(false);
        }

        refresh_strip();
    }

    void apply_flash_finish_action(flash_finish_action action) {
        if (action == flash_finish_action::NONE || !can_open_monitor()) {
            return;
        }

        start_monitor(action == flash_finish_action::RESUME_AND_REVEAL_MONITOR);
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

    void append_monitor_mark(const QString& caption, bool is_danger) {
        if (!m_is_shutting_down && m_monitor_log != nullptr) {
            m_monitor_log->append_mark(caption, is_danger);
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

        case mcufw_event_kind::PORT:
            append_build_mark(QStringLiteral("Uploading on %1").arg(QString::fromStdString(event.port_path)), false);
            return;

        case mcufw_event_kind::FLASHED:
            m_saw_flashed_event = true;
            if (!event.port_name.empty() && !m_port_name.isEmpty()) {
                m_port_name = QString::fromStdString(event.port_name);
            }

            m_status_summary = QStringLiteral("Flashed");
            append_build_mark(QStringLiteral("Flashed %1").arg(QString::fromStdString(event.port_path)), false);
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
        if ((finished_kind == active_run_kind::BUILD || finished_kind == active_run_kind::FLASH) && succeeded) {
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
                run_build_or_flash(false);
                return;
            }
        }

        m_rebuild_after_install = false;
        if (finished_kind == active_run_kind::FLASH) {
            apply_flash_finish_action(m_flash_monitor.on_flash_finished(succeeded && m_saw_flashed_event));
        }

        refresh_strip();
    }

    void toggle_monitor() {
        if (m_monitor_task.is_running()) {
            m_flash_monitor.clear_for_manual_monitor_toggle();
            m_pending_flash_args.clear();
            m_monitor_task.stop();
            refresh_strip();
            return;
        }

        m_flash_monitor.clear_for_manual_monitor_toggle();
        m_pending_flash_args.clear();
        start_monitor(true);
    }

    void start_monitor(bool reveal_panel) {
        if (m_monitor_task.is_running() || m_is_shutting_down) {
            return;
        }

        resolved_serial_port resolved = current_port_resolution();
        if (resolved.name.empty() || !resolved.is_present) {
            m_host.show_status_note(QStringLiteral("Connect or select a serial port before starting Serial Monitor"));
            return;
        }

        m_crash_scanner.reset();
        m_monitor_stderr_buffer.clear();

        QStringList args = { QStringLiteral("monitor") };
        if (!m_port_name.isEmpty()) {
            args << QStringLiteral("--port") << m_port_name;
        }

        args << QStringLiteral("--baud") << QString::number(m_baud_rate);
        args << QStringLiteral("--") << QStringLiteral("--events");

        process_spec spec = { QString::fromLatin1(MCUFW_PROGRAM), args, m_sketch_dir, process_io_mode::PIPES, true };
        QString start_error;
        if (!m_monitor_task.start(spec, &start_error)) {
            append_monitor_mark(start_error, true);
            if (reveal_panel) {
                m_host.reveal_dock_panel(QStringLiteral("monitor"));
            }

            m_host.update_dock_state(QStringLiteral("monitor"), false, 1);
            refresh_strip();
            return;
        }

        if (reveal_panel) {
            m_host.reveal_dock_panel(QStringLiteral("monitor"));
        }

        m_host.update_dock_state(QStringLiteral("monitor"), true, 0);
        refresh_strip();
    }

    void scan_monitor_crash_output(QByteArrayView chunk) {
        std::string_view raw(chunk.data(), static_cast<size_t>(chunk.size()));
        std::optional<backtrace_location> first = m_crash_scanner.feed_chunk(raw, [](const std::string& path) {
            return QFileInfo::exists(QString::fromStdString(path));
        });

        if (first.has_value()) {
            QString short_name = QFileInfo(QString::fromStdString(first->file_path)).fileName();
            m_host.show_status_note(QStringLiteral("Decoded crash at %1:%2").arg(short_name).arg(first->line));
        }
    }

    void handle_monitor_stderr_chunk(QByteArrayView chunk) {
        m_monitor_stderr_buffer.append(chunk.data(), static_cast<size_t>(chunk.size()));
        size_t start = 0;
        while (true) {
            size_t newline = m_monitor_stderr_buffer.find('\n', start);
            if (newline == std::string::npos) {
                break;
            }

            handle_monitor_event_line(std::string_view(m_monitor_stderr_buffer.data() + start, newline - start));
            start = newline + 1;
        }

        if (start > 0) {
            m_monitor_stderr_buffer.erase(0, start);
        }
    }

    void handle_monitor_event_line(std::string_view line) {
        serial_event event = parse_serial_event_line(line);
        bool is_danger = false;
        std::string mark = format_serial_monitor_mark(event, is_danger);
        if (!mark.empty()) {
            append_monitor_mark(QString::fromStdString(mark), is_danger);
        }

        if (event.kind == serial_event_kind::OPEN || event.kind == serial_event_kind::BACK) {
            m_host.update_dock_state(QStringLiteral("monitor"), true, 0);
        } else if (event.kind == serial_event_kind::GONE || event.kind == serial_event_kind::CLOSED) {
            m_host.update_dock_state(QStringLiteral("monitor"), false, 0);
        } else if (event.kind == serial_event_kind::FAILED) {
            m_host.update_dock_state(QStringLiteral("monitor"), false, 1);
        }
    }

    void on_monitor_finished(const process_exit& result) {
        if (m_is_shutting_down) {
            return;
        }

        if (!m_monitor_stderr_buffer.empty()) {
            std::string tail = std::move(m_monitor_stderr_buffer);
            m_monitor_stderr_buffer.clear();
            handle_monitor_event_line(tail);
        }

        m_host.update_dock_state(
            QStringLiteral("monitor"),
            false,
            result.exit_code != 0 && !result.was_stopped ? 1 : 0);

        monitor_stop_action next_action = m_flash_monitor.on_monitor_stopped();
        if (next_action == monitor_stop_action::START_PENDING_FLASH) {
            QStringList flash_args = std::move(m_pending_flash_args);
            m_pending_flash_args.clear();
            start_mcufw_run(active_run_kind::FLASH, flash_args);
            return;
        }

        if (next_action == monitor_stop_action::RESTART_MONITOR) {
            if (can_open_monitor()) {
                start_monitor(false);
                return;
            }
        }

        refresh_strip();
    }

    extension_host& m_host;
    std::shared_ptr<serial_port_watch> m_ports;
    ui::ui_section_list* m_libraries = nullptr;
    ui::ui_log_panel* m_build_log = nullptr;
    serial_monitor_panel* m_monitor_panel = nullptr;
    ui::ui_log_panel* m_monitor_log = nullptr;
    QPointer<QWidget> m_open_popover;

    process_task m_run_task;
    process_task m_monitor_task;
    process_task m_boards_task;
    event_line_stream m_run_events;
    board_catalog m_board_catalog;
    flash_monitor_coordinator m_flash_monitor;
    crash_output_scanner m_crash_scanner;

    QString m_sketch_dir;
    QString m_board_fqbn;
    QString m_board_name;
    QString m_port_name;
    uint32_t m_baud_rate = DEFAULT_BAUD_RATE;

    std::vector<installed_library> m_installed_libraries;
    std::vector<installed_library> m_build_libraries;
    std::vector<embedded_diagnostic> m_diagnostics;
    memory_usage m_memory_usage;

    std::string m_monitor_stderr_buffer;
    active_run_kind m_active_run = active_run_kind::NONE;
    QString m_progress_label;
    int m_progress_percent = -1;
    QString m_status_summary;
    QString m_last_error_summary;
    QStringList m_pending_flash_args;

    bool m_has_explicit_board = false;
    bool m_saw_run_failure = false;
    bool m_saw_flashed_event = false;
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
