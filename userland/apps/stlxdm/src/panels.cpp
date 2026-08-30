/* The compositor's panels on its own toolkit hosts. Band surfaces
 * are retained like window buffers: the toolkit repaints only dirty
 * widgets into them, and compose blits whatever region damage
 * touches. The dock launches the config's pinned apps, and the top
 * bar shows system stats, the clock, and network state.
 */
#include "panels.hpp"

#include <stlx/net.h>
#include <stlxgfx/bmp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>

constexpr uint32_t PIN_BG = 0xFF313244;
constexpr uint32_t PIN_BG_HOVER = 0xFF45475A;
constexpr uint32_t PIN_BG_PRESS = 0xFF3E3E52;
constexpr uint32_t PIN_RING = 0xFF585B70;
constexpr int32_t PIN_RADIUS = 6;
constexpr int32_t PIN_ICON_RADIUS = 5;
constexpr uint32_t PIN_FALLBACK_FG = 0xFF6C7086;

constexpr uint32_t TIP_BG = 0xFF45475A;
constexpr uint32_t TIP_FG = 0xFFCDD6F4;
constexpr int32_t TIP_RADIUS = 4;
constexpr int32_t TIP_PAD_H = 8;
constexpr int32_t TIP_PAD_V = 4;
constexpr int32_t TIP_GAP = 6;
constexpr uint32_t TIP_FONT_PX = 12;

/* The tooltip pops above the dock, so hover transitions damage this
 * far beyond the band */
constexpr int32_t TIP_REACH = 48;

/* Refresh cadences. The tick runs on every wakeup, so the queries
 * gate themselves by elapsed time, stats each second and the rarely
 * changing network answer every third. */
constexpr uint64_t STATS_POLL_NS = 1000000000ull;
constexpr uint64_t NET_POLL_NS = 3000000000ull;

/* Letter-glyph fallback colors for pins without an icon file */
static const uint32_t PIN_ACCENTS[] = {
    0xFF89B4FA, 0xFFF38BA8, 0xFFA6E3A1, 0xFFFAB387,
    0xFFCBA6F7, 0xFF94E2D5, 0xFFF9E2AF, 0xFFEBA0AC,
    0xFF74C7EC, 0xFFB4BEFE, 0xFFF2CDCD, 0xFFF5C2E7,
    0xFF89DCEB, 0xFFBAC2DE, 0xFFCDD6F4, 0xFFFAB387
};

/* Live top bar strings, refreshed on the clock tick */
static char g_stats_str[64] = "";
static uint64_t g_stats_prev_busy = 0;
static uint64_t g_stats_prev_total = 0;

namespace {

/* One pinned launcher: an icon tile on a rounded hover surface,
 * firing its launch callback on release inside */
class dock_button : public ui::widget {
public:
    dock_button(stlxgfx_surface_t* icon, const dm_conf_pin& pin,
                int32_t index, uint32_t accent, int32_t icon_px)
        : m_icon(icon), m_pin(&pin), m_index(index), m_accent(accent),
          m_icon_px(icon_px) {}

    std::function<void(const char*)> on_launch;
    std::function<void(int32_t, bool)> on_hover;

    ui::size measure(ui::size) override {
        return { m_icon_px + 2, m_icon_px + 2 };
    }

    void paint(ui::painter& p) override {
        ui::rect icon_box = { 1, 1, m_icon_px, m_icon_px };

        if (m_hover && !m_pressed) {
            p.rounded_rect({ 0, 0, m_frame.w, m_frame.h },
                           PIN_RADIUS + 1, PIN_RING);
        }
        p.rounded_rect(icon_box, PIN_RADIUS,
                       m_pressed ? PIN_BG_PRESS
                       : m_hover ? PIN_BG_HOVER : PIN_BG);

        if (m_icon) {
            p.image({ icon_box.x, icon_box.y }, m_icon, PIN_ICON_RADIUS);
            return;
        }

        /* Fallback glyph: the label's first letter, or a question
         * mark when there is no label either */
        char glyph[2] = { m_pin->label[0] ? m_pin->label[0] : '?', '\0' };
        uint32_t px = static_cast<uint32_t>(m_icon_px) * 2 / 3;
        if (px < 10) {
            px = 10;
        }

        ui::size ts = p.measure_text(glyph, px);
        int32_t tx = icon_box.x + (icon_box.w - ts.w) / 2;
        int32_t ty = icon_box.y + (icon_box.h - ts.h) / 2
                   + p.font_ascent(px);
        p.text({ tx, ty }, glyph, px,
               m_pin->label[0] ? m_accent : PIN_FALLBACK_FG);
    }

