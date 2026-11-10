#include "stats_panel.hpp"

#include "bar.hpp"
#include "draw.hpp"
#include "programs.hpp"

#include <stlxtheme/theme.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

constexpr int32_t PANEL_WIDTH = 344;

/* The panel's right edge lines up with the readouts' hover background */
constexpr int32_t PANEL_RIGHT_INSET = top_bar::TARGET_INSET;

constexpr int32_t HEADER_HEIGHT = 40;
constexpr int32_t HEADER_PADDING_LEFT = 14;
constexpr int32_t HEADER_PADDING_RIGHT = 8;
constexpr int32_t PIN_SIZE = 26;
constexpr int32_t PIN_GLYPH_WIDTH = 10;
constexpr int32_t PIN_GLYPH_HEIGHT = 14;
constexpr int32_t PIN_HEAD_INSET = 2;
constexpr int32_t PIN_HEAD_HEIGHT = 6;
constexpr int32_t PIN_COLLAR_HEIGHT = 2;
constexpr int32_t PIN_NEEDLE_WIDTH = 2;

constexpr int32_t SECTION_PADDING_X = 14;
constexpr int32_t SECTION_PADDING_Y = 12;
constexpr int32_t SECTION_GAP = 10;
constexpr int32_t HEADING_HEIGHT = 16;
constexpr int32_t HEADING_GAP = 8;

constexpr uint32_t CPU_CELLS = 16;
constexpr int32_t CPU_CELL_HEIGHT = 2;
constexpr int32_t CPU_CELL_PITCH = 3;
constexpr int32_t CPU_COLUMN_GAP = 4;
constexpr int32_t CPU_LABEL_GAP = 4;
constexpr int32_t CPU_LABEL_HEIGHT = 12;
constexpr uint32_t CPUS_PER_ROW = 16;
constexpr int32_t CPU_ROW_GAP = 8;
constexpr int32_t CPU_COLUMN_HEIGHT = static_cast<int32_t>(CPU_CELLS) * CPU_CELL_PITCH - 1;
constexpr int32_t CPU_ROW_HEIGHT = CPU_COLUMN_HEIGHT + CPU_LABEL_GAP + CPU_LABEL_HEIGHT;

constexpr int32_t HISTORY_HEIGHT = 30;
constexpr int32_t HISTORY_LABEL_TOP = 2;
constexpr int32_t HISTORY_LINE_INSET = 2;

constexpr uint32_t MEMORY_CELLS = 124;
constexpr int32_t MEMORY_METER_HEIGHT = 8;
constexpr int32_t MEMORY_CELL_GAP = 1;
constexpr int32_t MEMORY_CAPTION_GAP = 6;
constexpr int32_t CAPTION_HEIGHT = 16;

constexpr int32_t TILE_COUNT = 3;
constexpr int32_t TILE_PADDING_X = 14;
constexpr int32_t TILE_PADDING_Y = 10;
constexpr int32_t TILE_FIGURE_HEIGHT = 20;
constexpr int32_t TILES_HEIGHT = 2 * TILE_PADDING_Y + 2 * CAPTION_HEIGHT + TILE_FIGURE_HEIGHT;

/* The tiles' shares of the width, the network tile's largest since an address is the longest figure */
constexpr int32_t TILE_SHARES[TILE_COUNT] = { 4, 4, 5 };
constexpr int32_t TILE_SHARE_TOTAL = TILE_SHARES[0] + TILE_SHARES[1] + TILE_SHARES[2];

constexpr uint32_t PROCESS_ROWS = 4;
constexpr int32_t PROCESS_HEADER_HEIGHT = 20;
constexpr int32_t PROCESS_ROW_HEIGHT = 24;
constexpr int32_t PROCESS_COLUMN_GAP = 10;
constexpr int32_t PID_COLUMN_WIDTH = 44;
constexpr int32_t LOAD_COLUMN_WIDTH = 52;
constexpr int32_t TIME_COLUMN_WIDTH = 70;

constexpr int32_t LINK_PADDING_X = 6;
constexpr int32_t LINK_PADDING_Y = 6;
constexpr int32_t LINK_GAP = 6;
constexpr int32_t FOOTER_PADDING_Y = 6;
constexpr int32_t FOOTER_PADDING_X = 8;

