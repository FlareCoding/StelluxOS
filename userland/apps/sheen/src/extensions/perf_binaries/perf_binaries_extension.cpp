#include "extensions/perf_binaries/perf_binaries_extension.h"
#include "core/editor_core.h"
#include "extensions/extension_host.h"
#include "extensions/perf_binaries/disasm_mapper.h"
#include "extensions/perf_binaries/disasm_split_view.h"
#include "extensions/perf_binaries/elf_hex_view.h"
#include "extensions/perf_binaries/elf_model.h"
#include "extensions/perf_binaries/ktrace_model.h"
#include "ui/chrome.h"
#include "ui/file_sheet.h"
#include "ui/ui_kit.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace sheen {

static constexpr const char* EXTENSION_ID = "perf_binaries";
static constexpr const char* KTRACE_CONTROL_PATH = "/dev/ktrace/control";
static constexpr qint64 ELF_MIN_FILE_BYTES = 64;
static constexpr qint64 TRACE_MIN_FILE_BYTES = 16;
static constexpr qint64 PROBE_HEADER_BYTES = 24;
static constexpr size_t MAX_SIDEBAR_ITEMS = 24;
static constexpr qsizetype SYMBOL_TAB_TITLE_MAX_CHARS = 28;
static constexpr int TRACE_BAR_HEIGHT = 32;
static constexpr int TRACE_BAR_PAD_X = 12;
static constexpr double HOT_LINE_PERCENT_THRESHOLD = 10.0;

static uint64_t g_capture_sequence = 0;

static constexpr package_requirement PERF_BINARIES_PACKAGES[] = {
    { "binutils", "/usr/bin/objdump" },
};

static constexpr dependency_group PERF_BINARIES_DEPS[] = {
    {
        "binary-tools",
        "GNU Binary Utilities",
        "Static binary disassembler and symbol inspection tools.",
        PERF_BINARIES_PACKAGES,
        {},
        nullptr,
        event_protocol::NONE,
        true,
    },
};

namespace {

class trace_summary_tab final : public QWidget {
public:
    explicit trace_summary_tab(const QString& trace_path, QWidget* parent = nullptr);
    ~trace_summary_tab() override;

    const QString& trace_path() const { return m_trace_path; }
    void stop_decode();

    std::function<void(const ktrace_profile_model& model, const ktrace_window_view& window)> on_loaded;
    std::function<void(const ktrace_hotspot& hotspot)> on_hotspot_activated;

private:
    void start_load();
    void finish_decode(const process_exit& exit_result);
    void cleanup_temp_db();
    void apply_loaded_model(ktrace_profile_model loaded);

    QString m_trace_path;
    QString m_temp_db_path;
    QLabel* m_header_label = nullptr;
    ui::ui_table_view* m_hotspots_table = nullptr;
    std::unique_ptr<process_task> m_decode_task;
    QByteArray m_decode_stderr;
    std::vector<ktrace_hotspot> m_hotspots;
};

class perf_binaries_session final : public extension_session {
public:
    explicit perf_binaries_session(extension_host& host);
    ~perf_binaries_session() override;

    void shutdown() override;
    void on_active_file_changed(const QString& file_path) override;

    QString workspace_root() const { return m_host.workspace_root(); }
    void adopt_elf_view(const QString& binary_path, elf_hex_view* view);
    void adopt_trace_tab(const QString& trace_path, trace_summary_tab* view);

private:
    void discover_initial_targets();
    void register_commands();
    QList<picker_choice> list_target_choices() const;
    void toggle_recording();
    void stop_detached_ktrace_recorder();
    QString choose_file_to_open(const QString& title, const QString& start_dir, const QString& initial_path);
    void browse_for_trace();
    void browse_for_binary();
    void open_trace_tab(const QString& path);
    void update_from_trace_window(const ktrace_profile_model& model, const ktrace_window_view& window,
                                  bool scoped_to_single_thread);
    QString resolve_existing_workspace_file(const std::string& recorded_path) const;
    void handle_sidebar_or_hotspot_row(const QString& row_id);
    void jump_to_hotspot_location(const std::string& function_name, const std::string& source_file,
                                  int source_line, bool prefer_disasm);
    void load_target_binary(const QString& path, bool open_tab);
    void populate_symbol_views();
    void open_elf_inspector_tab();
    void preview_symbol_by_index(const QString& row_id);
    void activate_symbol_by_index(const QString& row_id);
    void open_default_disassembly();
    void open_disassembly_for_symbol(const elf_symbol& symbol);
    void apply_badges_to_open_disasm_tabs();
    void refresh_strip();

    extension_host& m_host;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    bool m_is_shutting_down = false;

    ui::ui_section_list* m_hotspots_sidebar = nullptr;
    ui::ui_table_view* m_hotspots_dock = nullptr;
    ui::ui_table_view* m_syscalls_dock = nullptr;
    ui::ui_table_view* m_symbols_dock = nullptr;

    QPointer<elf_hex_view> m_elf_tab;
    QPointer<trace_summary_tab> m_trace_tab;
    std::vector<QPointer<disasm_split_view>> m_disasm_tabs;

    process_task m_ktrace_task;
    QByteArray m_ktrace_stderr;
    QString m_recording_output_path;
    bool m_is_recording = false;

    QString m_trace_path;
    QString m_trace_window_readout;
    std::vector<ktrace_hotspot> m_active_hotspots;
    QList<line_badge_item> m_active_badges;
    bool m_has_active_trace = false;

    QString m_binary_path;
    elf_binary_model m_elf_model;
    bool m_has_elf_model = false;
};

} // namespace

static std::vector<perf_binaries_session*> g_active_sessions;

static QString qstring_duration(uint64_t ns) {
    return QString::fromStdString(format_duration_ns(ns));
}

static std::string_view strip_parameter_list(std::string_view demangled) {
    size_t paren = demangled.find('(');
    return paren == std::string_view::npos ? demangled : demangled.substr(0, paren);
}

static perf_binaries_session* find_session_for_path(const QString& path) {
    QString clean_path = QFileInfo(path).absoluteFilePath();
    for (perf_binaries_session* session : g_active_sessions) {
        QString root = QDir::cleanPath(session->workspace_root());
        if (!root.isEmpty() && (clean_path == root || clean_path.startsWith(root + QLatin1Char('/')))) {
            return session;
        }
    }

    return nullptr;
}