    void on_pointer_enter() override {
        m_hover = true;
        invalidate();

        if (on_hover) {
            on_hover(m_index, true);
        }
    }

    void on_pointer_leave() override {
        m_hover = false;
        m_pressed = false;
        invalidate();

        if (on_hover) {
            on_hover(m_index, false);
        }
    }

    bool on_pointer_down(const ui::pointer_event&) override {
        m_pressed = true;
        invalidate();

        return true;
    }

    bool on_pointer_up(const ui::pointer_event& e) override {
        bool inside = e.pos.x >= 0 && e.pos.y >= 0 &&
                      e.pos.x < m_frame.w && e.pos.y < m_frame.h;
        bool fire = m_pressed && inside;

        m_pressed = false;
        invalidate();

        if (fire && on_launch && m_pin->path[0]) {
            on_launch(m_pin->path);
        }

        return true;
    }

private:
    stlxgfx_surface_t* m_icon = nullptr;
    const dm_conf_pin* m_pin = nullptr;
    int32_t m_index = 0;
    uint32_t m_accent = 0;
    int32_t m_icon_px = 0;
    bool m_hover = false;
    bool m_pressed = false;
};

} // namespace

static stlxgfx_surface_t* make_band(uint32_t w, int32_t h) {
    return stlxgfx_create_surface(w, static_cast<uint32_t>(h),
                                  32, 16, 8, 0);
}

static uint64_t monotonic_ns() {
    timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull
         + static_cast<uint64_t>(ts.tv_nsec);
}

/* Weekday, date, and time for the bar's center */
static void format_clock(char* out, size_t cap) {
    time_t t = time(nullptr);
    struct tm* tm = gmtime(&t);
    if (!tm) {
        out[0] = '\0';
        return;
    }

    strftime(out, cap, "%a %b %e  %H:%M:%S", tm);
}