constexpr uint64_t NS_PER_SECOND = 1000000000ull;
constexpr uint64_t SECONDS_PER_MINUTE = 60;
constexpr uint64_t KIBIBYTE = 1024;
constexpr uint64_t GIBIBYTE = KIBIBYTE * KIBIBYTE * KIBIBYTE;
constexpr uint64_t HUNDREDTHS = 100;
constexpr uint32_t DIGIT_GROUP = 1000;
constexpr uint32_t TENTHS = 10;

namespace {

/* A padded column of the panel's rows, with a hairline along its top when `has_hairline` is set */
class panel_section : public ui::box {
public:
    explicit panel_section(bool has_hairline) : ui::box(ui::axis::column), m_has_hairline(has_hairline) {
        m_style.padding = ui::edge_insets::xy(SECTION_PADDING_X, SECTION_PADDING_Y);
        m_style.gap = SECTION_GAP;
    }

    void paint(ui::painter& p) override {
        if (m_has_hairline) {
            p.fill({ 0, 0, m_frame.w, 1 }, STLXTHEME_LINE_SUBTLE);
        }
    }

private:
    bool m_has_hairline;
};

/* A section's first line: a caps name, a caption after it, and a figure with a dim suffix at the right */
class heading_row : public ui::widget {
public:
    heading_row(const char* name, const char* caption);

    ui::size measure(ui::size avail) override {
        return { avail.w, HEADING_HEIGHT };
    }

    void paint(ui::painter& p) override;

protected:
    text_picture m_name;
    text_picture m_caption;
    text_picture m_figure;
    text_picture m_suffix;
};

/* The CPU count, the busiest CPU and the overall load */
class cpu_heading : public heading_row {
public:
    explicit cpu_heading(const stats_sampler& stats) : heading_row("CPU", ""), m_stats(stats) {}

    void paint(ui::painter& p) override;

private:
    const stats_sampler& m_stats;
};

/* The share of memory in use, and the gigabytes used of the total */
class memory_heading : public heading_row {
public:
    explicit memory_heading(const stats_sampler& stats) : heading_row("MEMORY", ""), m_stats(stats) {}

    void paint(ui::painter& p) override;

private:
    const stats_sampler& m_stats;
};

/* A column of cells for each CPU, lit in proportion to its load, over the CPU's index */
class cpu_columns : public ui::widget {
public:
    explicit cpu_columns(const stats_sampler& stats) : m_stats(stats) {}

    ui::size measure(ui::size avail) override;
    void paint(ui::painter& p) override;

private:
    const stats_sampler& m_stats;
};

/* The overall load over the last minute as a line between two hairlines */
class load_history : public ui::widget {
public:
    explicit load_history(const stats_sampler& stats);

    ui::size measure(ui::size avail) override {
        return { avail.w, HISTORY_HEIGHT };
    }

    void paint(ui::painter& p) override;

private:
    const stats_sampler& m_stats;
    text_picture         m_label;
};

/* Memory in use as a row of cells, the page counts and the page size below */
class memory_meter : public ui::widget {
public:
    explicit memory_meter(const stats_sampler& stats);

    ui::size measure(ui::size avail) override {
        return { avail.w, MEMORY_METER_HEIGHT + MEMORY_CAPTION_GAP + CAPTION_HEIGHT };
    }

    void paint(ui::painter& p) override;

private:
    const stats_sampler& m_stats;
    text_picture         m_used;
    text_picture         m_of;
    text_picture         m_total;
    text_picture         m_pages;
    text_picture         m_page_size;
};

/* Uptime, tasks and the network side by side, each a caps name, a figure and a caption */
class stat_tiles : public ui::widget {
public:
    stat_tiles(const stats_sampler& stats, const network_summary& network);

    ui::size measure(ui::size avail) override {
        return { avail.w, TILES_HEIGHT };
    }

    void paint(ui::painter& p) override;

private:
    struct tile {
        text_picture name;
        text_picture figure;
        text_picture caption;
    };

    void update_tiles();

    const stats_sampler&   m_stats;
    const network_summary& m_network;
    tile                   m_uptime_tile;
    tile                   m_tasks_tile;
    tile                   m_network_tile;
};

/* The busiest processes by CPU: name, pid, percent of one CPU and CPU time */
class process_table : public ui::widget {
public:
    explicit process_table(const stats_sampler& stats);