static uint16_t probe_elf64_type(const QString& path) {
    QFileInfo file_info(path);
    if (!file_info.isFile() || file_info.size() < ELF_MIN_FILE_BYTES || file_info.size() > MAX_ELF_FILE_BYTES) {
        return 0;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }

    QByteArray head = file.read(PROBE_HEADER_BYTES);
    std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(head.constData()),
                                   static_cast<size_t>(head.size()));
    if (!is_elf64_binary(bytes) || bytes.size() < 18) {
        return 0;
    }

    return static_cast<uint16_t>(bytes[16] | (static_cast<uint16_t>(bytes[17]) << 8));
}

static bool probe_file_is_trace(const QString& path) {
    if (path.endsWith(QLatin1String(".ktrace"), Qt::CaseInsensitive)) {
        return true;
    }

    QFileInfo file_info(path);
    if (!file_info.isFile() || file_info.size() < TRACE_MIN_FILE_BYTES) {
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QByteArray head = file.read(PROBE_HEADER_BYTES);
    std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(head.constData()),
                                   static_cast<size_t>(head.size()));
    if (is_ktrace_capture(bytes)) {
        return true;
    }

    return is_sqlite_database(bytes) && path.endsWith(QLatin1String(".db"), Qt::CaseInsensitive);
}

static QStringList discover_workspace_binaries(const QString& root_path) {
    if (root_path.isEmpty()) {
        return {};
    }

    QStringList executables;
    QStringList relocatables;
    const QStringList search_dirs = { root_path, QDir(root_path).filePath(QStringLiteral("build")) };
    for (const QString& dir_path : search_dirs) {
        QDir dir(dir_path);
        if (!dir.exists()) {
            continue;
        }

        const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& entry : entries) {
            uint16_t elf_type = probe_elf64_type(entry.absoluteFilePath());
            if (elf_type == ELF_TYPE_EXEC || elf_type == ELF_TYPE_DYN) {
                executables.append(entry.absoluteFilePath());
            } else if (elf_type == ELF_TYPE_REL) {
                relocatables.append(entry.absoluteFilePath());
            }
        }
    }

    executables.append(relocatables);
    executables.removeDuplicates();
    return executables;
}

static QStringList discover_workspace_traces(const QString& root_path) {
    QStringList found;
    const QStringList search_dirs = root_path.isEmpty()
                                        ? QStringList{ QStringLiteral("/tmp") }
                                        : QStringList{ root_path, QStringLiteral("/tmp") };
    for (const QString& dir_path : search_dirs) {
        QDir dir(dir_path);
        if (!dir.exists()) {
            continue;
        }

        for (const QFileInfo& entry : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Time)) {
            if (probe_file_is_trace(entry.absoluteFilePath())) {
                found.append(entry.absoluteFilePath());
            }
        }
    }

    found.removeDuplicates();
    return found;
}

static QList<ui::table_row> build_syscall_table_rows(const std::vector<ktrace_syscall_summary>& summaries) {
    QList<ui::table_row> rows;
    for (const ktrace_syscall_summary& summary : summaries) {
        rows.append({
            QString::number(summary.number),
            { QString::fromStdString(summary.name), qstring_duration(summary.wall_ns),
              qstring_duration(summary.sleep_ns), QString::number(summary.call_count),
              QString::number(summary.error_count), qstring_duration(summary.max_duration_ns) },
        });
    }

    return rows;
}

trace_summary_tab::trace_summary_tab(const QString& trace_path, QWidget* parent)
    : QWidget(parent), m_trace_path(trace_path) {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* header = new perf_strip_bar(TRACE_BAR_HEIGHT, false, true, this);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(TRACE_BAR_PAD_X, 0, TRACE_BAR_PAD_X, 1);

    m_header_label = new QLabel(header);
    m_header_label->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
    m_header_label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                      .arg(token(STLXTHEME_TEXT_SECONDARY).name(QColor::HexArgb)));
    header_layout->addWidget(m_header_label, 1);
    root_layout->addWidget(header);

    m_hotspots_table = new ui::ui_table_view({
        { QStringLiteral("Thread / Kernel Activity"), 0, false }, { QStringLiteral("Self"), 96, true },
        { QStringLiteral("Total"), 96, true }, { QStringLiteral("Self %"), 80, true },
        { QStringLiteral("Calls"), 76, true },
    }, this);

    auto activate_fn = [this](const QString& row_id) {
        bool ok = false;
        size_t idx = row_id.toULongLong(&ok);
        if (ok && idx < m_hotspots.size() && on_hotspot_activated) {
            on_hotspot_activated(m_hotspots[idx]);
        }
    };

    m_hotspots_table->on_row_activated = activate_fn;
    m_hotspots_table->on_row_clicked = activate_fn;
    root_layout->addWidget(m_hotspots_table, 1);
    start_load();
}

trace_summary_tab::~trace_summary_tab() {
    stop_decode();
}

void trace_summary_tab::stop_decode() {
    if (m_decode_task) {
        m_decode_task->on_stdout = nullptr;
        m_decode_task->on_stderr = nullptr;
        m_decode_task->on_finished = nullptr;
        m_decode_task->stop();
        m_decode_task.reset();
    }

    cleanup_temp_db();
}

void trace_summary_tab::cleanup_temp_db() {
    if (!m_temp_db_path.isEmpty()) {
        QByteArray utf8 = m_temp_db_path.toUtf8();
        ::unlink(utf8.constData());
        QByteArray partial = utf8 + ".partial";
        ::unlink(partial.constData());
        m_temp_db_path.clear();
    }
}

