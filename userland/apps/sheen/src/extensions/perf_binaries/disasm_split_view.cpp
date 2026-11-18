#include "extensions/perf_binaries/disasm_split_view.h"
#include "core/kiln.h"
#include "extensions/perf_binaries/elf_hex_view.h"
#include "ui/chrome.h"
#include "ui/ui_kit.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <Scintilla.h>
#include <ScintillaEditBase.h>

#include <algorithm>
#include <cstdio>

namespace sheen {

static constexpr int HEADER_BAR_HEIGHT = 32;
static constexpr int HEADER_PADDING_X = 12;
static constexpr uint64_t DEFAULT_DISASM_FALLBACK_BYTES = 256ull;
static constexpr int MAX_SOURCE_SEARCH_FILES = 2000;
static constexpr int MAX_SOURCE_SEARCH_DEPTH = 4;
static constexpr double HOT_BADGE_PERCENT_THRESHOLD = 10.0;

static void set_scintilla_readonly_text(ScintillaEditBase* view, const QByteArray& utf8, const QString& file_hint) {
    view->send(SCI_SETREADONLY, 0);
    view->send(SCI_SETTEXT, 0, reinterpret_cast<sptr_t>(utf8.constData()));
    view->send(SCI_SETREADONLY, 1);

    qsizetype newline = utf8.indexOf('\n');
    QByteArrayView first_line = newline >= 0 ? QByteArrayView(utf8).first(newline) : QByteArrayView(utf8);
    apply_language(view, find_language(file_hint, first_line));
    fit_line_numbers(view);
}

static bool is_skipped_search_directory(const QString& dir_name) {
    return dir_name.startsWith(QLatin1Char('.')) || dir_name == QLatin1String("build") ||
           dir_name == QLatin1String("toolchain") || dir_name == QLatin1String("node_modules");
}

static QString find_matching_source_bounded(const QString& root_dir, std::string_view recorded_path,
                                            int depth, int& remaining_budget) {
    if (depth > MAX_SOURCE_SEARCH_DEPTH || remaining_budget <= 0) {
        return {};
    }

    QDir dir(root_dir);
    const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& file_info : files) {
        if (--remaining_budget < 0) {
            return {};
        }

        QString abs_path = file_info.absoluteFilePath();
        if (source_paths_match(recorded_path, abs_path.toStdString())) {
            return abs_path;
        }
    }

    const QFileInfoList subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& subdir : subdirs) {
        if (is_skipped_search_directory(subdir.fileName())) {
            continue;
        }

        QString found = find_matching_source_bounded(subdir.absoluteFilePath(), recorded_path,
                                                     depth + 1, remaining_budget);
        if (!found.isEmpty()) {
            return found;
        }
    }

    return {};
}

QString infer_project_root_for_binary(const QString& binary_path, const QString& workspace_root) {
    if (!workspace_root.isEmpty()) {
        return workspace_root;
    }

    QDir binary_dir = QFileInfo(binary_path).absoluteDir();
    if (binary_dir.dirName() == QLatin1String("build") && binary_dir.cdUp()) {
        return binary_dir.absolutePath();
    }

    return binary_dir.absolutePath();
}

disasm_split_view::disasm_split_view(const QString& binary_path, const QString& workspace_root,
                                     uint16_t elf_machine, uint16_t dwarf_version,
                                     const elf_symbol& target_symbol, QWidget* parent)
    : QWidget(parent),
      m_binary_path(binary_path),
      m_workspace_root(infer_project_root_for_binary(binary_path, workspace_root)),
      m_elf_machine(elf_machine),
      m_dwarf_version(dwarf_version),
      m_symbol(target_symbol) {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* top_bar = new perf_strip_bar(HEADER_BAR_HEIGHT, false, true, this);
    auto* bar_layout = new QHBoxLayout(top_bar);
    bar_layout->setContentsMargins(HEADER_PADDING_X, 0, HEADER_PADDING_X, 1);

    m_status_label = new QLabel(top_bar);
    m_status_label->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
    m_status_label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                                      .arg(token(STLXTHEME_TEXT_SECONDARY).name(QColor::HexArgb)));
    bar_layout->addWidget(m_status_label, 1);
    root_layout->addWidget(top_bar);

    m_split = new ui::ui_split_code_view(this);
    root_layout->addWidget(m_split, 1);

    start_objdump();
}