    ui::size measure(ui::size avail) override {
        return { avail.w, PROCESS_HEADER_HEIGHT + static_cast<int32_t>(PROCESS_ROWS) * PROCESS_ROW_HEIGHT };
    }

    void paint(ui::painter& p) override;

private:
    struct row {
        text_picture name;
        text_picture pid;
        text_picture load;
        text_picture time;
    };

    const stats_sampler& m_stats;
    text_picture         m_name_heading;
    text_picture         m_pid_heading;
    text_picture         m_load_heading;
    text_picture         m_time_heading;
    std::vector<std::unique_ptr<row>> m_rows;
};

/* A label with an optional mono detail, such as Task monitor and stlxtop, chosen by a release over it */
class panel_link : public ui::widget {
public:
    panel_link(const char* label, const char* detail);

    std::function<void()> on_choose;

    ui::size measure(ui::size avail) override;
    void paint(ui::painter& p) override;
    void on_pointer_enter() override;
    void on_pointer_leave() override;
    bool on_pointer_up(const ui::pointer_event& e) override;

private:
    text_picture m_label;
    text_picture m_detail;
    bool         m_hover = false;
};

/* The header's pin, which toggles whether the panel is `pinned()` */
class pin_button : public ui::widget {
public:
    explicit pin_button(bool& pinned) : m_pinned(pinned) {}
    ~pin_button() override { stlxgfx_destroy_surface(m_ring); }

    ui::size measure(ui::size) override {
        return { PIN_SIZE, PIN_SIZE };
    }

    void paint(ui::painter& p) override;
    void on_pointer_enter() override;
    void on_pointer_leave() override;
    bool on_pointer_up(const ui::pointer_event& e) override;

private:
    bool&              m_pinned;
    bool               m_hover = false;
    stlxgfx_surface_t* m_ring = nullptr;
};

/* The header's caps title and caption */
class header_title : public ui::widget {
public:
    header_title();

    ui::size measure(ui::size) override {
        return { m_title.width() + HEADING_GAP + m_caption.width(), HEADER_HEIGHT };
    }

    void paint(ui::painter& p) override {
        m_title.draw(p, 0, 0, m_frame.h);
        m_caption.draw(p, m_title.width() + HEADING_GAP, 0, m_frame.h);
    }

private:
    text_picture m_title;
    text_picture m_caption;
};

/* The header: a row with a hairline along its bottom */
class header_row : public ui::box {
public:
    header_row() : ui::box(ui::axis::row) {}

    void paint(ui::painter& p) override {
        p.fill({ 0, m_frame.h - 1, m_frame.w, 1 }, STLXTHEME_LINE_SUBTLE);
    }
};

} // namespace

/* `span_ns` in whole seconds, such as 60 s */
static std::string seconds_text(uint64_t span_ns) {
    char text[24];
    snprintf(text, sizeof(text), "%llu s", static_cast<unsigned long long>(span_ns / NS_PER_SECOND));

    return text;
}

static void format_count(char* out, size_t cap, uint64_t count) {
    if (count < DIGIT_GROUP) {
        snprintf(out, cap, "%llu", static_cast<unsigned long long>(count));
        return;
    }

    char head[32];
    format_count(head, sizeof(head), count / DIGIT_GROUP);
    snprintf(out, cap, "%s,%03llu", head, static_cast<unsigned long long>(count % DIGIT_GROUP));
}

/* Bytes in gibibytes to two decimals, labelled GB */
static void format_gigabytes(char* out, size_t cap, uint64_t bytes) {
    uint64_t hundredths = bytes * HUNDREDTHS / GIBIBYTE;
    snprintf(out, cap, "%llu.%02llu GB", static_cast<unsigned long long>(hundredths / HUNDREDTHS),
             static_cast<unsigned long long>(hundredths % HUNDREDTHS));
}

static void format_cpu_time(char* out, size_t cap, uint64_t cpu_time_ns) {
    uint64_t seconds = cpu_time_ns / NS_PER_SECOND;
    snprintf(out, cap, "%llu:%02llu", static_cast<unsigned long long>(seconds / SECONDS_PER_MINUTE),
             static_cast<unsigned long long>(seconds % SECONDS_PER_MINUTE));
}

static text_picture caps_text(const char* text, uint32_t color) {
    return text_picture(text, STLXTHEME_UI_FONT_SEMIBOLD_PATH, STLXTHEME_CAPTION_SIZE, color);
}