void trace_summary_tab::start_load() {
    stop_decode();
    QString title = QFileInfo(m_trace_path).fileName();

    QFile file(m_trace_path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_header_label->setText(QStringLiteral("Cannot open %1").arg(title));
        return;
    }

    QByteArray prefix = file.read(TRACE_MIN_FILE_BYTES);
    file.close();
    std::span<const uint8_t> head_span(reinterpret_cast<const uint8_t*>(prefix.constData()),
                                       static_cast<size_t>(prefix.size()));
    if (!is_ktrace_capture(head_span)) {
        ktrace_profile_model loaded;
        std::string error_message;
        if (!load_ktrace_database(m_trace_path.toStdString(), loaded, error_message)) {
            m_header_label->setText(QString::fromStdString(error_message));
            return;
        }

        apply_loaded_model(std::move(loaded));
        return;
    }

    g_capture_sequence++;
    m_temp_db_path = QStringLiteral("/tmp/sheen-ktrace-%1-%2.db").arg(::getpid()).arg(g_capture_sequence);
    cleanup_temp_db();
    m_temp_db_path = QStringLiteral("/tmp/sheen-ktrace-%1-%2.db").arg(::getpid()).arg(g_capture_sequence);
    m_decode_stderr.clear();
    m_header_label->setText(QStringLiteral("Decoding %1...").arg(title));

    m_decode_task = std::make_unique<process_task>();
    m_decode_task->on_stderr = [this](QByteArrayView chunk) { m_decode_stderr.append(chunk.data(), chunk.size()); };
    m_decode_task->on_finished = [this](const process_exit& exit_result) { finish_decode(exit_result); };

    QString start_error;
    process_spec spec;
    spec.program = QStringLiteral("/bin/ktrace-decode");
    spec.arguments = { QFileInfo(m_trace_path).absoluteFilePath(), QStringLiteral("-o"), m_temp_db_path };
    if (!m_decode_task->start(spec, &start_error)) {
        m_header_label->setText(QStringLiteral("ktrace-decode unavailable: %1").arg(start_error));
        stop_decode();
    }
}

void trace_summary_tab::finish_decode(const process_exit& exit_result) {
    if (exit_result.exit_code != 0) {
        QString detail = QString::fromUtf8(m_decode_stderr).trimmed();
        m_header_label->setText(detail.isEmpty() ? QStringLiteral("ktrace-decode failed") : detail);
        cleanup_temp_db();
        return;
    }

    ktrace_profile_model loaded;
    std::string error_message;
    bool ok = load_ktrace_database(m_temp_db_path.toStdString(), loaded, error_message);
    cleanup_temp_db();
    if (!ok) {
        m_header_label->setText(QString::fromStdString(error_message));
        return;
    }

    apply_loaded_model(std::move(loaded));
}

void trace_summary_tab::apply_loaded_model(ktrace_profile_model loaded) {
    uint64_t end_ns = std::max(loaded.session.start_ns + 1, loaded.session.effective_end_ns);
    ktrace_window_view window = query_ktrace_window(
        loaded, { loaded.session.start_ns, end_ns, std::nullopt, std::nullopt, true });
    m_hotspots = window.function_hotspots;

    QList<ui::table_row> hotspot_rows;
    for (size_t idx = 0; idx < window.function_hotspots.size(); idx++) {
        const ktrace_hotspot& hotspot = window.function_hotspots[idx];
        hotspot_rows.append({
            QString::number(idx),
            { QString::fromStdString(hotspot.name), qstring_duration(hotspot.self_ns),
              qstring_duration(hotspot.total_ns),
              QStringLiteral("%1%").arg(hotspot.self_percent, 0, 'f', 1),
              QString::number(hotspot.call_count) },
        });
    }

    m_hotspots_table->set_rows(hotspot_rows);

    QString health_tag = QStringLiteral("Health OK");
    if (!loaded.all_health_checks_passed) {
        if (loaded.session.lost_records > 0) {
            health_tag = QStringLiteral("Lost %1").arg(loaded.session.lost_records);
        } else {
            health_tag = QStringLiteral("Incomplete");
            for (const ktrace_health_status& check : loaded.health_checks) {
                if (!check.passed && !check.name.empty()) {
                    health_tag = QString::fromStdString(check.name) + QStringLiteral(" failed");
                    break;
                }
            }
        }
    }

    m_header_label->setText(QStringLiteral("%1   %2   %3 CPUs   %4 records   %5   Duration %6")
                                .arg(QFileInfo(m_trace_path).fileName(), QString::fromStdString(loaded.session.arch))
                                .arg(loaded.session.cpu_count)
                                .arg(loaded.session.records)
                                .arg(health_tag, qstring_duration(window.window_ns)));

    if (on_loaded) {
        on_loaded(loaded, window);
    }
}

static bool can_open_elf_file(const QString& path, QByteArrayView header_bytes) {
    (void)path;
    std::span<const uint8_t> span(reinterpret_cast<const uint8_t*>(header_bytes.data()),
                                  static_cast<size_t>(header_bytes.size()));
    return is_elf64_binary(span);
}

static QWidget* create_elf_file_view(editor_core& editor, const QString& path) {
    auto* view = new elf_hex_view(path);
    if (perf_binaries_session* session = find_session_for_path(path)) {
        session->adopt_elf_view(path, view);
        return view;
    }

    QPointer<editor_core> guarded_editor(&editor);
    QPointer<elf_hex_view> guarded_view(view);
    QString project_root = infer_project_root_for_binary(path);

    view->on_symbol_activated = [guarded_editor, guarded_view, path, project_root](const elf_symbol& symbol) {
        if (!guarded_editor || !guarded_view) {
            return;
        }

        QString owner_id = QString::fromLatin1(EXTENSION_ID);
        QString tab_id = QStringLiteral("disasm:%1:%2").arg(path, QString::fromStdString(symbol.raw_name));
        if (guarded_editor->has_custom_tab(owner_id, tab_id)) {
            guarded_editor->focus_custom_tab(owner_id, tab_id);
            return;
        }

        auto* split = new disasm_split_view(path, project_root,
                                            guarded_view->model().header.machine,
                                            guarded_view->model().header.dwarf_version, symbol);
        QString title = QStringLiteral("%1 - Disasm")
                            .arg(QString::fromStdString(symbol.demangled_name).left(SYMBOL_TAB_TITLE_MAX_CHARS));
        guarded_editor->open_custom_tab(owner_id, { tab_id, title, split });
    };

    return view;
}

static bool can_open_trace_file(const QString& path, QByteArrayView header_bytes) {
    std::span<const uint8_t> span(reinterpret_cast<const uint8_t*>(header_bytes.data()),
                                  static_cast<size_t>(header_bytes.size()));
    if (is_ktrace_capture(span) || path.endsWith(QLatin1String(".ktrace"), Qt::CaseInsensitive)) {
        return true;
    }

    return is_sqlite_database(span) &&
           (path.endsWith(QLatin1String(".db"), Qt::CaseInsensitive) ||
            path.endsWith(QLatin1String(".sqlite"), Qt::CaseInsensitive));
}

