/* The window host: a tree bound to an stlxwin window. Flush lays out
 * against the buffer the compositor sized, paints the dirty subtrees,
 * brings the swapped in buffer up to date with the previous frame's
 * damage, and commits exactly what changed.
 */
#include <stlxui/stlxui.h>

#include <stlxgfx/surface.h>
#include <stlxwin/proto.h>
#include <stlxwin/stlxwin.h>

namespace ui {

window_host::window_host(app& a) : m_app(&a) {}

window_host::~window_host() {
    if (m_win) {
        stlxwin_window_destroy(m_win);
    }
}

void window_host::flush() {
    if (!m_win || !m_root || !tree_dirty()) {
        return;
    }

    stlxwin_buffer* buf = stlxwin_begin_frame(m_win);
    if (!buf) {
        return;
    }

    /* A size change or a never seen slot repaints everything, since
     * fresh buffers hold nothing at all. Otherwise layout runs
     * unconditionally and unchanged geometry produces no damage. */
    bool resized = buf->width != m_last_w || buf->height != m_last_h;
    if (resized) {
        m_grounded[0] = nullptr;
        m_grounded[1] = nullptr;
    }

    bool fresh = buf->pixels != m_grounded[0] &&
                 buf->pixels != m_grounded[1];
    layout_tree({ static_cast<int32_t>(buf->width),
                  static_cast<int32_t>(buf->height) });
    if (resized || fresh) {
        m_root->invalidate();
        m_last_damage.clear();
    } else if (buf->pixels != m_last_pixels) {
        /* The other grounded slot missed exactly the previous frame */
        invalidate_rects(m_last_damage);
    }

    stlxgfx_surface_t* s = stlxgfx_surface_from_buffer(
        reinterpret_cast<uint8_t*>(buf->pixels), buf->width, buf->height,
        buf->stride, 32, 16, 8, 0);
    if (!s) {
        return;
    }

    std::vector<rect> damage;
    paint_tree(s, damage);
    stlxgfx_destroy_surface(s);

    if (damage.empty()) {
        return;
    }

    /* Wire rects cap at the protocol limit, overflow commits full */
    stlxwin_rect rects[SWP_COMMIT_MAX_RECTS];
    uint32_t n = 0;
    bool full = false;
    for (const rect& r : damage) {
        if (n == SWP_COMMIT_MAX_RECTS) {
            full = true;
            break;
        }

        rects[n].x = r.x;
        rects[n].y = r.y;
        rects[n].w = r.w;
        rects[n].h = r.h;
        n++;
    }

    if (full) {
        stlxwin_commit(m_win, buf, nullptr, 0, 0);
    } else {
        stlxwin_commit(m_win, buf, rects, n, 0);
    }

    m_last_pixels = buf->pixels;
    m_last_w = buf->width;
    m_last_h = buf->height;
    m_last_damage = damage;

    if (buf->pixels != m_grounded[0] && buf->pixels != m_grounded[1]) {
        m_grounded[m_grounded[0] ? 1 : 0] = buf->pixels;
    }
}

} // namespace ui
