#include "bar.hpp"

#include <stlxtheme/theme.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

constexpr int32_t TARGET_HEIGHT = 24;

/* Padding left and right of the content inside the mark's hover background and the readouts' frame */
constexpr int32_t TARGET_PADDING = 8;

constexpr int32_t SEPARATOR_MARGIN = 12;
constexpr int32_t SEPARATOR_HEIGHT = 16;
constexpr int32_t SEPARATOR_WIDTH = 2 * SEPARATOR_MARGIN + 1;
constexpr int32_t READOUT_GAP = 8;
constexpr int32_t CLOCK_GAP = 8;

constexpr int32_t METER_CELLS = 16;
constexpr int32_t METER_CELL_WIDTH = 2;
constexpr int32_t METER_CELL_HEIGHT = 8;
constexpr int32_t METER_CELL_GAP = 1;
constexpr int32_t METER_WIDTH = METER_CELLS * (METER_CELL_WIDTH + METER_CELL_GAP) - METER_CELL_GAP;

/* The load figure takes at least the width of "99%", so a change of load keeps
 * the readouts' width while the load stays under 100% */
constexpr int32_t CPU_FIGURE_COLUMNS = 3;

constexpr uint64_t MEBIBYTE = 1024ull * 1024;
constexpr uint64_t GIBIBYTE = 1024 * MEBIBYTE;

/* A vertical hairline `line_x` pixels from its frame's left edge, followed by `SEPARATOR_MARGIN` of space */
class bar_separator : public ui::widget {
public:
    explicit bar_separator(int32_t line_x) : m_line_x(line_x) {}

    void set_shown(bool shown) {
        if (shown != m_shown) {
            m_shown = shown;
            invalidate();
        }
    }

    ui::size measure(ui::size) override {
        return { m_line_x + 1 + SEPARATOR_MARGIN, SEPARATOR_HEIGHT };
    }

    void paint(ui::painter& p) override {
        if (m_shown) {
            p.fill({ m_line_x, 0, 1, m_frame.h }, STLXTHEME_LINE);
        }
    }

private:
    int32_t m_line_x;
    bool    m_shown = true;
};

namespace {

/* The bar's glass and the hairline along its bottom */
class bar_background : public ui::box {
public:
    explicit bar_background(const stlxgfx_surface_t* glass) : ui::box(ui::axis::row), m_glass(glass) {}

    void paint(ui::painter& p) override {
        p.image({ 0, 0 }, m_glass);
        p.fill({ 0, m_frame.h - 1, m_frame.w, 1 }, STLXTHEME_LINE_SUBTLE);
    }

private:
    const stlxgfx_surface_t* m_glass;
};

} // namespace

/* The `TARGET_HEIGHT` rows centered in `frame`, `reach_left` in from its left side, counted from its top left */
static ui::rect target_slot(const ui::rect& frame, int32_t reach_left) {
    return { reach_left, (frame.h - TARGET_HEIGHT) / 2, frame.w - reach_left, TARGET_HEIGHT };
}

/* Used memory in whole mebibytes below a gibibyte, else in gibibytes cut to a tenth */
static void format_memory(char* out, size_t cap, uint64_t used_bytes) {
    if (used_bytes < GIBIBYTE) {
        snprintf(out, cap, "%lluM", static_cast<unsigned long long>(used_bytes / MEBIBYTE));
        return;
    }

    uint64_t tenths = used_bytes * 10 / GIBIBYTE;
    snprintf(out, cap, "%llu.%lluG", static_cast<unsigned long long>(tenths / 10),
             static_cast<unsigned long long>(tenths % 10));
}

bar_target::bar_target(int32_t reach_left) : m_reach_left(reach_left) {}

bar_target::~bar_target() {
    stlxgfx_destroy_surface(m_ring);
}

void bar_target::set_open(bool open) {
    if (open != m_open) {
        m_open = open;
        invalidate();
    }
}