static QWidget* create_trace_file_view(editor_core& editor, const QString& path) {
    auto* view = new trace_summary_tab(path);
    if (perf_binaries_session* session = find_session_for_path(path)) {
        session->adopt_trace_tab(path, view);
        return view;
    }

    QPointer<editor_core> guarded_editor(&editor);
    view->on_hotspot_activated = [guarded_editor](const ktrace_hotspot& hotspot) {
        if (!guarded_editor || hotspot.source_file.empty() || hotspot.source_line <= 0) {
            return;
        }

        QString candidate = QString::fromStdString(hotspot.source_file);
        if (QFileInfo::exists(candidate)) {
            guarded_editor->open_file_at_line(QFileInfo(candidate).absoluteFilePath(), hotspot.source_line);
        }
    };

    return view;
}

static constexpr file_opener_spec PERF_BINARIES_OPENERS[] = {
    { "elf", "Open ELF & Hex", can_open_elf_file, create_elf_file_view },
    { "ktrace", "Open Trace", can_open_trace_file, create_trace_file_view },
};

perf_binaries_session::perf_binaries_session(extension_host& host)
    : m_host(host),
      m_hotspots_sidebar(new ui::ui_section_list(QStringLiteral("Open..."))),
      m_hotspots_dock(new ui::ui_table_view({
          { QStringLiteral("Function"), 0, false }, { QStringLiteral("Self"), 92, true }, { QStringLiteral("Total"), 92, true },
          { QStringLiteral("Self %"), 78, true }, { QStringLiteral("Calls"), 72, true }, { QStringLiteral("Location"), 160, false },
      })),
      m_syscalls_dock(new ui::ui_table_view({
          { QStringLiteral("Syscall"), 0, false }, { QStringLiteral("Wall"), 96, true }, { QStringLiteral("Sleep"), 96, true },
          { QStringLiteral("Calls"), 78, true }, { QStringLiteral("Errors"), 72, true }, { QStringLiteral("Max"), 96, true },
      })),
      m_symbols_dock(new ui::ui_table_view({
          { QStringLiteral("Symbol"), 0, false }, { QStringLiteral("Section"), 96, false }, { QStringLiteral("VMA"), 112, false },
          { QStringLiteral("Size"), 84, true }, { QStringLiteral("Bind"), 76, false }, { QStringLiteral("Type"), 76, false },
      })) {
    g_active_sessions.push_back(this);

    m_hotspots_sidebar->set_empty_text(QStringLiteral("Open a trace or ELF64 binary"));
    m_hotspots_sidebar->on_action_clicked = [this] { browse_for_trace(); };
    m_hotspots_sidebar->on_row_activated = [this](const QString& row_id) { handle_sidebar_or_hotspot_row(row_id); };

    m_hotspots_dock->on_row_activated = [this](const QString& row_id) { handle_sidebar_or_hotspot_row(row_id); };
    m_hotspots_dock->on_row_clicked = [this](const QString& row_id) { handle_sidebar_or_hotspot_row(row_id); };
    m_symbols_dock->on_row_clicked = [this](const QString& row_id) { preview_symbol_by_index(row_id); };
    m_symbols_dock->on_row_activated = [this](const QString& row_id) { activate_symbol_by_index(row_id); };

    m_host.set_sidebar_sections({ { QStringLiteral("hotspots"), QStringLiteral("Hotspots"), m_hotspots_sidebar } });
    m_host.set_dock_panels({
        { QStringLiteral("hotspots"), QStringLiteral("Hotspots"), m_hotspots_dock, false, 0 },
        { QStringLiteral("syscalls"), QStringLiteral("Syscalls & Waits"), m_syscalls_dock, false, 0 },
        { QStringLiteral("symbols"), QStringLiteral("Symbols"), m_symbols_dock, false, 0 },
    });

    register_commands();
    discover_initial_targets();
}

perf_binaries_session::~perf_binaries_session() {
    shutdown();
}

void perf_binaries_session::stop_detached_ktrace_recorder() {
    if (!m_is_recording) {
        return;
    }

    int control_fd = ::open(KTRACE_CONTROL_PATH, O_WRONLY);
    if (control_fd >= 0) {
        ssize_t written = ::write(control_fd, "stop", 4);
        (void)written;
        ::close(control_fd);
    }

    m_recording_output_path.clear();
    m_is_recording = false;
}

void perf_binaries_session::shutdown() {
    if (m_is_shutting_down) {
        return;
    }

    m_is_shutting_down = true;
    *m_alive = false;
    g_active_sessions.erase(std::remove(g_active_sessions.begin(), g_active_sessions.end(), this),
                            g_active_sessions.end());

    m_ktrace_task.on_stdout = nullptr;
    m_ktrace_task.on_stderr = nullptr;
    m_ktrace_task.on_finished = nullptr;
    m_ktrace_task.stop();
    stop_detached_ktrace_recorder();

    if (m_elf_tab) {
        m_elf_tab->on_symbol_activated = nullptr;
        m_elf_tab.clear();
    }

    if (m_trace_tab) {
        m_trace_tab->on_loaded = nullptr;
        m_trace_tab->on_hotspot_activated = nullptr;
        m_trace_tab->stop_decode();
        m_trace_tab.clear();
    }

    for (QPointer<disasm_split_view>& tab : m_disasm_tabs) {
        if (tab) {
            tab->stop_disassembly();
        }
    }

    m_disasm_tabs.clear();
    m_hotspots_sidebar = nullptr;
    m_hotspots_dock = nullptr;
    m_syscalls_dock = nullptr;
    m_symbols_dock = nullptr;
}

void perf_binaries_session::on_active_file_changed(const QString& file_path) {
    if (m_is_shutting_down || file_path.isEmpty()) {
        return;
    }

    if (probe_elf64_type(file_path) != 0 && file_path != m_binary_path) {
        load_target_binary(file_path, false);
    } else if (probe_file_is_trace(file_path) && file_path != m_trace_path) {
        m_trace_path = file_path;
        refresh_strip();
    }
}

void perf_binaries_session::adopt_elf_view(const QString& binary_path, elf_hex_view* view) {
    if (m_is_shutting_down || !view || !view->is_valid()) {
        return;
    }

    m_elf_tab = view;
    m_elf_tab->on_symbol_activated = [this](const elf_symbol& symbol) { open_disassembly_for_symbol(symbol); };
    m_binary_path = binary_path;
    m_elf_model = view->model();

    m_has_elf_model = true;
    populate_symbol_views();
    refresh_strip();
}