/* The default interface summarized as a short bar string */
static void format_net(char* out, size_t cap) {
    struct stlx_net_status st;
    if (stlx_net_get_status(&st) != 0 || st.if_count == 0) {
        snprintf(out, cap, "no network");
        return;
    }

    const struct stlx_ifinfo* def = stlx_net_default_if(&st);
    if (!def) {
        snprintf(out, cap, "no network");
        return;
    }

    if (!(def->flags & STLX_IFF_UP)) {
        snprintf(out, cap, "disconnected");
        return;
    }
    if (!(def->flags & STLX_IFF_CONFIGURED)) {
        snprintf(out, cap, "unconfigured");
        return;
    }

    uint32_t ip = def->ipv4_addr;
    snprintf(out, cap, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

/* Reads a small /dev/sysinfo file into buf, NUL terminated */
static ssize_t read_stats_file(const char* path, char* buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    size_t total = 0;
    while (total < cap - 1) {
        ssize_t rd = read(fd, buf + total, cap - 1 - total);
        if (rd <= 0) {
            break;
        }
        total += static_cast<size_t>(rd);
    }
    close(fd);
    buf[total] = '\0';

    return static_cast<ssize_t>(total);
}

/* Value of a "<label> <number>" line in a stats text snapshot */
static uint64_t stats_field(const char* text, const char* label) {
    const char* p = strstr(text, label);
    if (!p) {
        return 0;
    }

    return strtoull(p + strlen(label), nullptr, 10);
}

/* Overall CPU utilization from busy and idle tick deltas, plus
 * memory consumption from page counters */
static void update_sys_stats() {
    char buf[512];
    if (read_stats_file("/dev/sysinfo/cpu", buf, sizeof(buf)) <= 0) {
        g_stats_str[0] = '\0';
        return;
    }

    uint64_t busy = 0;
    uint64_t idle = 0;
    const char* p = buf;
    while (*p) {
        if (strncmp(p, "cpu", 3) == 0) {
            char* end = nullptr;
            strtoull(p + 3, &end, 10);
            busy += strtoull(end, &end, 10);
            idle += strtoull(end, &end, 10);
        }
        while (*p && *p != '\n') {
            p++;
        }
        if (*p) {
            p++;
        }
    }

    uint64_t total = busy + idle;
    unsigned cpu_pct = 0;
    if (g_stats_prev_total != 0 && total > g_stats_prev_total) {
        uint64_t d_busy = busy - g_stats_prev_busy;
        uint64_t d_total = total - g_stats_prev_total;
        cpu_pct = static_cast<unsigned>((d_busy * 100 + d_total / 2)
                                        / d_total);
    }
    g_stats_prev_busy = busy;
    g_stats_prev_total = total;

    if (read_stats_file("/dev/sysinfo/mem", buf, sizeof(buf)) <= 0) {
        g_stats_str[0] = '\0';
        return;
    }

    uint64_t page_size = stats_field(buf, "page_size ");
    uint64_t total_pages = stats_field(buf, "total_pages ");
    uint64_t used_pages = stats_field(buf, "used_pages ");
    uint64_t used_mb = used_pages * page_size / (1024 * 1024);
    uint64_t total_mb = total_pages * page_size / (1024 * 1024);

    snprintf(g_stats_str, sizeof(g_stats_str),
             "CPU %u%%  MEM %llu/%llu MB", cpu_pct,
             static_cast<unsigned long long>(used_mb),
             static_cast<unsigned long long>(total_mb));
}

int dm_panels::init(uint32_t screen_w, uint32_t screen_h,
                    const dm_config& conf) {
    m_conf = &conf;
    m_width = screen_w;
    m_height = screen_h;
    m_dock_h = static_cast<int32_t>(conf.taskbar_height);
    m_dock_y = static_cast<int32_t>(screen_h) - m_dock_h;

    /* One row below the bar carries its accent underline, outside the
     * toolkit's layout so it never repaints */
    m_band = make_band(screen_w, BAR_H + 1);
    m_dock = make_band(screen_w, m_dock_h);
    if (!m_band || !m_dock) {
        return -1;
    }

    stlxgfx_fill_rect(m_band, 0, BAR_H, screen_w, 1, conf.accent_color);

    m_tip_font = stlxgfx_font_open(STLXGFX_FONT_PATH, TIP_FONT_PX);
    if (!m_tip_font) {
        return -1;
    }
    stlxgfx_font_metrics_get(m_tip_font, &m_tip_fm);

    /* The bar: name and stats left, the clock truly centered between
     * two equal flex halves, network state right */
    auto bar = std::make_unique<ui::box>(ui::axis::row);
    bar->s().background = conf.bar_color;
    bar->s().padding = ui::edge_insets::xy(10, 0);
    bar->s().align_items = ui::align::center;

    ui::box* left = bar->add<ui::box>(ui::axis::row);
    left->s().main = ui::length::flex();
    left->s().align_items = ui::align::center;
    left->s().gap = 24;

    ui::label* name = left->add<ui::label>("Stellux");
    name->s().main = ui::length::content();
    name->set_color(conf.text_color);

    update_sys_stats();
    m_stats_query_ns = monotonic_ns();
    m_stats = left->add<ui::label>(g_stats_str);
    m_stats->s().main = ui::length::content();
    m_stats->set_color(conf.accent_color);

    char text[64];
    format_clock(text, sizeof(text));
    m_clock = bar->add<ui::label>(text);
    m_clock->s().main = ui::length::content();
    m_clock->set_color(conf.text_color);

    ui::box* right = bar->add<ui::box>(ui::axis::row);
    right->s().main = ui::length::flex();
    right->s().align_items = ui::align::center;
    right->s().justify = ui::align::end;

    char net[32];
    format_net(net, sizeof(net));
    m_net_query_ns = monotonic_ns();
    m_net = right->add<ui::label>(net);
    m_net->s().main = ui::length::content();
    m_net->set_color(conf.text_color);

    m_host.set_root(std::move(bar));

    /* The dock: the config's pins centered between flex spacers. The
     * accent top edge is drawn at compose time over the band blit. */
    auto dock = std::make_unique<ui::box>(ui::axis::row);
    dock->s().background = conf.bar_color;
    dock->s().align_items = ui::align::center;

    /* Tiles are one pixel wider than their icons on each side, so the
     * icon pitch matches the configured icon size plus spacing */
    int32_t spacing = static_cast<int32_t>(conf.taskbar_spacing);
    dock->s().gap = spacing >= 2 ? spacing - 2 : 0;

    ui::box* lead = dock->add<ui::box>();
    lead->s().main = ui::length::flex();

    int32_t icon_px = static_cast<int32_t>(conf.taskbar_icon_size);
    for (uint32_t i = 0; i < conf.pin_count; i++) {
        const dm_conf_pin& pin = conf.pins[i];

        stlxgfx_surface_t* icon = nullptr;
        if (pin.icon_path[0]) {
            icon = stlxgfx_load_bmp(pin.icon_path);
        }
        if (!icon) {
            icon = stlxgfx_load_bmp(DM_CONF_DEFAULT_ICON);
        }

        uint32_t accent = PIN_ACCENTS[
            i % (sizeof(PIN_ACCENTS) / sizeof(PIN_ACCENTS[0]))];
        dock_button* btn = dock->add<dock_button>(
            icon, pin, static_cast<int32_t>(i), accent, icon_px);
        btn->s().main = ui::length::fixed(icon_px + 2);
        btn->s().cross = ui::length::fixed(icon_px + 2);
        btn->on_launch = [this](const char* path) {
            if (on_launch) {
                on_launch(path);
            }
        };
        btn->on_hover = [this](int32_t index, bool entered) {
            hover_pin(index, entered);
        };
    }

    ui::box* tail = dock->add<ui::box>();
    tail->s().main = ui::length::flex();

    m_dock_host.set_root(std::move(dock));

    return 0;
}

void dm_panels::shutdown() {
    stlxgfx_destroy_surface(m_band);
    stlxgfx_destroy_surface(m_dock);
    m_band = nullptr;
    m_dock = nullptr;

    stlxgfx_font_close(m_tip_font);
    m_tip_font = nullptr;
}

void dm_panels::hover_pin(int32_t index, bool entered) {
    if (entered) {
        m_hover_pin = index;
        return;
    }

    if (m_hover_pin == index) {
        m_hover_pin = -1;
    }
}

/* Screen-space icon box of one pin, the flex-centered row reduced to
 * arithmetic: tiles are icon plus two, pitch adds the gap */
damage_list::rect dm_panels::pin_icon_rect(int32_t index) const {
    int32_t n = static_cast<int32_t>(m_conf->pin_count);
    int32_t icon = static_cast<int32_t>(m_conf->taskbar_icon_size);
    int32_t tile = icon + 2;
    int32_t spacing = static_cast<int32_t>(m_conf->taskbar_spacing);
    int32_t gap = spacing >= 2 ? spacing - 2 : 0;

    int32_t total = n * tile + (n - 1) * gap;
    int32_t start = (static_cast<int32_t>(m_width) - total) / 2;
    int32_t x = start + index * (tile + gap) + 1;
    int32_t y = m_dock_y + (m_dock_h - icon) / 2;

    return { x, y, icon, icon };
}

void dm_panels::flush(damage_list& damage) {
    if (m_band && m_host.dirty()) {
        m_host.layout_now(static_cast<int32_t>(m_width), BAR_H);

        std::vector<ui::rect> out;
        m_host.paint_now(m_band, out);

        for (const ui::rect& r : out) {
            damage.add(r.x, r.y, r.w, r.h);
        }
    }

    if (m_dock && m_dock_host.dirty()) {
        m_dock_host.layout_now(static_cast<int32_t>(m_width), m_dock_h);

        std::vector<ui::rect> out;
        m_dock_host.paint_now(m_dock, out);

        /* Dock rects translate down to the band's screen position */
        for (const ui::rect& r : out) {
            damage.add(r.x, r.y + m_dock_y, r.w, r.h);
        }
    }

    /* A tooltip appearing or vanishing repaints the dock strip plus
     * its reach above the band */
    if (m_hover_pin != m_drawn_hover_pin) {
        damage.add(0, m_dock_y - TIP_REACH,
                   static_cast<int32_t>(m_width), TIP_REACH + m_dock_h);
        m_drawn_hover_pin = m_hover_pin;
    }
}

void dm_panels::compose(stlxgfx_surface_t* back,
                        const damage_list::rect& r) {
    int32_t band_h = BAR_H + 1;
    if (m_band && r.y < band_h) {
        int32_t x0 = r.x > 0 ? r.x : 0;
        int32_t y0 = r.y > 0 ? r.y : 0;
        int32_t x1 = r.x + r.w < static_cast<int32_t>(m_width)
                   ? r.x + r.w : static_cast<int32_t>(m_width);
        int32_t y1 = r.y + r.h < band_h ? r.y + r.h : band_h;

        if (x0 < x1 && y0 < y1) {
            stlxgfx_blit(back, x0, y0, m_band, x0, y0,
                         static_cast<uint32_t>(x1 - x0),
                         static_cast<uint32_t>(y1 - y0));
        }
    }

    if (m_dock && r.y + r.h > m_dock_y) {
        int32_t x0 = r.x > 0 ? r.x : 0;
        int32_t y0 = r.y > m_dock_y ? r.y : m_dock_y;
        int32_t x1 = r.x + r.w < static_cast<int32_t>(m_width)
                   ? r.x + r.w : static_cast<int32_t>(m_width);
        int32_t y1 = r.y + r.h < m_dock_y + m_dock_h
                   ? r.y + r.h : m_dock_y + m_dock_h;

        if (x0 < x1 && y0 < y1) {
            stlxgfx_blit(back, x0, y0, m_dock, x0, y0 - m_dock_y,
                         static_cast<uint32_t>(x1 - x0),
                         static_cast<uint32_t>(y1 - y0));

            /* The dock's accent top edge, mirroring the bar's underline */
            if (y0 == m_dock_y) {
                stlxgfx_fill_rect(back, x0, m_dock_y,
                                  static_cast<uint32_t>(x1 - x0), 1,
                                  m_conf->accent_color);
            }
        }
    }
}

void dm_panels::compose_top(stlxgfx_surface_t* back,
                            const damage_list::rect& r) {
    if (m_hover_pin < 0 ||
        m_hover_pin >= static_cast<int32_t>(m_conf->pin_count)) {
        return;
    }

    const dm_conf_pin& pin = m_conf->pins[m_hover_pin];
    if (!pin.label[0]) {
        return;
    }

    int32_t tw = stlxgfx_text_width(m_tip_font, pin.label,
                                    strlen(pin.label));
    int32_t th = m_tip_fm.ascent + m_tip_fm.descent;
    damage_list::rect icon = pin_icon_rect(m_hover_pin);

    int32_t tip_w = tw + 2 * TIP_PAD_H;
    int32_t tip_h = th + 2 * TIP_PAD_V;
    int32_t tip_x = icon.x + icon.w / 2 - tip_w / 2;
    int32_t tip_y = icon.y - tip_h - TIP_GAP;

    if (tip_x < 2) {
        tip_x = 2;
    }
    if (tip_x + tip_w > static_cast<int32_t>(m_width) - 2) {
        tip_x = static_cast<int32_t>(m_width) - tip_w - 2;
    }
    if (tip_y < 2) {
        tip_y = 2;
    }

    if (tip_x >= r.x + r.w || tip_x + tip_w <= r.x ||
        tip_y >= r.y + r.h || tip_y + tip_h <= r.y) {
        return;
    }

    /* A view over the compose rect clips the box and text for free */
    stlxgfx_surface_t* view = stlxgfx_surface_from_buffer(
        back->pixels + static_cast<uint32_t>(r.y) * back->pitch
            + static_cast<uint32_t>(r.x) * 4,
        static_cast<uint32_t>(r.w), static_cast<uint32_t>(r.h),
        back->pitch, 32, 16, 8, 0);
    if (!view) {
        return;
    }

    stlxgfx_fill_rounded_rect(view, tip_x - r.x, tip_y - r.y,
                              static_cast<uint32_t>(tip_w),
                              static_cast<uint32_t>(tip_h),
                              static_cast<uint32_t>(TIP_RADIUS), TIP_BG);
    stlxgfx_draw_text(view, m_tip_font, tip_x - r.x + TIP_PAD_H,
                      tip_y - r.y + TIP_PAD_V + m_tip_fm.ascent,
                      pin.label, strlen(pin.label), TIP_FG);
    stlxgfx_destroy_surface(view);
}

void dm_panels::pointer_move(int32_t x, int32_t y) {
    if (y >= m_dock_y) {
        m_host.pointer_move(-1, -1);
        m_dock_host.pointer_move(x, y - m_dock_y);
        return;
    }

    m_dock_host.pointer_move(-1, -1);
    m_host.pointer_move(x, y);
}

void dm_panels::pointer_button(int32_t x, int32_t y, uint8_t btn,
                               bool down) {
    if (y >= m_dock_y) {
        m_dock_host.pointer_button(x, y - m_dock_y, btn, down);
        return;
    }

    m_host.pointer_button(x, y, btn, down);
}

int64_t dm_panels::clock_timeout_ns(uint64_t now_ns) const {
    uint64_t into_second = now_ns % 1000000000ull;

    return static_cast<int64_t>(1000000000ull - into_second);
}

void dm_panels::clock_tick() {
    if (!m_clock) {
        return;
    }

    char text[64];
    format_clock(text, sizeof(text));
    m_clock->set_text(text);

    /* Stats sample busy and idle deltas, so calls faster than the
     * cadence would shrink the window to noise and repaint per wake */
    uint64_t now = monotonic_ns();
    if (m_stats && now - m_stats_query_ns >= STATS_POLL_NS) {
        m_stats_query_ns = now;

        update_sys_stats();
        m_stats->set_text(g_stats_str);
    }

    if (m_net && now - m_net_query_ns >= NET_POLL_NS) {
        m_net_query_ns = now;

        char net[32];
        format_net(net, sizeof(net));
        m_net->set_text(net);
    }
}
