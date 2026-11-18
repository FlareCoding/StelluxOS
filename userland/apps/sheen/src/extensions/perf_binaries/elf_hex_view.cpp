#include "extensions/perf_binaries/elf_hex_view.h"
#include "ui/chrome.h"
#include "ui/ui_kit.h"

#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

namespace sheen {

static constexpr int SUMMARY_BAR_HEIGHT = 32, TOOLBAR_ROW_HEIGHT = 34, FOOTER_BAR_HEIGHT = 28;
static constexpr int BAR_PADDING_X = 12, TOOLBAR_GAP = 6, HEX_PADDING_X = 12, HEX_PADDING_Y = 8;
static constexpr int HEX_ROW_HEIGHT = 20, HEX_SELECTION_ALPHA = 38, WHEEL_SCROLL_ROWS = 3;
static constexpr int HEX_FIRST_COL_CHARS = 10, HEX_MID_GAP_COL_CHARS = 24, HEX_CELL_STEP_CHARS = 3;
static constexpr qreal HEX_SELECTION_RADIUS = 3.0, HALF_PX = 0.5;
static constexpr uint64_t ONE_KIB = 1024ull, ONE_MIB = 1024ull * 1024ull;
static constexpr int MODE_SECTIONS = 0, MODE_SEGMENTS = 1, MODE_SYMBOLS = 2;

namespace {

struct quick_ehdr_chip {
    const char* button_label;
    const char* field_name;
};

class hairline_divider final : public QWidget {
public:
    explicit hairline_divider(QWidget* parent) : QWidget(parent) {
        setFixedWidth(1);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), token(STLXTHEME_LINE));
    }
};

} // namespace

static constexpr quick_ehdr_chip QUICK_EHDR_CHIPS[] = {
    { "e_ident", "Ehdr.e_ident" },
    { "e_type", "Ehdr.e_type" },
    { "e_machine", "Ehdr.e_machine" },
    { "e_entry", "Ehdr.e_entry" },
    { "e_phoff", "Ehdr.e_phoff" },
    { "e_shoff", "Ehdr.e_shoff" },
};

class hex_byte_canvas final : public QWidget {
public:
    explicit hex_byte_canvas(QWidget* parent = nullptr)
        : QWidget(parent), m_scroll(new QScrollBar(Qt::Vertical, this)) {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addStretch(1);
        layout->addWidget(m_scroll);

        connect(m_scroll, &QScrollBar::valueChanged, this, [this] { update(); });
    }

    void set_bytes(std::span<const uint8_t> bytes) {
        m_bytes = bytes;
        update_scrollbar_range();
        update();
    }

    void set_selection(byte_range selection, bool scroll_into_view) {
        m_selection = selection;
        if (scroll_into_view && selection.start < m_bytes.size()) {
            int target_row = static_cast<int>(selection.start / HEX_BYTES_PER_ROW);
            int visible = visible_row_count();
            int first = m_scroll->value();
            if (target_row < first || target_row >= first + std::max(1, visible - 1)) {
                m_scroll->setValue(std::max(0, target_row - 1));
            }
        }

        update();
    }

    std::function<void(byte_range range)> on_bytes_selected;

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        update_scrollbar_range();
    }

    void wheelEvent(QWheelEvent* event) override {
        int delta_y = event->angleDelta().y();
        int step = delta_y > 0 ? -WHEEL_SCROLL_ROWS : (delta_y < 0 ? WHEEL_SCROLL_ROWS : 0);
        if (step != 0) {
            m_scroll->setValue(m_scroll->value() + step);
            event->accept();
            return;
        }

        QWidget::wheelEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        uint64_t hit = 0;
        if (event->button() == Qt::LeftButton && byte_offset_at(event->pos(), hit)) {
            m_drag_anchor = hit;
            m_is_dragging = true;
            commit_drag_selection(hit);
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        uint64_t hit = 0;
        if (m_is_dragging && byte_offset_at(event->pos(), hit)) {
            commit_drag_selection(hit);
        }
    }

    void mouseReleaseEvent(QMouseEvent*) override { m_is_dragging = false; }
    void paintEvent(QPaintEvent*) override;

private:
    void commit_drag_selection(uint64_t hit) {
        byte_range selection{ std::min(m_drag_anchor, hit), std::max(m_drag_anchor, hit) + 1 };
        set_selection(selection, false);
        if (on_bytes_selected) {
            on_bytes_selected(selection);
        }
    }

    int visible_row_count() const { return std::max(1, (height() - 2 * HEX_PADDING_Y) / HEX_ROW_HEIGHT); }

    void update_scrollbar_range() {
        int total_rows = static_cast<int>((m_bytes.size() + HEX_BYTES_PER_ROW - 1) / HEX_BYTES_PER_ROW);
        int page = visible_row_count();
        m_scroll->setPageStep(page);
        m_scroll->setRange(0, std::max(0, total_rows - page));
    }

    bool byte_offset_at(const QPoint& position, uint64_t& out_offset) const;

    std::span<const uint8_t> m_bytes;
    byte_range m_selection;
    QScrollBar* m_scroll = nullptr;
    uint64_t m_drag_anchor = 0;
    bool m_is_dragging = false;
};