static text_picture caption_text(const char* text) {
    return text_picture(text, STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM);
}

static text_picture mono_text(const char* text, float em_px, uint32_t color) {
    return text_picture(text, STLXTHEME_MONO_FONT_PATH, em_px, color);
}

heading_row::heading_row(const char* name, const char* caption)
    : m_name(caps_text(name, STLXTHEME_TEXT_SECONDARY)), m_caption(caption_text(caption)),
      m_figure(mono_text("", STLXTHEME_BODY_SIZE, STLXTHEME_TEXT)), m_suffix(caption_text("")) {}

void heading_row::paint(ui::painter& p) {
    m_name.draw(p, 0, 0, m_frame.h);
    m_caption.draw(p, m_name.width() + HEADING_GAP, 0, m_frame.h);

    int32_t suffix_x = m_frame.w - m_suffix.width();
    m_suffix.draw(p, suffix_x, 0, m_frame.h);
    m_figure.draw(p, suffix_x - m_figure.width(), 0, m_frame.h);
}

void cpu_heading::paint(ui::painter& p) {
    uint32_t count = m_stats.cpu_count();
    uint32_t busiest = 0;
    for (uint32_t cpu = 1; cpu < count; cpu++) {
        if (m_stats.cpu_load(cpu) > m_stats.cpu_load(busiest)) {
            busiest = cpu;
        }
    }

    char caption[48] = "no CPU figures";
    char figure[8] = "-";
    if (count > 0) {
        snprintf(caption, sizeof(caption), "%u %s, peak cpu%u %u%%", count, count == 1 ? "CPU" : "CPUs", busiest,
                 m_stats.cpu_load(busiest));
        snprintf(figure, sizeof(figure), "%u%%", m_stats.load());
    }

    m_caption.set_text(caption);
    m_figure.set_text(figure);

    heading_row::paint(p);
}

void memory_heading::paint(ui::painter& p) {
    uint64_t total = m_stats.total_pages();
    uint64_t used = m_stats.used_pages();

    char caption[24] = "no memory figures";
    char figure[24] = "-";
    std::string suffix;
    if (total > 0) {
        snprintf(caption, sizeof(caption), "%llu%%", static_cast<unsigned long long>(used * STATS_PERCENT / total));
        format_gigabytes(figure, sizeof(figure), used * m_stats.page_size());

        char total_text[24];
        format_gigabytes(total_text, sizeof(total_text), total * m_stats.page_size());
        suffix = std::string(" / ") + total_text;
    }

    m_caption.set_text(caption);
    m_figure.set_text(figure);
    m_suffix.set_text(suffix);

    heading_row::paint(p);
}

ui::size cpu_columns::measure(ui::size avail) {
    int32_t rows = static_cast<int32_t>((std::max(m_stats.cpu_count(), 1u) + CPUS_PER_ROW - 1) / CPUS_PER_ROW);
    return { avail.w, rows * CPU_ROW_HEIGHT + (rows - 1) * CPU_ROW_GAP };
}

void cpu_columns::paint(ui::painter& p) {
    uint32_t count = m_stats.cpu_count();
    int32_t columns = static_cast<int32_t>(std::min(std::max(count, 1u), CPUS_PER_ROW));
    int32_t column_w = (m_frame.w - (columns - 1) * CPU_COLUMN_GAP) / columns;
    for (uint32_t cpu = 0; cpu < count; cpu++) {
        int32_t x = static_cast<int32_t>(cpu % CPUS_PER_ROW) * (column_w + CPU_COLUMN_GAP);
        int32_t top = static_cast<int32_t>(cpu / CPUS_PER_ROW) * (CPU_ROW_HEIGHT + CPU_ROW_GAP);
        uint32_t lit = lit_cells(m_stats.cpu_load(cpu), STATS_PERCENT, CPU_CELLS);
        for (uint32_t cell = 0; cell < CPU_CELLS; cell++) {
            int32_t cell_y = top + CPU_COLUMN_HEIGHT - CPU_CELL_HEIGHT - static_cast<int32_t>(cell) * CPU_CELL_PITCH;
            p.fill({ x, cell_y, column_w, CPU_CELL_HEIGHT }, cell < lit ? STLXTHEME_TEXT : STLXTHEME_CPU_COLUMN_IDLE);
        }

        char index[8];
        snprintf(index, sizeof(index), "%u", cpu);
        text_picture label = mono_text(index, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM);
        int32_t label_top = top + CPU_COLUMN_HEIGHT + CPU_LABEL_GAP;
        label.draw(p, x + (column_w - label.width()) / 2, label_top, CPU_LABEL_HEIGHT);
    }
}