void perf_binaries_session::adopt_trace_tab(const QString& trace_path, trace_summary_tab* view) {
    if (m_is_shutting_down || !view) {
        return;
    }

    if (m_trace_tab && m_trace_tab != view) {
        m_trace_tab->on_loaded = nullptr;
        m_trace_tab->on_hotspot_activated = nullptr;
    }

    m_trace_path = trace_path;
    m_trace_tab = view;
    view->on_loaded = [this](const ktrace_profile_model& model, const ktrace_window_view& window) {
        update_from_trace_window(model, window, false);
    };

    view->on_hotspot_activated = [this](const ktrace_hotspot& hotspot) {
        jump_to_hotspot_location(hotspot.name, hotspot.source_file, hotspot.source_line, false);
    };

    refresh_strip();
}

void perf_binaries_session::discover_initial_targets() {
    QStringList binaries = discover_workspace_binaries(m_host.workspace_root());
    if (!binaries.isEmpty()) {
        load_target_binary(binaries.first(), false);
    }

    for (const QString& trace : discover_workspace_traces(m_host.workspace_root())) {
        if (trace.startsWith(m_host.workspace_root())) {
            m_trace_path = trace;
            break;
        }
    }

    refresh_strip();
}

void perf_binaries_session::register_commands() {
    m_host.set_commands({
        { QStringLiteral("perf.record"), QStringLiteral("Performance"),
          QStringLiteral("Record / Stop ktrace Session"), {}, false,
          [this] { return !m_ktrace_task.is_running(); }, [this] { toggle_recording(); } },
        { QStringLiteral("perf.open_trace"), QStringLiteral("Performance"),
          QStringLiteral("Open Trace..."), {}, false, [] { return true; }, [this] { browse_for_trace(); } },
        { QStringLiteral("perf.show_syscalls"), QStringLiteral("Performance"),
          QStringLiteral("Show Syscalls & Waits"), {}, false,
          [this] { return m_has_active_trace; }, [this] { m_host.reveal_dock_panel(QStringLiteral("syscalls")); } },
        { QStringLiteral("perf.open_elf"), QStringLiteral("Performance"),
          QStringLiteral("Inspect ELF & Hex"), {}, true,
          [this] { return m_has_elf_model; }, [this] { open_elf_inspector_tab(); } },
        { QStringLiteral("perf.open_disasm"), QStringLiteral("Performance"),
          QStringLiteral("Open Source + Disassembly"), {}, false,
          [this] { return m_has_elf_model; }, [this] { open_default_disassembly(); } },
    });
}

QList<picker_choice> perf_binaries_session::list_target_choices() const {
    auto rel_label = [this](const QString& path) {
        return path.startsWith(m_host.workspace_root()) ? QDir(m_host.workspace_root()).relativeFilePath(path) : path;
    };

    QList<picker_choice> choices;
    for (const QString& trace : discover_workspace_traces(m_host.workspace_root())) {
        choices.append({ QStringLiteral("trace:") + trace, QFileInfo(trace).fileName(),
                         QStringLiteral("trace  %1").arg(rel_label(trace)), {}, false, false });
    }

    QStringList binaries = discover_workspace_binaries(m_host.workspace_root());
    if (!m_binary_path.isEmpty() && !binaries.contains(m_binary_path)) {
        binaries.prepend(m_binary_path);
    }

    for (const QString& bin : binaries) {
        choices.append({ QStringLiteral("elf:") + bin, QFileInfo(bin).fileName(),
                         QStringLiteral("ELF  %1").arg(rel_label(bin)), {}, false, false });
    }

    choices.append({ QStringLiteral("__open_trace__"), QStringLiteral("Open Trace..."), {}, {}, true, false });
    choices.append({ QStringLiteral("__open_binary__"), QStringLiteral("Open Binary..."), {}, {}, true, false });
    return choices;
}

void perf_binaries_session::toggle_recording() {
    if (m_is_shutting_down || m_ktrace_task.is_running()) {
        return;
    }

    bool starting = !m_is_recording;
    if (starting) {
        g_capture_sequence++;
        m_recording_output_path = QStringLiteral("/tmp/sheen-capture-%1-%2.ktrace")
                                      .arg(::getpid()).arg(g_capture_sequence);
    }

    m_ktrace_stderr.clear();
    m_ktrace_task.on_stderr = [this](QByteArrayView chunk) { m_ktrace_stderr.append(chunk.data(), chunk.size()); };
    m_ktrace_task.on_finished = [this, starting](const process_exit& exit_result) {
        if (m_is_shutting_down) {
            return;
        }

        if (starting) {
            if (exit_result.exit_code == 0) {
                m_is_recording = true;
                m_host.update_dock_state(QStringLiteral("hotspots"), true, 0);
                m_host.show_status_note(QStringLiteral("Recording kernel trace..."));
            } else {
                QString msg = QString::fromUtf8(m_ktrace_stderr).trimmed();
                m_host.show_status_note(msg.isEmpty() ? QStringLiteral("ktrace start failed") : msg);
            }
        } else {
            m_is_recording = false;
            m_host.update_dock_state(QStringLiteral("hotspots"), false, 0);
            if (exit_result.exit_code == 0 && QFileInfo::exists(m_recording_output_path)) {
                open_trace_tab(m_recording_output_path);
            } else {
                QString msg = QString::fromUtf8(m_ktrace_stderr).trimmed();
                m_host.show_status_note(msg.isEmpty() ? QStringLiteral("ktrace stop failed") : msg);
            }
        }

        refresh_strip();
    };

    QString error_message;
    process_spec spec;
    spec.program = QStringLiteral("/bin/ktrace");
    spec.arguments = starting
                         ? QStringList{ QStringLiteral("start"), QStringLiteral("-o"), m_recording_output_path }
                         : QStringList{ QStringLiteral("stop") };
    if (!m_ktrace_task.start(spec, &error_message)) {
        if (!starting) {
            m_is_recording = false;
        }

        m_host.show_status_note(QStringLiteral("ktrace error: %1").arg(error_message));
    }

    refresh_strip();
}