static QString hex_u64(uint64_t value, int width_digits = 8) {
    return QStringLiteral("0x%1").arg(value, width_digits, 16, QLatin1Char('0'));
}

perf_strip_bar::perf_strip_bar(int fixed_height, bool has_top_border, bool has_bottom_border, QWidget* parent)
    : QWidget(parent), m_has_top_border(has_top_border), m_has_bottom_border(has_bottom_border) {
    setFixedHeight(fixed_height);
}

void perf_strip_bar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), token(STLXTHEME_SHADE));
    if (m_has_top_border) {
        painter.fillRect(QRect(0, 0, width(), 1), token(STLXTHEME_LINE));
    }

    if (m_has_bottom_border) {
        painter.fillRect(QRect(0, height() - 1, width(), 1), token(STLXTHEME_LINE));
    }
}

bool hex_byte_canvas::byte_offset_at(const QPoint& position, uint64_t& out_offset) const {
    if (m_bytes.empty() || position.y() < HEX_PADDING_Y) {
        return false;
    }

    int row_in_view = (position.y() - HEX_PADDING_Y) / HEX_ROW_HEIGHT;
    uint64_t row_offset = static_cast<uint64_t>(m_scroll->value() + row_in_view) * HEX_BYTES_PER_ROW;
    if (row_offset >= m_bytes.size()) {
        return false;
    }

    QFontMetricsF metrics(mono_font(STLXTHEME_CAPTION_SIZE));
    qreal char_width = metrics.horizontalAdvance(QLatin1Char('0'));
    if (char_width <= 0.0) {
        return false;
    }

    int column = static_cast<int>((position.x() - HEX_PADDING_X) / char_width);
    if (column < HEX_FIRST_COL_CHARS) {
        return false;
    }

    int relative_col = column - HEX_FIRST_COL_CHARS;
    if (relative_col >= HEX_MID_GAP_COL_CHARS) {
        relative_col -= 1;
    }

    int byte_index = relative_col / HEX_CELL_STEP_CHARS;
    if (byte_index < 0 || byte_index >= static_cast<int>(HEX_BYTES_PER_ROW)) {
        return false;
    }

    uint64_t candidate = row_offset + static_cast<uint64_t>(byte_index);
    if (candidate >= m_bytes.size()) {
        return false;
    }

    out_offset = candidate;
    return true;
}