void bar_target::paint(ui::painter& p) {
    ui::rect background = slot();
    if (!m_open) {
        if (m_hover) {
            p.rounded_rect(background, STLXTHEME_CONTROL_RADIUS, STLXTHEME_SURFACE_HOVER);
        }

        return;
    }

    bool ring_fits = m_ring && static_cast<int32_t>(m_ring->width) == background.w &&
                     static_cast<int32_t>(m_ring->height) == background.h;
    if (!ring_fits) {
        stlxgfx_destroy_surface(m_ring);
        m_ring = make_ring_picture(background.w, background.h, STLXTHEME_CONTROL_RADIUS, STLXTHEME_LINE_ON);
    }

    p.rounded_rect(background, STLXTHEME_CONTROL_RADIUS, STLXTHEME_SURFACE_ON);
    p.image({ background.x, background.y }, m_ring);
}

void bar_target::on_pointer_enter() {
    m_hover = true;
    invalidate();
}

void bar_target::on_pointer_leave() {
    m_hover = false;
    invalidate();
}

/* A press takes no grab, since a release over the popover goes to its host and the bar's host would stay grabbed */
bool bar_target::on_pointer_down(const ui::pointer_event&) {
    if (on_press) {
        on_press();
    }

    return false;
}

ui::rect bar_target::slot() const {
    return target_slot(m_frame, m_reach_left);
}

mark_target::mark_target() : bar_target(top_bar::TARGET_INSET), m_mark(make_mark_picture(STLXTHEME_TEXT)) {}

mark_target::~mark_target() {
    stlxgfx_destroy_surface(m_mark);
}

ui::size mark_target::measure(ui::size) {
    return { top_bar::TARGET_INSET + STLXTHEME_GLYPH_SIZE + 2 * TARGET_PADDING, STLXTHEME_TOP_BAR_HEIGHT };
}

void mark_target::paint(ui::painter& p) {
    bar_target::paint(p);

    ui::rect content = slot();
    p.image({ content.x + TARGET_PADDING, (m_frame.h - STLXTHEME_GLYPH_SIZE) / 2 }, m_mark);
}

readouts_target::readouts_target()
    : m_cpu_label("CPU", STLXTHEME_UI_FONT_SEMIBOLD_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM),
      m_memory_label("MEM", STLXTHEME_UI_FONT_SEMIBOLD_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM),
      m_network_label("NET", STLXTHEME_UI_FONT_SEMIBOLD_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM),
      m_cpu("", STLXTHEME_MONO_FONT_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT_SECONDARY),
      m_memory("", STLXTHEME_MONO_FONT_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT_SECONDARY),
      m_network("", STLXTHEME_MONO_FONT_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT_SECONDARY) {}

void readouts_target::show_stats(const stats_sampler& stats) {
    char cpu[8];
    snprintf(cpu, sizeof(cpu), "%u%%", stats.load());
    char memory[16];
    format_memory(memory, sizeof(memory), stats.used_pages() * stats.page_size());

    m_cpu_load = stats.load();
    bool cpu_changed = m_cpu.set_text(cpu);
    bool memory_changed = m_memory.set_text(memory);
    if (cpu_changed || memory_changed) {
        invalidate_layout();
    }
}

void readouts_target::show_network(const char* summary) {
    if (m_network.set_text(summary)) {
        invalidate_layout();
    }
}

ui::size readouts_target::measure(ui::size) {
    int32_t cpu = m_cpu_label.width() + READOUT_GAP + METER_WIDTH + READOUT_GAP + cpu_figure_width();
    int32_t memory = m_memory_label.width() + READOUT_GAP + m_memory.width();
    int32_t network = m_network_label.width() + READOUT_GAP + m_network.width();

    return { 2 * TARGET_PADDING + cpu + memory + network + 2 * SEPARATOR_WIDTH, STLXTHEME_TOP_BAR_HEIGHT };
}

void readouts_target::paint(ui::painter& p) {
    int32_t x = TARGET_PADDING;
    m_cpu_label.draw(p, x, 0, m_frame.h);
    x = paint_meter(p, x + m_cpu_label.width() + READOUT_GAP) + READOUT_GAP;

    int32_t cpu_figure = cpu_figure_width();
    m_cpu.draw(p, x + cpu_figure - m_cpu.width(), 0, m_frame.h);

    x = paint_separator(p, x + cpu_figure);
    x = paint_separator(p, paint_readout(p, x, m_memory_label, m_memory));
    paint_readout(p, x, m_network_label, m_network);
}