load_history::load_history(const stats_sampler& stats)
    : m_stats(stats), m_label(seconds_text(STATS_HISTORY * STATS_SAMPLE_PERIOD_NS), STLXTHEME_MONO_FONT_PATH,
                              STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM) {}

void load_history::paint(ui::painter& p) {
    p.fill({ 0, 0, m_frame.w, 1 }, STLXTHEME_LINE_SUBTLE);
    p.fill({ 0, m_frame.h - 1, m_frame.w, 1 }, STLXTHEME_LINE_SUBTLE);
    m_label.draw(p, m_frame.w - m_label.width(), HISTORY_LABEL_TOP, m_label.line_height());

    int32_t span = m_frame.h - 2 * HISTORY_LINE_INSET;
    int32_t newest_x = m_frame.w - 1;
    int32_t oldest_age = static_cast<int32_t>(STATS_HISTORY) - 1;
    int32_t full_scale = static_cast<int32_t>(STATS_PERCENT);
    ui::point newer = {};
    for (uint32_t age = 0; age < m_stats.history_count(); age++) {
        int32_t x = newest_x - static_cast<int32_t>(age) * newest_x / oldest_age;
        int32_t y = m_frame.h - HISTORY_LINE_INSET - static_cast<int32_t>(m_stats.load(age)) * span / full_scale;
        ui::point point = { x, y };
        if (age > 0) {
            p.line(point, newer, STLXTHEME_TEXT_SECONDARY);
        }

        newer = point;
    }
}

memory_meter::memory_meter(const stats_sampler& stats)
    : m_stats(stats), m_used(mono_text("", STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM)), m_of(caption_text(" of ")),
      m_total(mono_text("", STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM)), m_pages(caption_text(" pages")),
      m_page_size(caption_text("")) {}

void memory_meter::paint(ui::painter& p) {
    uint64_t total = m_stats.total_pages();
    uint32_t lit = lit_cells(m_stats.used_pages(), total, MEMORY_CELLS);
    for (uint32_t cell = 0; cell < MEMORY_CELLS; cell++) {
        int32_t x0 = static_cast<int32_t>(cell) * (m_frame.w + MEMORY_CELL_GAP) / static_cast<int32_t>(MEMORY_CELLS);
        int32_t x1 = static_cast<int32_t>(cell + 1) * (m_frame.w + MEMORY_CELL_GAP) / static_cast<int32_t>(MEMORY_CELLS)
                     - MEMORY_CELL_GAP;
        p.fill({ x0, 0, x1 - x0, MEMORY_METER_HEIGHT }, cell < lit ? STLXTHEME_TEXT : STLXTHEME_MEMORY_METER_IDLE);
    }

    if (total == 0) {
        return;
    }

    char used[32];
    char total_text[32];
    char page_size[24];
    format_count(used, sizeof(used), m_stats.used_pages());
    format_count(total_text, sizeof(total_text), total);
    snprintf(
        page_size,
        sizeof(page_size),
        "%llu KB pages",
        static_cast<unsigned long long>(m_stats.page_size() / KIBIBYTE)
    );

    m_used.set_text(used);
    m_total.set_text(total_text);
    m_page_size.set_text(page_size);

    int32_t top = MEMORY_METER_HEIGHT + MEMORY_CAPTION_GAP;
    int32_t x = 0;
    for (const text_picture* part : { &m_used, &m_of, &m_total, &m_pages }) {
        part->draw(p, x, top, CAPTION_HEIGHT);
        x += part->width();
    }

    m_page_size.draw(p, m_frame.w - m_page_size.width(), top, CAPTION_HEIGHT);
}