void hex_byte_canvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), token(STLXTHEME_OPAQUE_POCKET));

    if (m_bytes.empty()) {
        painter.setFont(ui_font(STLXTHEME_BODY_SIZE));
        painter.setPen(token(STLXTHEME_TEXT_DIM));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No binary bytes loaded."));
        return;
    }

    QFont font = mono_font(STLXTHEME_CAPTION_SIZE);
    painter.setFont(font);
    QFontMetricsF metrics(font);
    qreal char_width = metrics.horizontalAdvance(QLatin1Char('0'));

    uint64_t base_offset = static_cast<uint64_t>(m_scroll->value()) * HEX_BYTES_PER_ROW;
    size_t rows_to_draw = static_cast<size_t>(visible_row_count() + 1);
    hex_ascii_window window = format_hex_ascii_window(m_bytes, base_offset, rows_to_draw, m_selection);

    QColor accent = token(STLXTHEME_ACCENT);
    QColor accent_fill = accent;
    accent_fill.setAlpha(HEX_SELECTION_ALPHA);

    for (size_t idx = 0; idx < window.rows.size(); idx++) {
        const hex_ascii_row& row = window.rows[idx];
        int row_y = HEX_PADDING_Y + static_cast<int>(idx) * HEX_ROW_HEIGHT;
        if (row.has_selection) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(accent, 1.0));
            painter.setBrush(accent_fill);

            QRectF hex_box(HEX_PADDING_X + static_cast<qreal>(row.hex_column_start) * char_width - 2.0,
                           row_y + 1.0 + HALF_PX,
                           static_cast<qreal>(row.hex_column_end - row.hex_column_start) * char_width + 4.0,
                           HEX_ROW_HEIGHT - 2.0);
            QRectF ascii_box(HEX_PADDING_X + static_cast<qreal>(row.ascii_column_start) * char_width - 1.0,
                             row_y + 1.0 + HALF_PX,
                             static_cast<qreal>(row.ascii_column_end - row.ascii_column_start) * char_width + 2.0,
                             HEX_ROW_HEIGHT - 2.0);
            painter.drawRoundedRect(hex_box, HEX_SELECTION_RADIUS, HEX_SELECTION_RADIUS);
            painter.drawRoundedRect(ascii_box, HEX_SELECTION_RADIUS, HEX_SELECTION_RADIUS);
            painter.setRenderHint(QPainter::Antialiasing, false);
        }

        QString full_line = QString::fromLatin1(row.line.data(), static_cast<qsizetype>(row.line.size()));
        int space_split = std::max(0, static_cast<int>(full_line.indexOf(QLatin1String("  "))));
        painter.setPen(token(STLXTHEME_TEXT_DIM));
        painter.drawText(QRectF(HEX_PADDING_X, row_y, space_split * char_width, HEX_ROW_HEIGHT),
                         Qt::AlignLeft | Qt::AlignVCenter, full_line.left(space_split));
        painter.setPen(token(STLXTHEME_TERMINAL_TEXT));
        painter.drawText(QRectF(HEX_PADDING_X + space_split * char_width, row_y,
                                width() - HEX_PADDING_X, HEX_ROW_HEIGHT),
                         Qt::AlignLeft | Qt::AlignVCenter, full_line.mid(space_split));
    }
}

QString format_byte_count(uint64_t bytes) {
    if (bytes >= ONE_MIB) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / static_cast<double>(ONE_MIB), 0, 'f', 1);
    }

    if (bytes >= ONE_KIB) {
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / static_cast<double>(ONE_KIB), 0, 'f', 1);
    }

    return QStringLiteral("%1 B").arg(bytes);
}

static QLabel* make_strip_label(perf_strip_bar* bar, uint32_t color_token) {
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(BAR_PADDING_X, 0, BAR_PADDING_X, 1);

    auto* label = new QLabel(bar);
    label->setFont(mono_font(STLXTHEME_CAPTION_SIZE));
    label->setStyleSheet(QStringLiteral("color: %1; background: transparent;")
                             .arg(token(color_token).name(QColor::HexArgb)));
    layout->addWidget(label, 1);
    return label;
}

