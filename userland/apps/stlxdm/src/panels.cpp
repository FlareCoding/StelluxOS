/* The compositor's panels on its own toolkit host. The band surface
 * is retained like a window buffer: the toolkit repaints only dirty
 * widgets into it, and compose blits whatever region damage touches.
 */
#include "panels.hpp"

#include <cstdio>
#include <ctime>

/* The band surface matches the pinned format of every buffer */
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

int dm_panels::init(uint32_t screen_w, uint32_t screen_h) {
    (void)screen_h;

    m_width = screen_w;
    m_band = make_band(screen_w, BAR_H);
    if (!m_band) {
        return -1;
    }

    auto root = std::make_unique<ui::box>(ui::axis::row);
    root->s().background = ui::theme::active().surface;
    root->s().padding = ui::edge_insets::xy(12, 0);
    root->s().align_items = ui::align::center;

    ui::label* name = root->add<ui::label>("Stellux");
    name->s().main = ui::length::content();

    ui::box* spacer = root->add<ui::box>();
    spacer->s().main = ui::length::flex();

    char text[16];
    format_clock(text, sizeof(text));
    m_clock = root->add<ui::label>(text);
    m_clock->s().main = ui::length::content();
    m_clock->set_color(ui::theme::active().text_dim);

    m_host.set_root(std::move(root));

    return 0;
}

void dm_panels::shutdown() {
    stlxgfx_destroy_surface(m_band);
    m_band = nullptr;
}

void dm_panels::flush(damage_list& damage) {
    if (!m_band || !m_host.dirty()) {
        return;
    }

    m_host.layout_now(static_cast<int32_t>(m_width), BAR_H);

    std::vector<ui::rect> out;
    m_host.paint_now(m_band, out);

    /* Band coordinates are already screen coordinates, the bar sits
     * at the origin */
    for (const ui::rect& r : out) {
        damage.add(r.x, r.y, r.w, r.h);
    }
}

void dm_panels::compose(stlxgfx_surface_t* back,
                        const damage_list::rect& r) {
    if (!m_band || r.y >= BAR_H) {
        return;
    }

    int32_t x0 = r.x > 0 ? r.x : 0;
    int32_t y0 = r.y > 0 ? r.y : 0;
    int32_t x1 = r.x + r.w < static_cast<int32_t>(m_width)
               ? r.x + r.w : static_cast<int32_t>(m_width);
    int32_t y1 = r.y + r.h < BAR_H ? r.y + r.h : BAR_H;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    stlxgfx_blit(back, x0, y0, m_band, x0, y0,
                 static_cast<uint32_t>(x1 - x0),
                 static_cast<uint32_t>(y1 - y0));
}

void dm_panels::pointer_move(int32_t x, int32_t y) {
    m_host.pointer_move(x, y);
}

void dm_panels::pointer_button(int32_t x, int32_t y, uint8_t btn,
                               bool down) {
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
}