disasm_split_view::~disasm_split_view() {
    stop_disassembly();
}

void disasm_split_view::stop_disassembly() {
    m_task.on_stdout = nullptr;
    m_task.on_stderr = nullptr;
    m_task.on_finished = nullptr;
    m_task.stop();
}

void disasm_split_view::start_objdump() {
    m_stdout_buffer.clear();
    m_stderr_buffer.clear();

    QStringList args = { QStringLiteral("-d"), QStringLiteral("-l"), QStringLiteral("-C") };
    if (m_elf_machine == ELF_MACHINE_X86_64) {
        args << QStringLiteral("-M") << QStringLiteral("intel");
    }

    bool has_real_section = !m_symbol.section_name.empty() && m_symbol.section_name != "UND" &&
                            m_symbol.section_name != "ABS" && m_symbol.section_name != "COM" &&
                            m_symbol.section_name != "RSV";
    if (has_real_section) {
        args << QStringLiteral("-j") << QString::fromStdString(m_symbol.section_name);
    }

    if (!m_symbol.raw_name.empty()) {
        uint64_t span = m_symbol.size > 0 ? m_symbol.size : DEFAULT_DISASM_FALLBACK_BYTES;
        uint64_t stop_vma = m_symbol.vma <= UINT64_MAX - span ? (m_symbol.vma + span) : UINT64_MAX;
        args << QStringLiteral("--start-address=0x%1").arg(m_symbol.vma, 0, 16);
        args << QStringLiteral("--stop-address=0x%1").arg(stop_vma, 0, 16);
    }

    args << QStringLiteral("--") << m_binary_path;

    m_task.on_stdout = [this](QByteArrayView chunk) { m_stdout_buffer.append(chunk.data(), chunk.size()); };
    m_task.on_stderr = [this](QByteArrayView chunk) { m_stderr_buffer.append(chunk.data(), chunk.size()); };
    m_task.on_finished = [this](const process_exit& result) { finish_objdump(result); };

    QString sym_label = m_symbol.demangled_name.empty()
                            ? QFileInfo(m_binary_path).fileName()
                            : QString::fromStdString(m_symbol.demangled_name);
    m_status_label->setText(QStringLiteral("Disassembling %1...").arg(sym_label));

    QString start_error;
    process_spec spec;
    spec.program = QStringLiteral("/usr/bin/objdump");
    spec.arguments = args;
    spec.working_directory = m_workspace_root;
    if (!m_task.start(spec, &start_error)) {
        m_status_label->setText(QStringLiteral("objdump unavailable: %1").arg(start_error));
        set_scintilla_readonly_text(
            m_split->right_view(),
            QStringLiteral("// Install GNU Binary Utilities via Extensions > Set Up or `stlxpkg install binutils`.\n")
                .toUtf8(),
            QStringLiteral("disasm.s"));
    }
}