QString perf_binaries_session::choose_file_to_open(const QString& title, const QString& start_dir,
                                                   const QString& initial_path) {
    if (m_is_shutting_down) {
        return {};
    }

    file_sheet chooser(QApplication::activeWindow(), file_sheet_mode::OPEN, start_dir, {});
    chooser.set_title(title);

    std::weak_ptr<bool> weak_alive = m_alive;
    open_request request;
    bool accepted = chooser.choose_to_open(&request, initial_path);
    auto locked = weak_alive.lock();
    if (!locked || !*locked || !accepted || request.paths.isEmpty()) {
        return {};
    }

    return request.paths.first();
}

void perf_binaries_session::browse_for_trace() {
    QString start_dir = m_trace_path.isEmpty() ? m_host.workspace_root() : QFileInfo(m_trace_path).absolutePath();
    QString chosen = choose_file_to_open(QStringLiteral("Open ktrace Capture or Decoded Database"),
                                         start_dir, m_trace_path);
    if (!chosen.isEmpty()) {
        open_trace_tab(chosen);
    }
}

void perf_binaries_session::browse_for_binary() {
    QString chosen = choose_file_to_open(QStringLiteral("Open ELF64 Binary"),
                                         m_host.workspace_root(), m_binary_path);
    if (!chosen.isEmpty()) {
        load_target_binary(chosen, true);
    }
}

void perf_binaries_session::open_trace_tab(const QString& path) {
    if (m_is_shutting_down || path.isEmpty()) {
        return;
    }

    QString tab_id = QStringLiteral("trace:") + path;
    QString title = QFileInfo(path).fileName();
    if (m_trace_tab && m_trace_tab->trace_path() == path) {
        m_host.open_custom_tab({ tab_id, title, m_trace_tab.data() });
        m_host.reveal_dock_panel(QStringLiteral("hotspots"));
        return;
    }

    auto* view = new trace_summary_tab(path);
    adopt_trace_tab(path, view);
    m_host.open_custom_tab({ tab_id, title, view });
    m_host.reveal_dock_panel(QStringLiteral("hotspots"));
}

void perf_binaries_session::update_from_trace_window(const ktrace_profile_model& model,
                                                     const ktrace_window_view& window,
                                                     bool scoped_to_single_thread) {
    if (m_is_shutting_down || !m_hotspots_dock || !m_syscalls_dock || !m_hotspots_sidebar) {
        return;
    }

    m_has_active_trace = true;
    m_active_hotspots = window.function_hotspots;

    double parallel_cpus = scoped_to_single_thread
                               ? 1.0
                               : static_cast<double>(std::max<uint32_t>(1, model.session.cpu_count));
    double capacity = static_cast<double>(window.window_ns) * parallel_cpus;
    double busy_pct = capacity > 0.0
                          ? std::min(100.0, 100.0 * static_cast<double>(window.busy_cpu_ns) / capacity)
                          : 0.0;
    m_trace_window_readout = QStringLiteral("%1  %2% CPU")
                                 .arg(qstring_duration(window.window_ns), QString::number(busy_pct, 'f', 0));

    QList<ui::table_row> hotspot_rows;
    QList<ui::section_row> side_rows;
    m_active_badges.clear();

    for (size_t idx = 0; idx < window.function_hotspots.size(); idx++) {
        const ktrace_hotspot& hotspot = window.function_hotspots[idx];
        QString loc = hotspot.source_file.empty()
                          ? QStringLiteral("-")
                          : QStringLiteral("%1:%2").arg(QString::fromStdString(hotspot.source_file)).arg(hotspot.source_line);
        QString key = QStringLiteral("hs:%1").arg(idx);
        QString pct_str = QStringLiteral("%1%").arg(hotspot.self_percent, 0, 'f', 1);
        hotspot_rows.append({ key, { QString::fromStdString(hotspot.name), qstring_duration(hotspot.self_ns),
                                     qstring_duration(hotspot.total_ns), pct_str, QString::number(hotspot.call_count), loc } });

        if (idx < MAX_SIDEBAR_ITEMS) {
            side_rows.append({ key, QString::fromStdString(hotspot.name), qstring_duration(hotspot.self_ns),
                               QStringLiteral("%1 self (%2 total)").arg(pct_str, qstring_duration(hotspot.total_ns)) });
        }

        if (!hotspot.source_file.empty() && hotspot.source_line > 0 && hotspot.self_ns > 0) {
            QString resolved = resolve_existing_workspace_file(hotspot.source_file);
            if (!resolved.isEmpty()) {
                std::string pill = format_gutter_percentage(hotspot.self_percent, hotspot.self_ns);
                m_active_badges.append({ resolved, hotspot.source_line, QString::fromStdString(pill),
                                         hotspot.self_percent >= HOT_LINE_PERCENT_THRESHOLD });
            }
        }
    }

    m_hotspots_dock->set_rows(hotspot_rows);
    m_hotspots_sidebar->set_rows(side_rows);
    m_host.set_line_badges(m_active_badges);
    apply_badges_to_open_disasm_tabs();
    m_syscalls_dock->set_rows(build_syscall_table_rows(window.syscall_summaries));
    refresh_strip();
}

QString perf_binaries_session::resolve_existing_workspace_file(const std::string& recorded_path) const {
    QString direct = QDir::cleanPath(QString::fromStdString(recorded_path));
    if (QFileInfo::exists(direct)) {
        return QFileInfo(direct).absoluteFilePath();
    }

    QString under_root = QDir::cleanPath(QDir(m_host.workspace_root()).filePath(direct));
    if (QFileInfo::exists(under_root)) {
        return QFileInfo(under_root).absoluteFilePath();
    }

    return {};
}

void perf_binaries_session::handle_sidebar_or_hotspot_row(const QString& row_id) {
    if (row_id.startsWith(QLatin1String("sym:"))) {
        activate_symbol_by_index(row_id.mid(4));
        return;
    }

    if (!row_id.startsWith(QLatin1String("hs:"))) {
        return;
    }

    bool ok = false;
    size_t idx = row_id.mid(3).toULongLong(&ok);
    if (ok && idx < m_active_hotspots.size()) {
        const ktrace_hotspot& hotspot = m_active_hotspots[idx];
        jump_to_hotspot_location(hotspot.name, hotspot.source_file, hotspot.source_line, false);
    }
}

