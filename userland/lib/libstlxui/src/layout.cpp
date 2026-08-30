/* The layout engine: one axis per container, fixed or content or
 * flex sizing, padding, gap, and alignment. Layout staleness always
 * relayouts the whole tree, the damage discipline lives in painting.
 */
#include <stlxui/stlxui.h>

namespace ui {

/* Start or center or end packing inside leftover space */
static int32_t align_offset(align a, int32_t avail, int32_t used) {
    int32_t free_space = avail - used;
    if (free_space <= 0) {
        return 0;
    }

    switch (a) {
    case align::center: return free_space / 2;
    case align::end:    return free_space;
    default:            return 0;
    }
}

void host::layout_tree(size viewport) {
    if (!m_root) {
        return;
    }

    m_root->m_frame = { 0, 0, viewport.w, viewport.h };

    /* Lays out w's children inside its content box, then recurses.
     * w's own frame was set by its parent before this runs. */
    auto lay = [](auto&& self, widget* w) -> void {
        w->m_needs_layout = false;

        const style& st = w->m_style;
        size_t count = w->m_children.size();
        if (count == 0) {
            return;
        }

        const edge_insets& pad = st.padding;
        bool row = st.direction == axis::row;
        int32_t avail_main = row ? w->m_frame.w - pad.left - pad.right
                                 : w->m_frame.h - pad.top - pad.bottom;
        int32_t avail_cross = row ? w->m_frame.h - pad.top - pad.bottom
                                  : w->m_frame.w - pad.left - pad.right;

        /* Pass one sizes inflexible children and sums flex weight. A
         * child's align_self wins unless it is the stretch default,
         * which defers to the container's align_items. Each entry
         * stores main in w and cross in h. */
        std::vector<size> sizes(count);
        int32_t used_main = 0;
        int32_t flex_total = 0;

        for (size_t i = 0; i < count; i++) {
            widget* c = w->m_children[i].get();
            size child_avail = row ? size{ avail_main, avail_cross }
                                   : size{ avail_cross, avail_main };
            size m = c->measure(child_avail);

            int32_t child_main = 0;
            const length& lm = c->m_style.main;
            if (lm.k == length::kind::fixed) {
                child_main = lm.value;
            } else if (lm.k == length::kind::content) {
                child_main = row ? m.w : m.h;
            } else {
                flex_total += lm.value > 0 ? lm.value : 1;
            }

            align eff = c->m_style.align_self != align::stretch
                      ? c->m_style.align_self : st.align_items;
            int32_t child_cross;
            const length& lc = c->m_style.cross;
            if (lc.k == length::kind::fixed) {
                child_cross = lc.value;
            } else if (eff == align::stretch) {
                child_cross = avail_cross;
            } else {
                child_cross = row ? m.h : m.w;
            }

            sizes[i] = { child_main, child_cross };

            if (lm.k != length::kind::flex) {
                used_main += child_main;
            }
        }

        if (count > 1) {
            used_main += st.gap * static_cast<int32_t>(count - 1);
        }

        /* Pass two divides the leftover among flex children, handing
         * the integer remainder out one pixel at a time */
        int32_t free_main = avail_main - used_main;
        if (free_main < 0) {
            free_main = 0;
        }

        if (flex_total > 0) {
            int32_t handed = 0;

            for (size_t i = 0; i < count; i++) {
                widget* c = w->m_children[i].get();
                if (c->m_style.main.k != length::kind::flex) {
                    continue;
                }

                int32_t weight = c->m_style.main.value > 0
                               ? c->m_style.main.value : 1;
                sizes[i].w = free_main * weight / flex_total;
                handed += sizes[i].w;
            }

            /* Floor division leaves a shortfall smaller than the flex
             * count, handed out one pixel at a time in tree order */
            int32_t shortfall = free_main - handed;
            for (size_t i = 0; i < count && shortfall > 0; i++) {
                if (w->m_children[i]->m_style.main.k == length::kind::flex) {
                    sizes[i].w++;
                    shortfall--;
                }
            }
        }

        /* Pass three places children along the axis and recurses.
         * Flex rows have no leftover, so justify only moves packed
         * content. */
        int32_t cursor = row ? pad.left : pad.top;
        if (flex_total == 0) {
            cursor += align_offset(st.justify, avail_main, used_main);
        }

        for (size_t i = 0; i < count; i++) {
            widget* c = w->m_children[i].get();
            int32_t main_px = sizes[i].w;
            int32_t cross_px = sizes[i].h;

            align eff = c->m_style.align_self != align::stretch
                      ? c->m_style.align_self : st.align_items;
            int32_t cross_base = row ? pad.top : pad.left;
            int32_t cross_off = eff == align::stretch
                              ? 0
                              : align_offset(eff, avail_cross, cross_px);

            if (row) {
                c->m_frame = { cursor, cross_base + cross_off,
                               main_px, cross_px };
            } else {
                c->m_frame = { cross_base + cross_off, cursor,
                               cross_px, main_px };
            }

            cursor += main_px + st.gap;

            self(self, c);
        }
    };

    lay(lay, m_root.get());
}

} // namespace ui