int32_t readouts_target::cpu_figure_width() const {
    return std::max(m_cpu.width(), CPU_FIGURE_COLUMNS * m_cpu.advance());
}

int32_t readouts_target::paint_readout(ui::painter& p, int32_t x, const text_picture& label,
                                       const text_picture& figure) const {
    label.draw(p, x, 0, m_frame.h);

    int32_t figure_x = x + label.width() + READOUT_GAP;
    figure.draw(p, figure_x, 0, m_frame.h);

    return figure_x + figure.width();
}

int32_t readouts_target::paint_separator(ui::painter& p, int32_t x) const {
    p.fill({ x + SEPARATOR_MARGIN, (m_frame.h - SEPARATOR_HEIGHT) / 2, 1, SEPARATOR_HEIGHT }, STLXTHEME_LINE);
    return x + SEPARATOR_WIDTH;
}

int32_t readouts_target::paint_meter(ui::painter& p, int32_t x) const {
    uint32_t cells = METER_CELLS;
    uint32_t lit = lit_cells(m_cpu_load, STATS_PERCENT, cells);
    int32_t top = (m_frame.h - METER_CELL_HEIGHT) / 2;
    for (uint32_t cell = 0; cell < cells; cell++) {
        int32_t cell_x = x + static_cast<int32_t>(cell) * (METER_CELL_WIDTH + METER_CELL_GAP);
        uint32_t color = cell < lit ? STLXTHEME_TEXT : STLXTHEME_METER_IDLE;
        p.fill({ cell_x, top, METER_CELL_WIDTH, METER_CELL_HEIGHT }, color);
    }

    return x + METER_WIDTH;
}

std::unique_ptr<ui::widget> top_bar::build(const stlxgfx_surface_t* glass) {
    auto root = std::make_unique<bar_background>(glass);
    root->s().align_items = ui::align::center;

    ui::box* left = root->add<ui::box>(ui::axis::row);
    left->s().main = ui::length::flex();
    left->s().align_items = ui::align::center;

    m_mark = left->add<mark_target>();
    m_separator = left->add<bar_separator>(SEPARATOR_MARGIN - TARGET_PADDING);
    m_app_name = left->add<ui::label>("");
    m_app_name->set_weight(ui::weight::semibold);

    ui::box* center = root->add<ui::box>(ui::axis::row);
    center->s().align_items = ui::align::center;
    center->s().gap = CLOCK_GAP;

    m_date = center->add<ui::label>("");
    m_date->set_weight(ui::weight::medium);
    m_date->set_color(STLXTHEME_TEXT_SECONDARY);
    m_time = center->add<ui::label>("");
    m_time->set_weight(ui::weight::medium);

    ui::box* right = root->add<ui::box>(ui::axis::row);
    right->s().main = ui::length::flex();
    right->s().align_items = ui::align::center;
    right->s().justify = ui::align::end;
    right->s().padding = { 0, TARGET_INSET, 0, 0 };
    m_readouts = right->add<readouts_target>();

    show_clock();
    show_app_name("");

    return root;
}

void top_bar::show_app_name(const std::string& name) {
    m_separator->set_shown(!name.empty());
    m_app_name->set_text(name);
}

void top_bar::show_clock() {
    time_t now = time(nullptr);
    struct tm* parts = gmtime(&now);
    if (!parts) {
        return;
    }

    char weekday_month[16];
    strftime(weekday_month, sizeof(weekday_month), "%a %b", parts);
    char date[24];
    snprintf(date, sizeof(date), "%s %d", weekday_month, parts->tm_mday);
    char time_of_day[8];
    strftime(time_of_day, sizeof(time_of_day), "%H:%M", parts);

    m_date->set_text(date);
    m_time->set_text(time_of_day);
}
