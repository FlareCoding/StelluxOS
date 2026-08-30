/* The host core: tree ownership, dirty subtree repaint with damage
 * accumulation, hit test then bubble dispatch, hover pairs, pointer
 * grab, and tab focus. Layout lives in layout.cpp.
 */
#include <stlxui/stlxui.h>

#include <stlxgfx/surface.h>

namespace ui {

/* Window space position of a widget, the sum of ancestor frames */
static point window_pos_of(const rect& frame, point parent_origin) {
    return { parent_origin.x + frame.x, parent_origin.y + frame.y };
}

static rect intersect_rects(const rect& a, const rect& b) {
    int32_t x0 = a.x > b.x ? a.x : b.x;
    int32_t y0 = a.y > b.y ? a.y : b.y;
    int32_t x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int32_t y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;

    if (x1 <= x0 || y1 <= y0) {
        return { 0, 0, 0, 0 };
    }

    return { x0, y0, x1 - x0, y1 - y0 };
}

void host::set_root(std::unique_ptr<widget> root) {
    m_focus = nullptr;
    m_hover = nullptr;
    m_pointer_grab = nullptr;
    m_root = std::move(root);

    if (!m_root) {
        return;
    }

    /* The whole tree binds to this host */
    std::vector<widget*> stack = { m_root.get() };
    while (!stack.empty()) {
        widget* w = stack.back();
        stack.pop_back();
        w->m_host = this;

        for (auto& c : w->m_children) {
            stack.push_back(c.get());
        }
    }

    m_root->invalidate_layout();
}

/* Collects the window space frames of invalidated widgets, then
 * repaints every widget intersecting them, clipped per damage rect,
 * so one dirty button repaints one button sized region */
void host::paint_tree(void* surface, std::vector<rect>& damage_out) {
    if (!m_root || !surface) {
        return;
    }

    auto collect = [&](auto&& self, widget* w, point origin) -> void {
        point pos = window_pos_of(w->m_frame, origin);

        if (w->m_needs_paint) {
            damage_out.push_back({ pos.x, pos.y,
                                   w->m_frame.w, w->m_frame.h });
            w->m_needs_paint = false;
        }

        for (auto& c : w->m_children) {
            self(self, c.get(), pos);
        }
    };
    collect(collect, m_root.get(), { 0, 0 });

    if (damage_out.empty()) {
        return;
    }

    /* Painting walks top down per rect, ancestors first so children
     * draw over their parents' backgrounds */
    auto paint_rect = [&](auto&& self, widget* w, point origin,
                          painter& p, const rect& clip_to) -> void {
        point pos = window_pos_of(w->m_frame, origin);
        rect abs = { pos.x, pos.y, w->m_frame.w, w->m_frame.h };
        rect visible = intersect_rects(abs, clip_to);
        if (visible.w <= 0 || visible.h <= 0) {
            return;
        }

        p.m_origin = pos;
        p.m_clips.push_back(visible);
        w->paint(p);
        p.m_clips.pop_back();

        for (auto& c : w->m_children) {
            self(self, c.get(), pos, p, visible);
        }
    };

    for (const rect& r : damage_out) {
        painter p;
        p.m_target = surface;

        paint_rect(paint_rect, m_root.get(), { 0, 0 }, p, r);
    }
}

widget* host::hit_at(point p, point* out_local) {
    if (!m_root || !m_root->m_frame.contains(p)) {
        return nullptr;
    }

    /* Later siblings paint later, so they win where frames overlap */
    auto walk = [out_local](auto&& self, widget* w, point l) -> widget* {
        for (size_t i = w->m_children.size(); i-- > 0;) {
            widget* c = w->m_children[i].get();
            if (c->m_frame.contains(l)) {
                return self(self, c,
                            point{ l.x - c->m_frame.x, l.y - c->m_frame.y });
            }
        }

        *out_local = l;
        return w;
    };

    widget* hit = walk(walk, m_root.get(),
                       point{ p.x - m_root->m_frame.x,
                              p.y - m_root->m_frame.y });
    return hit;
}

void host::dispatch_pointer_move(point p) {
    /* A pressed widget owns motion until release, hover untouched */
    if (m_pointer_grab) {
        point l = p;
        for (widget* a = m_pointer_grab; a != nullptr; a = a->m_parent) {
            l.x -= a->m_frame.x;
            l.y -= a->m_frame.y;
        }

        m_pointer_grab->on_pointer_move({ l, 0, 0 });
        return;
    }

    point local;
    widget* target = hit_at(p, &local);

    /* Leaf level enter and leave pairs, the way hover states expect */
    if (target != m_hover) {
        if (m_hover) {
            m_hover->on_pointer_leave();
        }
        m_hover = target;
        if (target) {
            target->on_pointer_enter();
        }
    }

    point l = local;
    for (widget* w = target; w != nullptr; w = w->m_parent) {
        if (w->on_pointer_move({ l, 0, 0 })) {
            return;
        }

        l.x += w->m_frame.x;
        l.y += w->m_frame.y;
    }
}

void host::dispatch_pointer_button(point p, uint8_t button, bool down) {
    point local;
    widget* target = hit_at(p, &local);

    if (down) {
        /* The consumer of the press owns the pointer until release */
        point l = local;
        for (widget* w = target; w != nullptr; w = w->m_parent) {
            if (w->on_pointer_down({ l, button, 0 })) {
                m_pointer_grab = w;
                return;
            }

            l.x += w->m_frame.x;
            l.y += w->m_frame.y;
        }
        return;
    }

    if (m_pointer_grab) {
        widget* grab = m_pointer_grab;
        m_pointer_grab = nullptr;

        point l = p;
        for (widget* a = grab; a != nullptr; a = a->m_parent) {
            l.x -= a->m_frame.x;
            l.y -= a->m_frame.y;
        }

        grab->on_pointer_up({ l, button, 0 });
        return;
    }

    point l = local;
    for (widget* w = target; w != nullptr; w = w->m_parent) {
        if (w->on_pointer_up({ l, button, 0 })) {
            return;
        }

        l.x += w->m_frame.x;
        l.y += w->m_frame.y;
    }
}

void host::dispatch_scroll(point p, int16_t dy) {
    point local;
    widget* target = hit_at(p, &local);

    point l = local;
    for (widget* w = target; w != nullptr; w = w->m_parent) {
        if (w->on_scroll({ l, 0, dy })) {
            return;
        }

        l.x += w->m_frame.x;
        l.y += w->m_frame.y;
    }
}

/* Keys go to the focus and bubble, an unconsumed tab cycles focus */
void host::dispatch_key(const key_event& e, bool down) {
    for (widget* w = m_focus; w != nullptr; w = w->m_parent) {
        bool consumed = down ? w->on_key_down(e) : w->on_key_up(e);
        if (consumed) {
            return;
        }
    }

    if (down && e.ch == '\t') {
        focus_next();
    }
}

void host::invalidate_rects(const std::vector<rect>& rects) {
    if (!m_root || rects.empty()) {
        return;
    }

    auto walk = [&rects](auto&& self, widget* w, point origin) -> void {
        point pos = window_pos_of(w->m_frame, origin);
        rect abs = { pos.x, pos.y, w->m_frame.w, w->m_frame.h };

        for (const rect& r : rects) {
            rect hit = intersect_rects(abs, r);
            if (hit.w > 0 && hit.h > 0) {
                w->m_needs_paint = true;
                break;
            }
        }

        for (auto& c : w->m_children) {
            self(self, c.get(), pos);
        }
    };

    walk(walk, m_root.get(), { 0, 0 });
}

bool host::tree_dirty() const {
    if (!m_root) {
        return false;
    }

    std::vector<const widget*> stack = { m_root.get() };
    while (!stack.empty()) {
        const widget* w = stack.back();
        stack.pop_back();

        if (w->m_needs_paint || w->m_needs_layout) {
            return true;
        }

        for (const auto& c : w->m_children) {
            stack.push_back(c.get());
        }
    }

    return false;
}

void host::focus_next() {
    std::vector<widget*> focusables;
    if (m_root) {
        auto collect = [&](auto&& self, widget* w) -> void {
            if (w->focusable()) {
                focusables.push_back(w);
            }

            for (auto& c : w->m_children) {
                self(self, c.get());
            }
        };
        collect(collect, m_root.get());
    }

    if (focusables.empty()) {
        return;
    }

    size_t next = 0;
    for (size_t i = 0; i < focusables.size(); i++) {
        if (focusables[i] == m_focus) {
            next = (i + 1) % focusables.size();
            break;
        }
    }

    focusables[next]->focus();
}

} // namespace ui
