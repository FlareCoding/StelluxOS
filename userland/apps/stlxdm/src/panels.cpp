/* The compositor's panels on its own toolkit hosts. Band surfaces
 * are retained like window buffers: the toolkit repaints only dirty
 * widgets into them, and compose blits whatever region damage
 * touches. The dock launches pinned apps, the top bar shows the
 * clock and network state, and the power overlay dims the desktop
 * above every window.
 */
#include "panels.hpp"

#include <stlx/net.h>
#include <stlx/proc.h>
#include <stlxgfx/bmp.h>

#include <cstdio>
#include <cstring>
#include <ctime>

/* Pinned launchers, conf driven once the conf unit lands */
struct dock_pin {
    const char* icon_path;
    const char* launch_path;
};

static const dock_pin DOCK_PINS[] = {
    { "/etc/res/icons/icon_stlxterm_32x32.bmp", "/bin/stlxterm" },
    { "/etc/res/icons/icon_doom_32x32.bmp", "/bin/doom" },
    { "/etc/res/icons/icon_unknown_32x32.bmp", "/bin/uidemo" },
};
constexpr const char* DEFAULT_ICON =
    "/etc/res/icons/icon_unknown_32x32.bmp";

constexpr int32_t DOCK_BTN_W = 40;
constexpr int32_t DOCK_ICON = 32;
constexpr int32_t ORB_R = 36;
constexpr int32_t ORB_BOX_W = 110;
constexpr int32_t ORB_BOX_H = 130;
constexpr uint32_t DIM_COLOR = 0xA0000000;

/* Every net query cadence in clock ticks */
constexpr uint32_t NET_POLL_TICKS = 3;

/* Launched apps live their own lives, the DM never waits */
static void launch_detached(const char* path) {
    int handle = proc_create(path, nullptr);
    if (handle < 0) {
        printf("stlxdm: launch failed: %s\r\n", path);
        return;
    }

    if (proc_start(handle) < 0) {
        printf("stlxdm: start failed: %s\r\n", path);
        return;
    }

    proc_detach(handle);
}

namespace {

/* One pinned launcher: an icon on a hover surface, spawning its app
 * detached on release */
class dock_button : public ui::widget {
public:
    dock_button(stlxgfx_surface_t* icon, const char* path)
        : m_icon(icon), m_path(path) {}

    ui::size measure(ui::size) override {
        return { DOCK_BTN_W, dm_panels::DOCK_H };
    }

    void paint(ui::painter& p) override {
        const ui::theme& t = ui::theme::active();

        if (m_pressed) {
            p.fill({ 0, 0, m_frame.w, m_frame.h }, t.surface_press);
        } else if (m_hover) {
            p.fill({ 0, 0, m_frame.w, m_frame.h }, t.surface_hover);
        }

        if (m_icon) {
            p.image({ (m_frame.w - DOCK_ICON) / 2,
                      (m_frame.h - DOCK_ICON) / 2 }, m_icon);
        }
    }

    void on_pointer_enter() override {
        m_hover = true;
        invalidate();
    }

    void on_pointer_leave() override {
        m_hover = false;
        m_pressed = false;
        invalidate();
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

        if (fire) {
            launch_detached(m_path);
        }

        return true;
    }

private:
    stlxgfx_surface_t* m_icon = nullptr;
    const char* m_path = nullptr;
    bool m_hover = false;
    bool m_pressed = false;
};

/* A power choice: a circle with its label beneath, ringed on hover */
class power_orb : public ui::widget {
public:
    power_orb(std::string text, ui::color fill)
        : m_text(std::move(text)), m_fill(fill) {}

    std::function<void()> on_pick;

    ui::size measure(ui::size) override {
        return { ORB_BOX_W, ORB_BOX_H };
    }

    void paint(ui::painter& p) override {
        const ui::theme& t = ui::theme::active();
        ui::point c = { m_frame.w / 2, ORB_R + 8 };

        if (m_hover) {
            p.circle(c, ORB_R + 4, t.text);
        }
        p.circle(c, ORB_R, m_fill);

        ui::size text = p.measure_text(m_text, 0);
        int32_t baseline = 2 * (ORB_R + 8) + 10 + p.font_ascent(0);

        p.text({ (m_frame.w - text.w) / 2, baseline }, m_text, 0, t.text);
    }

    void on_pointer_enter() override {
        m_hover = true;
        invalidate();
    }

    void on_pointer_leave() override {
        m_hover = false;
        invalidate();
    }

    bool on_pointer_down(const ui::pointer_event&) override {
        return true;
    }

    bool on_pointer_up(const ui::pointer_event& e) override {
        bool inside = e.pos.x >= 0 && e.pos.y >= 0 &&
                      e.pos.x < m_frame.w && e.pos.y < m_frame.h;
        if (inside && on_pick) {
            on_pick();
        }

        return true;
    }

private:
    std::string m_text;
    ui::color m_fill;
    bool m_hover = false;
};

/* The dock's power affordance at the right edge */
class power_star : public ui::widget {
public:
    std::function<void()> on_open;

    ui::size measure(ui::size) override {
        return { DOCK_BTN_W, dm_panels::DOCK_H };
    }