void disasm_split_view::finish_objdump(const process_exit& exit_result) {
    if (exit_result.exit_code != 0 && m_stdout_buffer.isEmpty()) {
        QString err_text = QString::fromUtf8(m_stderr_buffer).trimmed();
        QString shown = err_text.isEmpty() ? QStringLiteral("objdump exited with error") : err_text;
        m_status_label->setText(shown);
        set_scintilla_readonly_text(m_split->right_view(), (QStringLiteral("// ") + shown + QLatin1Char('\n')).toUtf8(),
                                    QStringLiteral("disasm.s"));
        return;
    }

    std::string parse_error;
    std::string_view text_view(m_stdout_buffer.constData(), static_cast<size_t>(m_stdout_buffer.size()));
    if (!parse_objdump_disassembly(text_view, m_mapped, parse_error)) {
        QString shown = QString::fromStdString(parse_error);
        m_status_label->setText(shown);
        set_scintilla_readonly_text(m_split->right_view(), (QStringLiteral("// ") + shown + QLatin1Char('\n')).toUtf8(),
                                    QStringLiteral("disasm.s"));
        return;
    }

    QByteArray disasm_text;
    for (size_t idx = 0; idx < m_mapped.instructions.size(); idx++) {
        const disasm_instruction_row& row = m_mapped.instructions[idx];
        char line_buf[512] = {};
        std::snprintf(line_buf, sizeof(line_buf), "0x%08llx  %-22s  %s",
                      static_cast<unsigned long long>(row.address),
                      row.raw_bytes_text.c_str(), row.mnemonic_and_operands.c_str());
        if (idx > 0) {
            disasm_text.append('\n');
        }

        disasm_text.append(line_buf);
    }

    set_scintilla_readonly_text(m_split->right_view(), disasm_text, QStringLiteral("disasm.s"));
    QString resolved_source = resolve_source_path(m_mapped.primary_source_file);
    load_source_pane(resolved_source);

    auto std_pairs = build_split_view_line_pairs(m_mapped);
    QList<std::pair<int, int>> qt_pairs;
    qt_pairs.reserve(static_cast<qsizetype>(std_pairs.size()));
    for (const auto& item : std_pairs) {
        qt_pairs.append(item);
    }

    m_split->set_line_mapping(qt_pairs);
    if (!qt_pairs.isEmpty()) {
        int first_src_line = std::max(1, qt_pairs.front().first);
        sptr_t pos = m_split->left_view()->send(SCI_POSITIONFROMLINE, first_src_line - 1);
        m_split->left_view()->send(SCI_GOTOPOS, pos);
    }

    refresh_stored_source_badges();

    QString arch_tag = m_elf_machine == ELF_MACHINE_X86_64 ? QStringLiteral("x86_64") : QStringLiteral("aarch64");
    QString dwarf_tag = m_dwarf_version > 0 ? QStringLiteral("DWARF v%1").arg(m_dwarf_version)
                                            : QStringLiteral("No DWARF");
    QString src_tag = resolved_source.isEmpty() ? QStringLiteral("unmapped") : QFileInfo(resolved_source).fileName();
    m_status_label->setText(QStringLiteral("%1   %2   %3   %4 instructions   %5")
                                .arg(QString::fromStdString(m_symbol.demangled_name), arch_tag, dwarf_tag)
                                .arg(m_mapped.instructions.size())
                                .arg(src_tag));
}

QString disasm_split_view::resolve_source_path(std::string_view recorded_path) const {
    if (recorded_path.empty()) {
        return {};
    }

    QString direct = QDir::cleanPath(QString::fromUtf8(recorded_path.data(), static_cast<qsizetype>(recorded_path.size())));
    if (QFileInfo::exists(direct)) {
        return QFileInfo(direct).absoluteFilePath();
    }

    if (m_workspace_root.isEmpty()) {
        return {};
    }

    QString under_root = QDir::cleanPath(QDir(m_workspace_root).filePath(direct));
    if (QFileInfo::exists(under_root)) {
        return QFileInfo(under_root).absoluteFilePath();
    }

    int budget = MAX_SOURCE_SEARCH_FILES;
    return find_matching_source_bounded(m_workspace_root, recorded_path, 0, budget);
}