stat_tiles::stat_tiles(const stats_sampler& stats, const network_summary& network)
    : m_stats(stats), m_network(network),
      m_uptime_tile { caps_text("UPTIME", STLXTHEME_TEXT_DIM), mono_text("", STLXTHEME_BODY_SIZE, STLXTHEME_TEXT),
                      caption_text("") },
      m_tasks_tile { caps_text("TASKS", STLXTHEME_TEXT_DIM), mono_text("", STLXTHEME_BODY_SIZE, STLXTHEME_TEXT),
                     caption_text("") },
      m_network_tile { caps_text("NETWORK", STLXTHEME_TEXT_DIM), mono_text("", STLXTHEME_BODY_SIZE, STLXTHEME_TEXT),
                       caption_text("") } {}

void stat_tiles::update_tiles() {
    uint64_t uptime_s = m_stats.uptime_ns() / NS_PER_SECOND;
    time_t booted = time(nullptr) - static_cast<time_t>(uptime_s);
    struct tm* parts = gmtime(&booted);
    char since[24] = "";
    if (parts && uptime_s > 0) {
        strftime(since, sizeof(since), "since %H:%M", parts);
    }

    uint32_t process_count = m_stats.process_count();
    char tasks[16] = "-";
    char processes[24] = "";
    if (m_stats.task_count() > 0) {
        bool is_count_whole = process_count < STATS_PROCESSES_MAX && !m_stats.tasks_cut();
        const char* more = is_count_whole ? "" : "+";
        const char* noun = process_count == 1 ? "process" : "processes";
        snprintf(tasks, sizeof(tasks), "%u%s", m_stats.task_count(), m_stats.tasks_cut() ? "+" : "");
        snprintf(processes, sizeof(processes), "%u%s %s", process_count, more, noun);
    }

    m_uptime_tile.figure.set_text(m_stats.uptime_ns() > 0 ? format_uptime(m_stats.uptime_ns()) : "-");
    m_uptime_tile.caption.set_text(since);
    m_tasks_tile.figure.set_text(tasks);
    m_tasks_tile.caption.set_text(processes);
    m_network_tile.figure.set_text(m_network.address);
    m_network_tile.caption.set_text(m_network.interface);
}

void stat_tiles::paint(ui::painter& p) {
    update_tiles();

    const tile* tiles[TILE_COUNT] = { &m_uptime_tile, &m_tasks_tile, &m_network_tile };
    int32_t shares_before = 0;
    for (int32_t i = 0; i < TILE_COUNT; i++) {
        int32_t x = shares_before * m_frame.w / TILE_SHARE_TOTAL;
        if (i > 0) {
            p.fill({ x, 0, 1, m_frame.h }, STLXTHEME_LINE_SUBTLE);
        }

        const tile& shown = *tiles[i];
        int32_t text_x = x + TILE_PADDING_X;
        shown.name.draw(p, text_x, TILE_PADDING_Y, CAPTION_HEIGHT);
        shown.figure.draw(p, text_x, TILE_PADDING_Y + CAPTION_HEIGHT, TILE_FIGURE_HEIGHT);
        shown.caption.draw(p, text_x, TILE_PADDING_Y + CAPTION_HEIGHT + TILE_FIGURE_HEIGHT, CAPTION_HEIGHT);
        shares_before += TILE_SHARES[i];
    }
}

process_table::process_table(const stats_sampler& stats)
    : m_stats(stats), m_name_heading(caps_text("NAME", STLXTHEME_TEXT_DIM)),
      m_pid_heading(caps_text("PID", STLXTHEME_TEXT_DIM)), m_load_heading(caps_text("CPU %", STLXTHEME_TEXT_DIM)),
      m_time_heading(caps_text("TIME", STLXTHEME_TEXT_DIM)) {
    for (uint32_t rank = 0; rank < PROCESS_ROWS; rank++) {
        uint32_t load_color = rank == 0 ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY;
        auto added = std::unique_ptr<row>(new row {
            text_picture("", STLXTHEME_UI_FONT_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT),
            mono_text("", STLXTHEME_BODY_SIZE, STLXTHEME_TEXT_SECONDARY),
            mono_text("", STLXTHEME_BODY_SIZE, load_color),
            mono_text("", STLXTHEME_BODY_SIZE, STLXTHEME_TEXT_SECONDARY) });
        m_rows.push_back(std::move(added));
    }
}