    void paint(ui::painter& p) override {
        const ui::theme& t = ui::theme::active();
        ui::point c = { m_frame.w / 2, m_frame.h / 2 };
        ui::color glyph = m_hover ? t.danger : t.text_dim;

        if (m_hover) {
            p.fill({ 0, 0, m_frame.w, m_frame.h }, t.surface_hover);
        }

        /* A power symbol: a ring with a stem through the top */
        p.circle(c, 9, glyph);
        p.circle(c, 5, m_hover ? t.surface_hover : t.surface);
        p.fill({ c.x - 1, c.y - 11, 3, 9 }, glyph);
    }

    void on_pointer_enter() override {
        m_hover = true;
        invalidate();
    }

    void on_pointer_leave() override {
        m_hover = false;
        invalidate();
    }

    bool on_pointer_down(const ui::pointer_event&) override {
        return true;
    }

    bool on_pointer_up(const ui::pointer_event& e) override {
        bool inside = e.pos.x >= 0 && e.pos.y >= 0 &&
                      e.pos.x < m_frame.w && e.pos.y < m_frame.h;
        if (inside && on_open) {
            on_open();
        }

        return true;
    }

private:
    bool m_hover = false;
};

/* Presses that reach the overlay root landed outside the orbs. Its
 * paint stores transparency so vanished hover rings actually erase,
 * which the background skip sentinel would not do. */
class overlay_root : public ui::box {
public:
    std::function<void()> on_outside;

    void paint(ui::painter& p) override {
        p.fill({ 0, 0, m_frame.w, m_frame.h }, 0x00000000);
    }

    bool on_pointer_down(const ui::pointer_event&) override {
        if (on_outside) {
            on_outside();
        }

        return true;
    }
};

} // namespace

static stlxgfx_surface_t* make_band(uint32_t w, int32_t h) {
    return stlxgfx_create_surface(w, static_cast<uint32_t>(h),
                                  32, 16, 8, 0);
}

static void format_clock(char* out, size_t cap) {
    time_t t = time(nullptr);
    uint64_t secs = static_cast<uint64_t>(t);

    snprintf(out, cap, "%02llu:%02llu:%02llu",
             (unsigned long long)(secs / 3600 % 24),
             (unsigned long long)(secs / 60 % 60),
             (unsigned long long)(secs % 60));
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

int dm_panels::init(uint32_t screen_w, uint32_t screen_h) {
    m_width = screen_w;
    m_height = screen_h;
    m_dock_y = static_cast<int32_t>(screen_h) - DOCK_H;

    m_band = make_band(screen_w, BAR_H);
    m_dock = make_band(screen_w, DOCK_H);
    m_overlay = make_band(screen_w, static_cast<int32_t>(screen_h));
    if (!m_band || !m_dock || !m_overlay) {
        return -1;
    }

    /* The top bar: name left, network state and clock right */
    auto bar = std::make_unique<ui::box>(ui::axis::row);
    bar->s().background = ui::theme::active().surface;
    bar->s().padding = ui::edge_insets::xy(12, 0);
    bar->s().align_items = ui::align::center;
    bar->s().gap = 16;

    ui::label* name = bar->add<ui::label>("Stellux");
    name->s().main = ui::length::content();

    ui::box* spacer = bar->add<ui::box>();
    spacer->s().main = ui::length::flex();

    char net[32];
    format_net(net, sizeof(net));
    m_net = bar->add<ui::label>(net);
    m_net->s().main = ui::length::content();
    m_net->set_color(ui::theme::active().text_dim);

    char text[16];
    format_clock(text, sizeof(text));
    m_clock = bar->add<ui::label>(text);
    m_clock->s().main = ui::length::content();
    m_clock->set_color(ui::theme::active().text_dim);

    m_host.set_root(std::move(bar));

    /* The dock: pins centered between flex spacers, power at the
     * right edge where the old desktop kept it */
    auto dock = std::make_unique<ui::box>(ui::axis::row);
    dock->s().background = ui::theme::active().surface;
    dock->s().align_items = ui::align::center;
    dock->s().gap = 6;

    ui::box* lead = dock->add<ui::box>();
    lead->s().main = ui::length::flex();

    for (const dock_pin& pin : DOCK_PINS) {
        stlxgfx_surface_t* icon = stlxgfx_load_bmp(pin.icon_path);
        if (!icon) {
            icon = stlxgfx_load_bmp(DEFAULT_ICON);
        }

        dock_button* btn = dock->add<dock_button>(icon, pin.launch_path);
        btn->s().main = ui::length::fixed(DOCK_BTN_W);
        btn->s().cross = ui::length::fixed(DOCK_H);
    }

    ui::box* tail = dock->add<ui::box>();
    tail->s().main = ui::length::flex();

    power_star* star = dock->add<power_star>();
    star->s().main = ui::length::fixed(DOCK_BTN_W);
    star->s().cross = ui::length::fixed(DOCK_H);
    star->on_open = [this]() {
        if (on_power_request) {
            on_power_request();
        }
    };

    m_dock_host.set_root(std::move(dock));

    /* The overlay: two orbs centered on a transparent ground, the
     * dim itself is composed under the band */
    auto ov = std::make_unique<overlay_root>();
    ov->s().direction = ui::axis::row;
    ov->s().justify = ui::align::center;
    ov->s().align_items = ui::align::center;
    ov->s().gap = 48;
    ov->on_outside = [this]() {
        if (on_power_request) {
            on_power_request();
        }
    };

    power_orb* restart = ov->add<power_orb>(
        "Restart", ui::theme::active().accent);
    restart->s().main = ui::length::fixed(ORB_BOX_W);
    restart->s().cross = ui::length::fixed(ORB_BOX_H);
    restart->on_pick = []() {
        printf("stlxdm: restart chosen\r\n");
        launch_detached("/bin/reboot");
    };

    power_orb* off = ov->add<power_orb>(
        "Shut down", ui::theme::active().danger);
    off->s().main = ui::length::fixed(ORB_BOX_W);
    off->s().cross = ui::length::fixed(ORB_BOX_H);
    off->on_pick = []() {
        printf("stlxdm: shutdown chosen\r\n");
        launch_detached("/bin/shutdown");
    };

    m_overlay_host.set_root(std::move(ov));

    return 0;
}

void dm_panels::shutdown() {
    stlxgfx_destroy_surface(m_band);
    stlxgfx_destroy_surface(m_dock);
    stlxgfx_destroy_surface(m_overlay);
    m_band = nullptr;
    m_dock = nullptr;
    m_overlay = nullptr;
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
        m_dock_host.layout_now(static_cast<int32_t>(m_width), DOCK_H);

        std::vector<ui::rect> out;
        m_dock_host.paint_now(m_dock, out);

        /* Dock rects translate down to the band's screen position */
        for (const ui::rect& r : out) {
            damage.add(r.x, r.y + m_dock_y, r.w, r.h);
        }
    }

    if (m_overlay && m_overlay_open && m_overlay_host.dirty()) {
        m_overlay_host.layout_now(static_cast<int32_t>(m_width),
                                  static_cast<int32_t>(m_height));

        /* The transparent ground must actually clear, since orbs move
         * nowhere and hover rings grow into fresh pixels */
        std::vector<ui::rect> out;
        m_overlay_host.paint_now(m_overlay, out);

        for (const ui::rect& r : out) {
            damage.add(r.x, r.y, r.w, r.h);
        }
    }
}