elf_hex_view::elf_hex_view(const QString& binary_path, QWidget* parent)
    : QWidget(parent), m_binary_path(binary_path) {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* summary_bar = new perf_strip_bar(SUMMARY_BAR_HEIGHT, false, true, this);
    m_summary_label = make_strip_label(summary_bar, STLXTHEME_TEXT_SECONDARY);
    root_layout->addWidget(summary_bar);

    auto* toolbar = new perf_strip_bar(TOOLBAR_ROW_HEIGHT, false, true, this);
    auto* bar_layout = new QHBoxLayout(toolbar);
    bar_layout->setContentsMargins(BAR_PADDING_X, 0, BAR_PADDING_X, 1);
    bar_layout->setSpacing(TOOLBAR_GAP);

    m_sections_button = new ui::ui_pill_button(QStringLiteral("Sections"), toolbar);
    m_segments_button = new ui::ui_pill_button(QStringLiteral("Segments"), toolbar);
    m_symbols_button = new ui::ui_pill_button(QStringLiteral("Symbols"), toolbar);
    m_shdr_mode_button = new ui::ui_pill_button(QStringLiteral("Shdr Bytes"), toolbar);

    connect(m_sections_button, &QAbstractButton::clicked, this, [this] { switch_table_mode(MODE_SECTIONS); });
    connect(m_segments_button, &QAbstractButton::clicked, this, [this] { switch_table_mode(MODE_SEGMENTS); });
    connect(m_symbols_button, &QAbstractButton::clicked, this, [this] { switch_table_mode(MODE_SYMBOLS); });
    connect(m_shdr_mode_button, &QAbstractButton::clicked, this, [this] {
        m_inspect_section_headers = !m_inspect_section_headers;
        m_shdr_mode_button->set_checked_dot(m_inspect_section_headers);
    });

    bar_layout->addWidget(m_sections_button);
    bar_layout->addWidget(m_segments_button);
    bar_layout->addWidget(m_symbols_button);
    bar_layout->addWidget(m_shdr_mode_button);
    bar_layout->addStretch(1);

    for (const quick_ehdr_chip& chip : QUICK_EHDR_CHIPS) {
        auto* button = new ui::ui_pill_button(QString::fromLatin1(chip.button_label), toolbar);
        QString field_name = QString::fromLatin1(chip.field_name);
        byte_range range = elf_header_field_range(chip.field_name);
        connect(button, &QAbstractButton::clicked, this, [this, range, field_name] {
            select_byte_range(range, field_name);
        });
        bar_layout->addWidget(button);
    }

    root_layout->addWidget(toolbar);

    auto* split_body = new QWidget(this);
    auto* split_layout = new QHBoxLayout(split_body);
    split_layout->setContentsMargins(0, 0, 0, 0);
    split_layout->setSpacing(0);

    m_table_stack = new QStackedWidget(split_body);
    m_sections_table = new ui::ui_table_view({
        { QStringLiteral("Section"), 124, false }, { QStringLiteral("Type"), 90, false },
        { QStringLiteral("VMA"), 104, false },     { QStringLiteral("Offset"), 92, false },
        { QStringLiteral("Size"), 82, true },      { QStringLiteral("Share"), 66, true },
        { QStringLiteral("Flags"), 0, false },
    }, m_table_stack);
    m_segments_table = new ui::ui_table_view({
        { QStringLiteral("Segment"), 90, false },  { QStringLiteral("VAddr"), 112, false },
        { QStringLiteral("Offset"), 96, false },   { QStringLiteral("FileSz"), 88, true },
        { QStringLiteral("MemSz"), 88, true },     { QStringLiteral("Flags"), 0, false },
    }, m_table_stack);
    m_symbols_table = new ui::ui_table_view({
        { QStringLiteral("Symbol"), 0, false },    { QStringLiteral("Section"), 92, false },
        { QStringLiteral("VMA"), 108, false },     { QStringLiteral("Size"), 78, true },
        { QStringLiteral("Bind"), 70, false },
    }, m_table_stack);

    m_table_stack->addWidget(m_sections_table);
    m_table_stack->addWidget(m_segments_table);
    m_table_stack->addWidget(m_symbols_table);

    m_sections_table->on_row_activated = [this](const QString& id) { handle_section_row(id); };
    m_segments_table->on_row_activated = [this](const QString& id) { handle_segment_row(id); };
    m_symbols_table->on_row_activated = [this](const QString& id) { activate_symbol_row(id); };
    m_sections_table->on_row_clicked = [this](const QString& id) { handle_section_row(id); };
    m_segments_table->on_row_clicked = [this](const QString& id) { handle_segment_row(id); };
    m_symbols_table->on_row_clicked = [this](const QString& id) { preview_symbol_row(id); };

    auto* right_box = new QWidget(split_body);
    auto* right_layout = new QVBoxLayout(right_box);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(0);

    m_hex_canvas = new hex_byte_canvas(right_box);
    m_hex_canvas->on_bytes_selected = [this](byte_range selection) {
        update_selection_readout(selection, QStringLiteral("Byte selection"));
    };

    right_layout->addWidget(m_hex_canvas, 1);

    auto* footer = new perf_strip_bar(FOOTER_BAR_HEIGHT, true, false, right_box);
    m_selection_label = make_strip_label(footer, STLXTHEME_TEXT);
    right_layout->addWidget(footer);

    split_layout->addWidget(m_table_stack, 1);
    split_layout->addWidget(new hairline_divider(split_body));
    split_layout->addWidget(right_box, 1);
    root_layout->addWidget(split_body, 1);
    reload_binary();
}