void perf_binaries_session::jump_to_hotspot_location(const std::string& function_name,
                                                     const std::string& source_file,
                                                     int source_line, bool prefer_disasm) {
    if (m_has_elf_model && !function_name.empty() && (prefer_disasm || source_file.empty())) {
        for (const elf_symbol& symbol : m_elf_model.symbols) {
            if (symbol.symbol_type != ELF_STT_FUNC) {
                continue;
            }

            if (symbol.demangled_name == function_name || symbol.raw_name == function_name ||
                strip_parameter_list(symbol.demangled_name) == function_name) {
                open_disassembly_for_symbol(symbol);
                return;
            }
        }
    }

    if (!source_file.empty() && source_line > 0) {
        QString resolved = resolve_existing_workspace_file(source_file);
        if (!resolved.isEmpty()) {
            m_host.open_file_at_line(resolved, source_line);
        }
    }
}

void perf_binaries_session::load_target_binary(const QString& path, bool open_tab) {
    if (m_is_shutting_down) {
        return;
    }

    QFileInfo file_info(path);
    if (!file_info.isFile() || file_info.size() > MAX_ELF_FILE_BYTES) {
        m_host.show_status_note(QStringLiteral("%1 exceeds maximum ELF size").arg(file_info.fileName()));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_host.show_status_note(QStringLiteral("Cannot read %1").arg(file_info.fileName()));
        return;
    }

    QByteArray bytes = file.read(MAX_ELF_FILE_BYTES);
    std::span<const uint8_t> raw_span(reinterpret_cast<const uint8_t*>(bytes.constData()),
                                      static_cast<size_t>(bytes.size()));
    elf_binary_model parsed;
    std::string error_message;
    if (!parse_elf_binary(raw_span, parsed, error_message)) {
        m_host.show_status_note(QString::fromStdString(error_message));
        return;
    }

    m_binary_path = path;
    m_elf_model = std::move(parsed);
    m_has_elf_model = true;
    if (m_elf_tab && m_elf_tab->binary_path() == path) {
        m_elf_tab->reload_binary();
    }

    populate_symbol_views();
    refresh_strip();

    if (open_tab) {
        open_elf_inspector_tab();
    }
}

void perf_binaries_session::populate_symbol_views() {
    if (!m_symbols_dock || !m_hotspots_sidebar) {
        return;
    }

    QList<ui::table_row> dock_rows;
    std::vector<size_t> ranked_functions;
    for (size_t idx = 0; idx < m_elf_model.symbols.size(); idx++) {
        const elf_symbol& symbol = m_elf_model.symbols[idx];
        QString vma_text = QStringLiteral("0x%1").arg(symbol.vma, 8, 16, QLatin1Char('0'));
        dock_rows.append({
            QString::number(idx),
            { QString::fromStdString(symbol.demangled_name), QString::fromStdString(symbol.section_name),
              vma_text, format_byte_count(symbol.size), QString::fromStdString(symbol.binding_name),
              QString::fromStdString(symbol.type_name) },
        });

        if (symbol.symbol_type == ELF_STT_FUNC || symbol.section_name.starts_with(".text")) {
            ranked_functions.push_back(idx);
        }
    }

    m_symbols_dock->set_rows(dock_rows);
    if (m_has_active_trace) {
        return;
    }

    std::sort(ranked_functions.begin(), ranked_functions.end(), [this](size_t lhs, size_t rhs) {
        if (m_elf_model.symbols[lhs].size != m_elf_model.symbols[rhs].size) {
            return m_elf_model.symbols[lhs].size > m_elf_model.symbols[rhs].size;
        }

        return m_elf_model.symbols[lhs].demangled_name < m_elf_model.symbols[rhs].demangled_name;
    });

    QList<ui::section_row> top_rows;
    size_t limit = std::min(ranked_functions.size(), MAX_SIDEBAR_ITEMS);
    for (size_t i = 0; i < limit; i++) {
        size_t sym_idx = ranked_functions[i];
        const elf_symbol& symbol = m_elf_model.symbols[sym_idx];
        top_rows.append({
            QStringLiteral("sym:%1").arg(sym_idx), QString::fromStdString(symbol.demangled_name),
            format_byte_count(symbol.size),
            QStringLiteral("0x%1 (%2)")
                .arg(symbol.vma, 8, 16, QLatin1Char('0'))
                .arg(QString::fromStdString(symbol.section_name)),
        });
    }

    m_hotspots_sidebar->set_rows(top_rows);
}

void perf_binaries_session::open_elf_inspector_tab() {
    if (m_is_shutting_down || m_binary_path.isEmpty()) {
        return;
    }

    QString tab_id = QStringLiteral("elf:") + m_binary_path;
    QString title = QStringLiteral("%1 - ELF").arg(QFileInfo(m_binary_path).fileName());
    if (m_elf_tab && m_elf_tab->binary_path() == m_binary_path) {
        m_host.open_custom_tab({ tab_id, title, m_elf_tab.data() });
        m_host.reveal_dock_panel(QStringLiteral("symbols"));
        return;
    }

    auto* view = new elf_hex_view(m_binary_path);
    view->on_symbol_activated = [this](const elf_symbol& symbol) { open_disassembly_for_symbol(symbol); };
    m_elf_tab = view;
    m_host.open_custom_tab({ tab_id, title, view });
    m_host.reveal_dock_panel(QStringLiteral("symbols"));
}

void perf_binaries_session::activate_symbol_by_index(const QString& row_id) {
    bool ok = false;
    size_t idx = row_id.toULongLong(&ok);
    if (!ok || !m_has_elf_model || idx >= m_elf_model.symbols.size()) {
        return;
    }

    const elf_symbol& symbol = m_elf_model.symbols[idx];
    if (symbol.symbol_type == ELF_STT_FUNC || symbol.section_name.starts_with(".text")) {
        open_disassembly_for_symbol(symbol);
        return;
    }

    open_elf_inspector_tab();
    byte_range range;
    if (m_elf_tab && symbol_file_byte_range(m_elf_model, symbol, range)) {
        m_elf_tab->select_byte_range(range, QString::fromStdString(symbol.demangled_name));
    }
}

void perf_binaries_session::preview_symbol_by_index(const QString& row_id) {
    bool ok = false;
    size_t idx = row_id.toULongLong(&ok);
    byte_range range;
    if (ok && m_has_elf_model && idx < m_elf_model.symbols.size() &&
        m_elf_tab && symbol_file_byte_range(m_elf_model, m_elf_model.symbols[idx], range)) {
        m_elf_tab->select_byte_range(range, QString::fromStdString(m_elf_model.symbols[idx].demangled_name));
    }
}