void dm_panels::compose(stlxgfx_surface_t* back,
                        const damage_list::rect& r) {
    if (m_band && r.y < BAR_H) {
        int32_t x0 = r.x > 0 ? r.x : 0;
        int32_t y0 = r.y > 0 ? r.y : 0;
        int32_t x1 = r.x + r.w < static_cast<int32_t>(m_width)
                   ? r.x + r.w : static_cast<int32_t>(m_width);
        int32_t y1 = r.y + r.h < BAR_H ? r.y + r.h : BAR_H;

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
        int32_t y1 = r.y + r.h < m_dock_y + DOCK_H
                   ? r.y + r.h : m_dock_y + DOCK_H;

        if (x0 < x1 && y0 < y1) {
            stlxgfx_blit(back, x0, y0, m_dock, x0, y0 - m_dock_y,
                         static_cast<uint32_t>(x1 - x0),
                         static_cast<uint32_t>(y1 - y0));
        }
    }
}

void dm_panels::overlay_compose(stlxgfx_surface_t* back,
                                const damage_list::rect& r) {
    if (!m_overlay_open || !m_overlay) {
        return;
    }

    stlxgfx_fill_rect_blend(back, r.x, r.y,
                            static_cast<uint32_t>(r.w),
                            static_cast<uint32_t>(r.h), DIM_COLOR);
    stlxgfx_blit_alpha(back, r.x, r.y, m_overlay, r.x, r.y,
                       static_cast<uint32_t>(r.w),
                       static_cast<uint32_t>(r.h));
}

void dm_panels::overlay_toggle(bool open) {
    if (m_overlay_open == open) {
        return;
    }

    m_overlay_open = open;

    if (open) {
        stlxgfx_clear(m_overlay, 0x00000000);
        m_overlay_host.root()->invalidate_layout();
        return;
    }

    m_overlay_host.pointer_move(-1, -1);
}

void dm_panels::overlay_pointer_move(int32_t x, int32_t y) {
    m_overlay_host.pointer_move(x, y);
}

void dm_panels::overlay_pointer_button(int32_t x, int32_t y, uint8_t btn,
                                       bool down) {
    m_overlay_host.pointer_button(x, y, btn, down);
}

bool dm_panels::overlay_key(uint16_t usage) {
    return usage == 0x29;
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

    char text[16];
    format_clock(text, sizeof(text));
    m_clock->set_text(text);

    /* The network answer changes rarely, so it polls at a slower
     * cadence than the clock */
    m_net_tick++;
    if (m_net && m_net_tick >= NET_POLL_TICKS) {
        m_net_tick = 0;

        char net[32];
        format_net(net, sizeof(net));
        m_net->set_text(net);
    }
}