void elf_hex_view::reload_binary() {
    m_bytes.clear();
    m_model = {};
    m_is_valid = false;
    m_sections_table->set_rows({});
    m_segments_table->set_rows({});
    m_symbols_table->set_rows({});

    QFileInfo file_info(m_binary_path);
    if (!file_info.isFile() || file_info.size() > MAX_ELF_FILE_BYTES) {
        m_hex_canvas->set_bytes({});
        m_summary_label->setText(QStringLiteral("%1 exceeds maximum inspectable ELF size (64 MB)")
                                     .arg(file_info.fileName()));
        switch_table_mode(MODE_SECTIONS);
        return;
    }

    QFile file(m_binary_path);
    if (file.open(QIODevice::ReadOnly)) {
        m_bytes = file.read(MAX_ELF_FILE_BYTES);
    }

    std::span<const uint8_t> raw_span(reinterpret_cast<const uint8_t*>(m_bytes.constData()),
                                      static_cast<size_t>(m_bytes.size()));
    m_hex_canvas->set_bytes(raw_span);

    std::string parse_error;
    m_is_valid = parse_elf_binary(raw_span, m_model, parse_error);
    if (!m_is_valid) {
        m_summary_label->setText(QStringLiteral("%1: %2")
                                     .arg(file_info.fileName(), QString::fromStdString(parse_error)));
        switch_table_mode(MODE_SECTIONS);
        return;
    }

    QString dwarf_note = !m_model.header.has_dwarf
                             ? QStringLiteral("No DWARF")
                             : (m_model.header.dwarf_version > 0
                                    ? QStringLiteral("DWARF v%1").arg(m_model.header.dwarf_version)
                                    : QStringLiteral("DWARF"));
    QString linkage = m_model.header.is_static ? QStringLiteral("Static") : QStringLiteral("Dynamic");
    m_summary_label->setText(
        QStringLiteral("ELF64 LSB   %1 (%2)   Entry %3   %4   %5   Phdr %6   Shdr %7")
            .arg(QString::fromStdString(m_model.header.type_name),
                 QString::fromStdString(m_model.header.machine_name),
                 hex_u64(m_model.header.entry), linkage, dwarf_note)
            .arg(m_model.header.phnum)
            .arg(m_model.header.shnum));

    populate_tables();
    switch_table_mode(MODE_SECTIONS);
    select_byte_range(elf_header_field_range("Ehdr.e_entry"), QStringLiteral("Ehdr.e_entry"));
}

void elf_hex_view::populate_tables() {
    QList<ui::table_row> section_rows = {
        { QStringLiteral("ehdr"),
          { QStringLiteral("Ehdr"), QStringLiteral("EHDR"), hex_u64(0), hex_u64(0),
            format_byte_count(m_model.header.header_byte_range.end), QStringLiteral("-"), QString() } },
    };

    for (const elf_section& section : m_model.sections) {
        if (section.index == 0 && section.name.empty()) {
            continue;
        }

        QString share_text = section.is_nobits ? QStringLiteral("-")
                                               : QStringLiteral("%1%").arg(section.share * 100.0, 0, 'f', 1);
        QString sec_name = section.name.empty() ? QStringLiteral("<unnamed>") : QString::fromStdString(section.name);
        section_rows.append({ QString::number(section.index),
                              { sec_name, QString::fromStdString(section.type_name), hex_u64(section.vma),
                                hex_u64(section.file_offset), format_byte_count(section.size),
                                share_text, QString::fromStdString(section.flags_label) } });
    }

    m_sections_table->set_rows(section_rows);

    QList<ui::table_row> segment_rows;
    for (size_t idx = 0; idx < m_model.load_segments.size(); idx++) {
        const elf_segment& segment = m_model.load_segments[idx];
        segment_rows.append({ QString::number(idx),
                              { QStringLiteral("PT_LOAD %1").arg(segment.index), hex_u64(segment.vaddr),
                                hex_u64(segment.file_offset), format_byte_count(segment.file_size),
                                format_byte_count(segment.mem_size), QString::fromStdString(segment.flags_label) } });
    }

    m_segments_table->set_rows(segment_rows);

    QList<ui::table_row> symbol_rows;
    for (size_t idx = 0; idx < m_model.symbols.size(); idx++) {
        const elf_symbol& symbol = m_model.symbols[idx];
        symbol_rows.append({ QString::number(idx),
                             { QString::fromStdString(symbol.demangled_name),
                               QString::fromStdString(symbol.section_name), hex_u64(symbol.vma),
                               format_byte_count(symbol.size), QString::fromStdString(symbol.binding_name) } });
    }

    m_symbols_table->set_rows(symbol_rows);
}