void perf_binaries_session::open_default_disassembly() {
    if (!m_has_elf_model) {
        return;
    }

    const elf_symbol* chosen = nullptr;
    for (const elf_symbol& symbol : m_elf_model.symbols) {
        if ((symbol.symbol_type == ELF_STT_FUNC || symbol.section_name.starts_with(".text")) &&
            (!chosen || symbol.size > chosen->size)) {
            chosen = &symbol;
        }
    }

    open_disassembly_for_symbol(chosen ? *chosen : elf_symbol{});
}

void perf_binaries_session::apply_badges_to_open_disasm_tabs() {
    QHash<QString, QList<disasm_line_badge>> by_file;
    for (const line_badge_item& item : m_active_badges) {
        by_file[item.file_path].append({ item.line, item.label, item.is_hot });
    }

    for (const QPointer<disasm_split_view>& tab : m_disasm_tabs) {
        if (!tab) {
            continue;
        }

        for (auto it = by_file.cbegin(); it != by_file.cend(); ++it) {
            tab->apply_source_line_badges(it.key(), it.value());
        }
    }
}

void perf_binaries_session::open_disassembly_for_symbol(const elf_symbol& symbol) {
    if (m_is_shutting_down || !m_has_elf_model || m_binary_path.isEmpty()) {
        return;
    }

    QString key = symbol.raw_name.empty() ? QStringLiteral("text") : QString::fromStdString(symbol.raw_name);
    QString tab_id = QStringLiteral("disasm:%1:%2").arg(m_binary_path, key);
    QString label = symbol.demangled_name.empty()
                        ? QFileInfo(m_binary_path).fileName()
                        : QString::fromStdString(symbol.demangled_name).left(SYMBOL_TAB_TITLE_MAX_CHARS);
    QString title = QStringLiteral("%1 - Disasm").arg(label);

    for (const QPointer<disasm_split_view>& existing : m_disasm_tabs) {
        if (existing && existing->binary_path() == m_binary_path && existing->symbol_raw_name() == symbol.raw_name) {
            m_host.open_custom_tab({ tab_id, title, existing.data() });
            return;
        }
    }

    auto* view = new disasm_split_view(m_binary_path, m_host.workspace_root(),
                                       m_elf_model.header.machine, m_elf_model.header.dwarf_version, symbol);
    m_disasm_tabs.emplace_back(view);
    apply_badges_to_open_disasm_tabs();
    m_host.open_custom_tab({ tab_id, title, view });
}

void perf_binaries_session::refresh_strip() {
    if (m_is_shutting_down) {
        return;
    }

    QString picker_label = !m_trace_path.isEmpty()
                               ? QFileInfo(m_trace_path).fileName()
                               : (!m_binary_path.isEmpty() ? QFileInfo(m_binary_path).fileName()
                                                           : QStringLiteral("Select Target"));
    QString elf_summary = m_has_elf_model
                              ? QStringLiteral("%1 %2").arg(QString::fromStdString(m_elf_model.header.type_name),
                                                            QString::fromStdString(m_elf_model.header.arch_name))
                              : QStringLiteral("Ready");
    QString summary = m_is_recording ? QStringLiteral("Recording...")
                                     : (!m_trace_window_readout.isEmpty() ? m_trace_window_readout : elf_summary);
    QString record_label = m_is_recording ? QStringLiteral("Stop") : QStringLiteral("Record");

    auto on_pick = [this](const QString& id) {
        if (id == QLatin1String("__open_trace__")) {
            browse_for_trace();
        } else if (id == QLatin1String("__open_binary__")) {
            browse_for_binary();
        } else if (id.startsWith(QLatin1String("trace:"))) {
            open_trace_tab(id.mid(6));
        } else if (id.startsWith(QLatin1String("elf:"))) {
            load_target_binary(id.mid(4), true);
        }
    };

    m_host.set_strip({
        { { QStringLiteral("target"), picker_label, {}, QStringLiteral("Filter traces and ELF binaries..."), true,
            [this] { return list_target_choices(); }, on_pick } },
        { { QStringLiteral("record"), record_label, QStringLiteral("Start or stop a kernel ktrace capture"),
            !m_ktrace_task.is_running(), m_is_recording, [this] { toggle_recording(); } },
          { QStringLiteral("open_trace"), QStringLiteral("Open Trace..."),
            QStringLiteral("Open a .ktrace capture or ktrace-decode SQLite database"), true, false,
            [this] { browse_for_trace(); } },
          { QStringLiteral("source_disasm"), QStringLiteral("Source + Disasm"),
            QStringLiteral("Open synchronized C++ source and objdump disassembly"), m_has_elf_model, false,
            [this] { open_default_disassembly(); } },
          { QStringLiteral("elf_hex"), QStringLiteral("ELF & Hex"),
            QStringLiteral("Inspect ELF64 sections and Hex/ASCII bytes"), m_has_elf_model, false,
            [this] { open_elf_inspector_tab(); } } },
        { { QStringLiteral("window_summary"), summary, -1, false } },
        { QStringLiteral(".ktrace"), QStringLiteral(".db"), QStringLiteral(".elf"), QStringLiteral(".o") },
    });
}

static bool qualifies_perf_binaries(const workspace_files& files) {
    if (files.has_suffix(QStringLiteral(".ktrace")) || files.has_suffix(QStringLiteral(".elf"))) {
        return true;
    }

    if (files.folder_name.isEmpty()) {
        return false;
    }

    QDir root_dir(files.root_path);
    if (files.contains(files.folder_name) && probe_elf64_type(root_dir.filePath(files.folder_name)) != 0) {
        return true;
    }

    return files.contains(QStringLiteral("build")) &&
           probe_elf64_type(root_dir.filePath(QStringLiteral("build/") + files.folder_name)) != 0;
}

static std::unique_ptr<extension_session> create_perf_binaries_session(extension_host& host) {
    return std::make_unique<perf_binaries_session>(host);
}

extension_descriptor perf_binaries_extension() {
    return { EXTENSION_ID, "Performance & Binaries",
             "Profile ktrace captures, hotspots, syscalls, ELF64 sections, Hex/ASCII bytes, and disassembly.",
             extension_icon::BINARY, "Trace capture or ELF64 binary detected", true,
             PERF_BINARIES_DEPS, PERF_BINARIES_OPENERS,
             qualifies_perf_binaries, create_perf_binaries_session };
}
} // namespace sheen