void process_table::paint(ui::painter& p) {
    int32_t time_right = m_frame.w;
    int32_t load_right = time_right - TIME_COLUMN_WIDTH - PROCESS_COLUMN_GAP;
    int32_t pid_right = load_right - LOAD_COLUMN_WIDTH - PROCESS_COLUMN_GAP;
    int32_t name_width = pid_right - PID_COLUMN_WIDTH - PROCESS_COLUMN_GAP;

    m_name_heading.draw(p, 0, 0, PROCESS_HEADER_HEIGHT);
    m_pid_heading.draw(p, pid_right - m_pid_heading.width(), 0, PROCESS_HEADER_HEIGHT);
    m_load_heading.draw(p, load_right - m_load_heading.width(), 0, PROCESS_HEADER_HEIGHT);
    m_time_heading.draw(p, time_right - m_time_heading.width(), 0, PROCESS_HEADER_HEIGHT);

    uint32_t shown_rows = std::min(m_stats.process_count(), PROCESS_ROWS);
    for (uint32_t rank = 0; rank < shown_rows; rank++) {
        const stats_process& process = m_stats.process(rank);
        char pid[16];
        snprintf(pid, sizeof(pid), "%u", process.pid);
        char load[16];
        snprintf(load, sizeof(load), "%u.%u", process.load_tenths / TENTHS, process.load_tenths % TENTHS);

        char time_text[24];
        format_cpu_time(time_text, sizeof(time_text), process.cpu_time_ns);

        row& shown = *m_rows[rank];
        shown.name.set_text(process.name);
        shown.pid.set_text(pid);
        shown.load.set_text(load);
        shown.time.set_text(time_text);

        int32_t top = PROCESS_HEADER_HEIGHT + static_cast<int32_t>(rank) * PROCESS_ROW_HEIGHT;
        p.push_clip({ 0, top, name_width, PROCESS_ROW_HEIGHT });
        shown.name.draw(p, 0, top, PROCESS_ROW_HEIGHT);
        p.pop_clip();

        shown.pid.draw(p, pid_right - shown.pid.width(), top, PROCESS_ROW_HEIGHT);
        shown.load.draw(p, load_right - shown.load.width(), top, PROCESS_ROW_HEIGHT);
        shown.time.draw(p, time_right - shown.time.width(), top, PROCESS_ROW_HEIGHT);
    }
}

panel_link::panel_link(const char* label, const char* detail)
    : m_label(label, STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT),
      m_detail(mono_text(detail, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT_SECONDARY)) {}

ui::size panel_link::measure(ui::size) {
    int32_t detail = m_detail.width() > 0 ? LINK_GAP + m_detail.width() : 0;
    return { 2 * LINK_PADDING_X + m_label.width() + detail, HEADING_HEIGHT + 2 * LINK_PADDING_Y };
}

void panel_link::paint(ui::painter& p) {
    if (m_hover) {
        p.rounded_rect({ 0, 0, m_frame.w, m_frame.h }, STLXTHEME_CONTROL_RADIUS, STLXTHEME_SURFACE_HOVER);
    }

    m_label.draw(p, LINK_PADDING_X, 0, m_frame.h);
    m_detail.draw(p, LINK_PADDING_X + m_label.width() + LINK_GAP, 0, m_frame.h);
}

void panel_link::on_pointer_enter() {
    m_hover = true;
    m_detail.set_color(STLXTHEME_TEXT);
    invalidate();
}

void panel_link::on_pointer_leave() {
    m_hover = false;
    m_detail.set_color(STLXTHEME_TEXT_SECONDARY);
    invalidate();
}

bool panel_link::on_pointer_up(const ui::pointer_event&) {
    if (on_choose) {
        on_choose();
    }

    return true;
}

