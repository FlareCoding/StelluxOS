#include "extensions/perf_binaries/perf_binaries_extension.h"
#include "core/editor_core.h"
#include "extensions/extension_host.h"
#include "extensions/perf_binaries/disasm_split_view.h"
#include "extensions/perf_binaries/elf_hex_view.h"
#include "extensions/perf_binaries/elf_model.h"
#include "ui/file_sheet.h"
#include "ui/ui_kit.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>

#include <algorithm>
#include <memory>
#include <vector>

namespace sheen {

static constexpr const char* EXTENSION_ID = "perf_binaries";
static constexpr qint64 ELF_MIN_FILE_BYTES = 64;
static constexpr qint64 PROBE_HEADER_BYTES = 24;
static constexpr size_t MAX_SIDEBAR_SYMBOLS = 24;
static constexpr qsizetype SYMBOL_TAB_TITLE_MAX_CHARS = 28;

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

class perf_binaries_session final : public extension_session {
public:
    explicit perf_binaries_session(extension_host& host);
    ~perf_binaries_session() override;

    void shutdown() override;
    void on_active_file_changed(const QString& file_path) override;

    QString workspace_root() const { return m_host.workspace_root(); }
    void adopt_elf_view(const QString& binary_path, elf_hex_view* view);

private:
    void register_commands();
    QList<picker_choice> list_target_choices() const;
    void browse_for_binary();
    void load_target_binary(const QString& path, bool open_tab);
    void populate_symbol_views();
    void open_elf_inspector_tab();
    void preview_symbol_by_index(const QString& row_id);
    void activate_symbol_by_index(const QString& row_id);
    void open_default_disassembly();
    void open_disassembly_for_symbol(const elf_symbol& symbol);
    void refresh_strip();

    extension_host& m_host;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    bool m_is_shutting_down = false;

    ui::ui_section_list* m_top_symbols = nullptr;
    ui::ui_table_view* m_symbols_dock = nullptr;
    QPointer<elf_hex_view> m_elf_tab;
    std::vector<QPointer<disasm_split_view>> m_disasm_tabs;

    QString m_binary_path;
    elf_binary_model m_elf_model;
    bool m_has_elf_model = false;
};

} // namespace

static std::vector<perf_binaries_session*> g_active_sessions;

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

static constexpr file_opener_spec PERF_BINARIES_OPENERS[] = {
    {
        "elf",
        "Open ELF & Hex",
        can_open_elf_file,
        create_elf_file_view,
    },
};

perf_binaries_session::perf_binaries_session(extension_host& host)
    : m_host(host),
      m_top_symbols(new ui::ui_section_list(QStringLiteral("Open..."))),
      m_symbols_dock(new ui::ui_table_view({
          { QStringLiteral("Symbol"), 0, false },  { QStringLiteral("Section"), 96, false },
          { QStringLiteral("VMA"), 112, false },   { QStringLiteral("Size"), 84, true },
          { QStringLiteral("Bind"), 76, false },   { QStringLiteral("Type"), 76, false },
      })) {
    g_active_sessions.push_back(this);
    m_top_symbols->set_empty_text(QStringLiteral("Select an ELF64 binary"));
    m_top_symbols->on_action_clicked = [this] { browse_for_binary(); };
    m_top_symbols->on_row_activated = [this](const QString& row_id) { activate_symbol_by_index(row_id); };
    m_symbols_dock->on_row_clicked = [this](const QString& row_id) { preview_symbol_by_index(row_id); };
    m_symbols_dock->on_row_activated = [this](const QString& row_id) { activate_symbol_by_index(row_id); };

    m_host.set_sidebar_sections({ { QStringLiteral("top_symbols"), QStringLiteral("Top Symbols"), m_top_symbols } });
    m_host.set_dock_panels({ { QStringLiteral("symbols"), QStringLiteral("Symbols"), m_symbols_dock, false, 0 } });
    register_commands();

    QStringList initial = discover_workspace_binaries(m_host.workspace_root());
    if (!initial.isEmpty()) {
        load_target_binary(initial.first(), false);
    } else {
        refresh_strip();
    }
}

perf_binaries_session::~perf_binaries_session() {
    shutdown();
}

void perf_binaries_session::shutdown() {
    if (m_is_shutting_down) {
        return;
    }

    m_is_shutting_down = true;
    *m_alive = false;
    g_active_sessions.erase(std::remove(g_active_sessions.begin(), g_active_sessions.end(), this),
                            g_active_sessions.end());

    if (m_elf_tab) {
        m_elf_tab->on_symbol_activated = nullptr;
        m_elf_tab.clear();
    }

    for (QPointer<disasm_split_view>& tab : m_disasm_tabs) {
        if (tab) {
            tab->stop_disassembly();
        }
    }

    m_disasm_tabs.clear();
    m_top_symbols = nullptr;
    m_symbols_dock = nullptr;
}

