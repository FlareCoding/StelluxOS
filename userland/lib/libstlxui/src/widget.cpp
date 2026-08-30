/* Widget tree mechanics: ownership, invalidation, focus, and the
 * default measure and paint behavior containers inherit. Dirty flags
 * on the widgets are the pending work store, the host derives damage
 * from them at flush, so no state lives anywhere else.
 */
#include <stlxui/stlxui.h>

namespace ui {

void widget::adopt(std::unique_ptr<widget> child) {
    child->m_parent = this;

    /* The whole adopted subtree joins this widget's host */
    std::vector<widget*> stack = { child.get() };
    while (!stack.empty()) {
        widget* w = stack.back();
        stack.pop_back();
        w->m_host = m_host;

        for (auto& c : w->m_children) {
            stack.push_back(c.get());
        }
    }

    m_children.push_back(std::move(child));
    invalidate_layout();
}

void widget::remove(widget* child) {
    auto in_subtree = [](widget* root, widget* target) {
        std::vector<widget*> stack = { root };
        while (!stack.empty()) {
            widget* w = stack.back();
            stack.pop_back();

            if (w == target) {
                return true;
            }

            for (auto& c : w->m_children) {
                stack.push_back(c.get());
            }
        }

        return false;
    };

    for (size_t i = 0; i < m_children.size(); i++) {
        if (m_children[i].get() != child) {
            continue;
        }

        /* Host references into the dying subtree must not dangle */
        if (m_host) {
            if (m_host->m_focus && in_subtree(child, m_host->m_focus)) {
                m_host->m_focus = nullptr;
            }

            if (m_host->m_hover && in_subtree(child, m_host->m_hover)) {
                m_host->m_hover = nullptr;
            }

            if (m_host->m_pointer_grab &&
                in_subtree(child, m_host->m_pointer_grab)) {
                m_host->m_pointer_grab = nullptr;
            }
        }

        m_children.erase(m_children.begin() + static_cast<long>(i));
        invalidate_layout();

        return;
    }
}

void widget::invalidate() {
    m_needs_paint = true;
}

/* Content sizing makes ancestor geometry depend on descendants, so
 * staleness walks to the root and the host relayouts the whole tree */
void widget::invalidate_layout() {
    for (widget* w = this; w != nullptr; w = w->m_parent) {
        w->m_needs_layout = true;
    }

    invalidate();
}

void widget::focus() {
    if (!focusable() || !m_host || m_host->m_focus == this) {
        return;
    }

    widget* old = m_host->m_focus;
    m_host->m_focus = this;

    if (old) {
        old->on_focus_out();
        old->invalidate();
    }

    on_focus_in();
    invalidate();
}

bool widget::focused() const {
    return m_host != nullptr && m_host->m_focus == this;
}

/* The default content size wraps the children along the flow axis,
 * fixed children contributing their fixed size and flex children
 * nothing, since flex only divides a parent's leftover space */
size widget::measure(size avail) {
    const edge_insets& pad = m_style.padding;
    bool row = m_style.direction == axis::row;
    int32_t avail_main;
    int32_t avail_cross;

    if (row) {
        avail_main = avail.w - pad.left - pad.right;
        avail_cross = avail.h - pad.top - pad.bottom;
    } else {
        avail_main = avail.h - pad.top - pad.bottom;
        avail_cross = avail.w - pad.left - pad.right;
    }

    int32_t main_sum = 0;
    int32_t cross_max = 0;
    int32_t count = 0;

    for (auto& c : m_children) {
        size child_avail = row ? size{ avail_main, avail_cross }
                               : size{ avail_cross, avail_main };
        size m = c->measure(child_avail);

        int32_t child_main = row ? m.w : m.h;
        int32_t child_cross = row ? m.h : m.w;

        if (c->s().main.k == length::kind::fixed) {
            child_main = c->s().main.value;
        } else if (c->s().main.k == length::kind::flex) {
            child_main = 0;
        }

        if (c->s().cross.k == length::kind::fixed) {
            child_cross = c->s().cross.value;
        }

        main_sum += child_main;
        if (child_cross > cross_max) {
            cross_max = child_cross;
        }

        count++;
    }

    if (count > 1) {
        main_sum += m_style.gap * (count - 1);
    }

    if (row) {
        return { main_sum + pad.left + pad.right,
                 cross_max + pad.top + pad.bottom };
    }

    return { cross_max + pad.left + pad.right,
             main_sum + pad.top + pad.bottom };
}

void widget::paint(painter& p) {
    if (m_style.background != 0) {
        p.fill({ 0, 0, m_frame.w, m_frame.h }, m_style.background);
    }
}

bool widget::on_pointer_down(const pointer_event&) { return false; }
bool widget::on_pointer_up(const pointer_event&) { return false; }
bool widget::on_pointer_move(const pointer_event&) { return false; }
void widget::on_pointer_enter() {}
void widget::on_pointer_leave() {}
bool widget::on_scroll(const pointer_event&) { return false; }
bool widget::on_key_down(const key_event&) { return false; }
bool widget::on_key_up(const key_event&) { return false; }

} // namespace ui