void disasm_split_view::load_source_pane(const QString& resolved_path) {
    if (!resolved_path.isEmpty()) {
        QFile src_file(resolved_path);
        if (src_file.open(QIODevice::ReadOnly)) {
            set_scintilla_readonly_text(m_split->left_view(), src_file.read(MAX_ELF_FILE_BYTES), resolved_path);
            return;
        }
    }

    QString fallback = m_mapped.primary_source_file.empty()
                           ? QStringLiteral("// Binary has no DWARF line correlation for this symbol.\n")
                           : QStringLiteral("// Source file not found on disk: %1\n")
                                 .arg(QString::fromStdString(m_mapped.primary_source_file));
    set_scintilla_readonly_text(m_split->left_view(), fallback.toUtf8(), QStringLiteral("missing.cpp"));
}

void disasm_split_view::apply_address_samples(std::span<const address_sample> samples, uint64_t denominator_override) {
    disasm_gutter_profile profile = compute_disasm_gutter_profile(m_mapped, samples, {}, denominator_override);

    ScintillaEditBase* right = m_split->right_view();
    right->send(SCI_EOLANNOTATIONCLEARALL);
    if (!profile.instruction_costs.empty()) {
        right->send(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_STADIUM);
        for (const instruction_gutter_cost& cost : profile.instruction_costs) {
            std::string text = format_gutter_percentage(cost.percent, cost.weight);
            right->send(SCI_EOLANNOTATIONSETTEXT, static_cast<uptr_t>(cost.row_index),
                        reinterpret_cast<sptr_t>(text.c_str()));
            right->send(SCI_EOLANNOTATIONSETSTYLE, static_cast<uptr_t>(cost.row_index),
                        cost.percent >= HOT_BADGE_PERCENT_THRESHOLD ? STYLE_BADGE_HOT : STYLE_BADGE_DIM);
        }
    }

    ScintillaEditBase* left = m_split->left_view();
    left->send(SCI_EOLANNOTATIONCLEARALL);
    if (!profile.line_costs.empty()) {
        left->send(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_STADIUM);
        int max_line = static_cast<int>(left->send(SCI_GETLINECOUNT));
        for (const source_line_gutter_cost& cost : profile.line_costs) {
            if (cost.source_line < 1 || cost.source_line > max_line) {
                continue;
            }

            std::string text = format_gutter_percentage(cost.percent, cost.weight);
            left->send(SCI_EOLANNOTATIONSETTEXT, cost.source_line - 1, reinterpret_cast<sptr_t>(text.c_str()));
            left->send(SCI_EOLANNOTATIONSETSTYLE, cost.source_line - 1,
                       cost.percent >= HOT_BADGE_PERCENT_THRESHOLD ? STYLE_BADGE_HOT : STYLE_BADGE_DIM);
        }
    }
}

void disasm_split_view::apply_source_line_badges(const QString& source_path, const QList<disasm_line_badge>& badges) {
    m_pending_badges.clear();
    for (const disasm_line_badge& badge : badges) {
        m_pending_badges.append({ source_path, badge });
    }

    refresh_stored_source_badges();
}

void disasm_split_view::refresh_stored_source_badges() {
    if (m_mapped.primary_source_file.empty()) {
        return;
    }

    ScintillaEditBase* left = m_split->left_view();
    left->send(SCI_EOLANNOTATIONCLEARALL);

    int max_line = static_cast<int>(left->send(SCI_GETLINECOUNT));
    bool any_applied = false;
    for (const auto& [path, badge] : m_pending_badges) {
        if (!source_paths_match(m_mapped.primary_source_file, path.toStdString()) ||
            badge.line < 1 || badge.line > max_line) {
            continue;
        }

        if (!any_applied) {
            left->send(SCI_EOLANNOTATIONSETVISIBLE, EOLANNOTATION_STADIUM);
            any_applied = true;
        }

        QByteArray utf8 = badge.label.toUtf8();
        left->send(SCI_EOLANNOTATIONSETTEXT, badge.line - 1, reinterpret_cast<sptr_t>(utf8.constData()));
        left->send(SCI_EOLANNOTATIONSETSTYLE, badge.line - 1, badge.is_hot ? STYLE_BADGE_HOT : STYLE_BADGE_DIM);
    }
}

} // namespace sheen