void perf_binaries_session::on_active_file_changed(const QString& file_path) {
    if (!m_is_shutting_down && !file_path.isEmpty() &&
        probe_elf64_type(file_path) != 0 && file_path != m_binary_path) {
        load_target_binary(file_path, false);
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

void perf_binaries_session::register_commands() {
    m_host.set_commands({
        { QStringLiteral("perf.open_elf"), QStringLiteral("Performance"),
          QStringLiteral("Inspect ELF & Hex"), QString(), false,
          [this] { return m_has_elf_model; },
          [this] { open_elf_inspector_tab(); } },
        { QStringLiteral("perf.open_disasm"), QStringLiteral("Performance"),
          QStringLiteral("Open Source + Disassembly"), QString(), false,
          [this] { return m_has_elf_model; },
          [this] { open_default_disassembly(); } },
        { QStringLiteral("perf.browse_binary"), QStringLiteral("Performance"),
          QStringLiteral("Choose Binary..."), QString(), true,
          [] { return true; },
          [this] { browse_for_binary(); } },
    });
}

QList<picker_choice> perf_binaries_session::list_target_choices() const {
    QStringList paths = discover_workspace_binaries(m_host.workspace_root());
    if (!m_binary_path.isEmpty() && !paths.contains(m_binary_path)) {
        paths.prepend(m_binary_path);
    }

    QList<picker_choice> choices;
    for (const QString& path : paths) {
        QString relative = path.startsWith(m_host.workspace_root())
                               ? QDir(m_host.workspace_root()).relativeFilePath(path)
                               : path;
        choices.append({ path, QFileInfo(path).fileName(),
                         QStringLiteral("ELF  %1").arg(relative), {}, false, false });
    }

    choices.append({ QStringLiteral("__browse__"), QStringLiteral("Open Binary..."), {}, {}, true, false });
    return choices;
}

void perf_binaries_session::browse_for_binary() {
    if (m_is_shutting_down) {
        return;
    }

    QWidget* window_widget = QApplication::activeWindow();
    file_sheet chooser(window_widget, file_sheet_mode::OPEN, m_host.workspace_root(), {});
    chooser.set_title(QStringLiteral("Open ELF64 Binary"));

    std::weak_ptr<bool> weak_alive = m_alive;
    open_request request;
    bool accepted = chooser.choose_to_open(&request, m_binary_path);
    auto locked = weak_alive.lock();
    if (!locked || !*locked || !accepted || request.paths.isEmpty()) {
        return;
    }

    load_target_binary(request.paths.first(), true);
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
    if (!m_symbols_dock || !m_top_symbols) {
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

    std::sort(ranked_functions.begin(), ranked_functions.end(), [this](size_t lhs, size_t rhs) {
        if (m_elf_model.symbols[lhs].size != m_elf_model.symbols[rhs].size) {
            return m_elf_model.symbols[lhs].size > m_elf_model.symbols[rhs].size;
        }

        return m_elf_model.symbols[lhs].demangled_name < m_elf_model.symbols[rhs].demangled_name;
    });

    QList<ui::section_row> top_rows;
    size_t limit = std::min(ranked_functions.size(), MAX_SIDEBAR_SYMBOLS);
    for (size_t i = 0; i < limit; i++) {
        size_t sym_idx = ranked_functions[i];
        const elf_symbol& symbol = m_elf_model.symbols[sym_idx];
        top_rows.append({
            QString::number(sym_idx),
            QString::fromStdString(symbol.demangled_name),
            format_byte_count(symbol.size),
            QStringLiteral("0x%1 (%2)")
                .arg(symbol.vma, 8, 16, QLatin1Char('0'))
                .arg(QString::fromStdString(symbol.section_name)),
        });
    }

    m_top_symbols->set_rows(top_rows);
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
    m_host.open_custom_tab({ tab_id, title, view });
}

void perf_binaries_session::refresh_strip() {
    if (m_is_shutting_down) {
        return;
    }

    QString picker_label = m_binary_path.isEmpty()
                               ? QStringLiteral("Select Binary")
                               : QFileInfo(m_binary_path).fileName();
    QString summary = QStringLiteral("No binary selected");
    if (m_has_elf_model) {
        QString dwarf = m_elf_model.header.dwarf_version > 0
                            ? QStringLiteral("DWARF v%1").arg(m_elf_model.header.dwarf_version)
                            : (m_elf_model.header.has_dwarf ? QStringLiteral("DWARF") : QStringLiteral("Stripped"));
        summary = QStringLiteral("%1 %2  %3")
                      .arg(QString::fromStdString(m_elf_model.header.type_name),
                           QString::fromStdString(m_elf_model.header.arch_name), dwarf);
    }

    m_host.set_strip({
        { { QStringLiteral("target_binary"), picker_label, {}, QStringLiteral("Filter ELF binaries..."), true,
            [this] { return list_target_choices(); },
            [this](const QString& choice_id) {
                if (choice_id == QLatin1String("__browse__")) {
                    browse_for_binary();
                } else {
                    load_target_binary(choice_id, true);
                }
            } } },
        { { QStringLiteral("elf_hex"), QStringLiteral("ELF & Hex"),
            QStringLiteral("Inspect ELF64 sections and Hex/ASCII bytes"), m_has_elf_model, false,
            [this] { open_elf_inspector_tab(); } },
          { QStringLiteral("source_disasm"), QStringLiteral("Source + Disasm"),
            QStringLiteral("Open synchronized C++ source and objdump disassembly"), m_has_elf_model, false,
            [this] { open_default_disassembly(); } } },
        { { QStringLiteral("binary_summary"), summary, -1, false } },
        { QStringLiteral(".elf"), QStringLiteral(".o") },
    });
}

static bool qualifies_perf_binaries(const workspace_files& files) {
    if (files.has_suffix(QStringLiteral(".elf")) || files.folder_name.isEmpty()) {
        return files.has_suffix(QStringLiteral(".elf"));
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
             "Inspect ELF64 sections, symbols, Hex/ASCII bytes, and DWARF-mapped disassembly.",
             extension_icon::BINARY, "ELF64 binary detected", true,
             PERF_BINARIES_DEPS, PERF_BINARIES_OPENERS,
             qualifies_perf_binaries, create_perf_binaries_session };
}

} // namespace sheen