void pin_button::paint(ui::painter& p) {
    ui::rect slot = { 0, 0, m_frame.w, m_frame.h };
    if (m_pinned) {
        if (!m_ring) {
            m_ring = make_ring_picture(slot.w, slot.h, STLXTHEME_CONTROL_RADIUS, STLXTHEME_LINE_ON);
        }

        p.rounded_rect(slot, STLXTHEME_CONTROL_RADIUS, STLXTHEME_SURFACE_ON);
        p.image({ 0, 0 }, m_ring);
    } else if (m_hover) {
        p.rounded_rect(slot, STLXTHEME_CONTROL_RADIUS, STLXTHEME_SURFACE_HOVER);
    }

    uint32_t color = m_pinned || m_hover ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY;
    int32_t x = (slot.w - PIN_GLYPH_WIDTH) / 2;
    int32_t y = (slot.h - PIN_GLYPH_HEIGHT) / 2;
    p.fill({ x + PIN_HEAD_INSET, y, PIN_GLYPH_WIDTH - 2 * PIN_HEAD_INSET, PIN_HEAD_HEIGHT }, color);
    p.fill({ x, y + PIN_HEAD_HEIGHT, PIN_GLYPH_WIDTH, PIN_COLLAR_HEIGHT }, color);
    p.fill({ x + (PIN_GLYPH_WIDTH - PIN_NEEDLE_WIDTH) / 2, y + PIN_HEAD_HEIGHT + PIN_COLLAR_HEIGHT, PIN_NEEDLE_WIDTH,
             PIN_GLYPH_HEIGHT - PIN_HEAD_HEIGHT - PIN_COLLAR_HEIGHT }, color);
}

void pin_button::on_pointer_enter() {
    m_hover = true;
    invalidate();
}

void pin_button::on_pointer_leave() {
    m_hover = false;
    invalidate();
}

bool pin_button::on_pointer_up(const ui::pointer_event&) {
    m_pinned = !m_pinned;
    invalidate();

    return true;
}

header_title::header_title()
    : m_title(caps_text("SYSTEM", STLXTHEME_TEXT_SECONDARY)),
      m_caption(caption_text(("sampled every " + seconds_text(STATS_SAMPLE_PERIOD_NS)).c_str())) {}

int32_t stats_panel::width() const {
    return PANEL_WIDTH;
}

int32_t stats_panel::left(int32_t output_w) const {
    return output_w - PANEL_RIGHT_INSET - PANEL_WIDTH;
}

std::unique_ptr<ui::widget> stats_panel::build() {
    m_stats.set_reads_tasks(true);
    m_live.clear();

    auto root = std::make_unique<ui::box>(ui::axis::column);

    header_row* header = root->add<header_row>();
    header->s().padding = { 0, HEADER_PADDING_RIGHT, 0, HEADER_PADDING_LEFT };
    header->s().align_items = ui::align::center;
    header->add<header_title>();
    header->add<ui::box>()->s().main = ui::length::flex();
    header->add<pin_button>(m_pinned);

    panel_section* cpu = root->add<panel_section>(false);
    m_live.push_back(cpu->add<cpu_heading>(m_stats));
    m_live.push_back(cpu->add<cpu_columns>(m_stats));
    m_live.push_back(cpu->add<load_history>(m_stats));

    panel_section* memory = root->add<panel_section>(true);
    m_live.push_back(memory->add<memory_heading>(m_stats));
    m_live.push_back(memory->add<memory_meter>(m_stats));

    panel_section* tiles = root->add<panel_section>(true);
    tiles->s().padding = {};
    m_live.push_back(tiles->add<stat_tiles>(m_stats, m_network));

    panel_section* processes = root->add<panel_section>(true);
    ui::box* heading = processes->add<ui::box>(ui::axis::row);
    heading->s().align_items = ui::align::center;
    heading->add<heading_row>("TOP PROCESSES", "by CPU")->s().main = ui::length::flex();

    const char* task_monitor_name = strrchr(TASK_MONITOR_PROGRAM.args, '/') + 1;
    panel_link* task_monitor = heading->add<panel_link>(TASK_MONITOR_PROGRAM.label, task_monitor_name);
    m_live.push_back(processes->add<process_table>(m_stats));

    panel_section* footer = root->add<panel_section>(true);
    footer->s().padding = ui::edge_insets::xy(FOOTER_PADDING_X, FOOTER_PADDING_Y);
    footer->s().align_items = ui::align::start;
    panel_link* settings = footer->add<panel_link>(SETTINGS_PROGRAM.label, "");

    task_monitor->on_choose = [this]() {
        on_spawn(TASK_MONITOR_PROGRAM.path, TASK_MONITOR_PROGRAM.args);
    };

    settings->on_choose = [this]() {
        on_spawn(SETTINGS_PROGRAM.path, SETTINGS_PROGRAM.args);
    };

    return root;
}

void stats_panel::refresh() {
    for (ui::widget* live : m_live) {
        live->invalidate();
    }
}

void stats_panel::closed() {
    m_live.clear();
    m_pinned = false;
    m_stats.set_reads_tasks(false);
}