void elf_hex_view::switch_table_mode(int mode_index) {
    m_table_stack->setCurrentIndex(mode_index);
    m_sections_button->set_checked_dot(mode_index == MODE_SECTIONS);
    m_segments_button->set_checked_dot(mode_index == MODE_SEGMENTS);
    m_symbols_button->set_checked_dot(mode_index == MODE_SYMBOLS);
}

void elf_hex_view::select_byte_range(byte_range range, const QString& label) {
    m_hex_canvas->set_selection(range, true);
    update_selection_readout(range, label);
}

void elf_hex_view::handle_section_row(const QString& row_id) {
    if (row_id == QLatin1String("ehdr")) {
        select_byte_range(m_model.header.header_byte_range, QStringLiteral("Elf64_Ehdr"));
        return;
    }

    bool ok = false;
    uint32_t section_index = row_id.toUInt(&ok);
    if (!ok || section_index >= m_model.sections.size()) {
        return;
    }

    const elf_section& section = m_model.sections[section_index];
    bool use_shdr = m_inspect_section_headers || section.is_nobits ||
                    section.content_byte_range.end <= section.content_byte_range.start;
    byte_range target = use_shdr ? section.header_byte_range : section.content_byte_range;
    QString prefix = use_shdr ? QStringLiteral("Shdr[%1] ").arg(section.index) : QString();
    select_byte_range(target, prefix + QString::fromStdString(section.name));
}

void elf_hex_view::handle_segment_row(const QString& row_id) {
    bool ok = false;
    size_t idx = row_id.toULongLong(&ok);
    if (!ok || idx >= m_model.load_segments.size()) {
        return;
    }

    const elf_segment& segment = m_model.load_segments[idx];
    byte_range target = m_inspect_section_headers || segment.file_byte_range.end <= segment.file_byte_range.start
                            ? segment.header_byte_range
                            : segment.file_byte_range;
    select_byte_range(target, QStringLiteral("PT_LOAD[%1]").arg(segment.index));
}

const elf_symbol* elf_hex_view::symbol_for_row(const QString& row_id) const {
    bool ok = false;
    size_t idx = row_id.toULongLong(&ok);
    return (ok && idx < m_model.symbols.size()) ? &m_model.symbols[idx] : nullptr;
}

void elf_hex_view::preview_symbol_row(const QString& row_id) {
    const elf_symbol* symbol = symbol_for_row(row_id);
    byte_range range;
    if (symbol && symbol_file_byte_range(m_model, *symbol, range)) {
        select_byte_range(range, QString::fromStdString(symbol->demangled_name));
    }
}

void elf_hex_view::activate_symbol_row(const QString& row_id) {
    preview_symbol_row(row_id);
    const elf_symbol* symbol = symbol_for_row(row_id);
    if (symbol && on_symbol_activated &&
        (symbol->symbol_type == ELF_STT_FUNC || symbol->section_name.starts_with(".text"))) {
        on_symbol_activated(*symbol);
    }
}

void elf_hex_view::update_selection_readout(byte_range range, const QString& label) {
    if (range.end <= range.start || range.start >= static_cast<uint64_t>(m_bytes.size())) {
        m_selection_label->setText(label);
        return;
    }

    uint64_t clamped_end = std::min<uint64_t>(range.end, static_cast<uint64_t>(m_bytes.size()));
    uint64_t length = clamped_end - range.start;
    QString detail = QStringLiteral("%1 @ %2 (%3 bytes)").arg(label, hex_u64(range.start, 4)).arg(length);

    if (length == 2 || length == 4 || length == 8) {
        uint64_t little_endian_value = 0;
        for (uint64_t i = 0; i < length; i++) {
            uint8_t byte_value = static_cast<uint8_t>(m_bytes.at(static_cast<qsizetype>(range.start + i)));
            little_endian_value |= static_cast<uint64_t>(byte_value) << (i * 8);
        }

        detail += QStringLiteral("   uint%1 LE: %2")
                      .arg(length * 8)
                      .arg(hex_u64(little_endian_value, static_cast<int>(length * 2)));
    }

    m_selection_label->setText(detail);
}

} // namespace sheen
