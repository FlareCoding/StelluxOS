#ifndef SHEEN_EXTENSIONS_PERF_BINARIES_DISASM_SPLIT_VIEW_H
#define SHEEN_EXTENSIONS_PERF_BINARIES_DISASM_SPLIT_VIEW_H

#include "extensions/perf_binaries/disasm_mapper.h"
#include "extensions/perf_binaries/elf_model.h"
#include "workspace/process_task.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QWidget>

#include <span>

class QLabel;

namespace sheen::ui {
class ui_split_code_view;
} // namespace sheen::ui

namespace sheen {

struct disasm_line_badge {
    int line = 1;
    QString label;
    bool is_hot = false;
};

/* Synchronized C++ source and `objdump -d -l -C` disassembly split pane. */
class disasm_split_view : public QWidget {
public:
    disasm_split_view(const QString& binary_path, const QString& workspace_root,
                      uint16_t elf_machine, uint16_t dwarf_version,
                      const elf_symbol& target_symbol, QWidget* parent = nullptr);
    ~disasm_split_view() override;

    const QString& binary_path() const { return m_binary_path; }
    const std::string& symbol_raw_name() const { return m_symbol.raw_name; }

    void stop_disassembly();
    void apply_address_samples(std::span<const address_sample> samples, uint64_t denominator_override = 0);
    void apply_source_line_badges(const QString& source_path, const QList<disasm_line_badge>& badges);

private:
    void start_objdump();
    void finish_objdump(const process_exit& exit_result);
    QString resolve_source_path(std::string_view recorded_path) const;
    void load_source_pane(const QString& resolved_path);
    void refresh_stored_source_badges();

    QString m_binary_path;
    QString m_workspace_root;
    uint16_t m_elf_machine = 0;
    uint16_t m_dwarf_version = 0;
    elf_symbol m_symbol;

    QLabel* m_status_label = nullptr;
    ui::ui_split_code_view* m_split = nullptr;
    process_task m_task;
    QByteArray m_stdout_buffer;
    QByteArray m_stderr_buffer;
    disasm_map m_mapped;
    QList<std::pair<QString, disasm_line_badge>> m_pending_badges;
};

QString infer_project_root_for_binary(const QString& binary_path, const QString& workspace_root = {});

} // namespace sheen

#endif // SHEEN_EXTENSIONS_PERF_BINARIES_DISASM_SPLIT_VIEW_H
