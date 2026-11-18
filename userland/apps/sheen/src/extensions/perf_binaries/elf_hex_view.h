#ifndef SHEEN_EXTENSIONS_PERF_BINARIES_ELF_HEX_VIEW_H
#define SHEEN_EXTENSIONS_PERF_BINARIES_ELF_HEX_VIEW_H

#include "extensions/perf_binaries/elf_model.h"

#include <QByteArray>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>

class QLabel;
class QStackedWidget;

namespace sheen::ui {
class ui_pill_button;
class ui_table_view;
} // namespace sheen::ui

namespace sheen {

inline constexpr qint64 MAX_ELF_FILE_BYTES = 64ll * 1024ll * 1024ll;

class hex_byte_canvas;

/* Shared shaded toolbar or footer strip with optional top and bottom 1 px brass hairlines. */
class perf_strip_bar : public QWidget {
public:
    perf_strip_bar(int fixed_height, bool has_top_border, bool has_bottom_border, QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    bool m_has_top_border;
    bool m_has_bottom_border;
};

/* Side-by-side ELF64 header/section/segment/symbol table and synced Hex & ASCII inspector. */
class elf_hex_view : public QWidget {
public:
    explicit elf_hex_view(const QString& binary_path, QWidget* parent = nullptr);

    bool is_valid() const { return m_is_valid; }
    const QString& binary_path() const { return m_binary_path; }
    const elf_binary_model& model() const { return m_model; }

    void reload_binary();
    void select_byte_range(byte_range range, const QString& label);

    std::function<void(const elf_symbol& symbol)> on_symbol_activated;

private:
    void populate_tables();
    void switch_table_mode(int mode_index);
    void handle_section_row(const QString& row_id);
    void handle_segment_row(const QString& row_id);
    const elf_symbol* symbol_for_row(const QString& row_id) const;
    void preview_symbol_row(const QString& row_id);
    void activate_symbol_row(const QString& row_id);
    void update_selection_readout(byte_range range, const QString& label);

    QString m_binary_path;
    QByteArray m_bytes;
    elf_binary_model m_model;
    bool m_is_valid = false;
    bool m_inspect_section_headers = false;

    QLabel* m_summary_label = nullptr;
    QLabel* m_selection_label = nullptr;
    QStackedWidget* m_table_stack = nullptr;
    ui::ui_table_view* m_sections_table = nullptr;
    ui::ui_table_view* m_segments_table = nullptr;
    ui::ui_table_view* m_symbols_table = nullptr;
    ui::ui_pill_button* m_sections_button = nullptr;
    ui::ui_pill_button* m_segments_button = nullptr;
    ui::ui_pill_button* m_symbols_button = nullptr;
    ui::ui_pill_button* m_shdr_mode_button = nullptr;
    hex_byte_canvas* m_hex_canvas = nullptr;
};

QString format_byte_count(uint64_t bytes);

} // namespace sheen

#endif // SHEEN_EXTENSIONS_PERF_BINARIES_ELF_HEX_VIEW_H
