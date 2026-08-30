#include "decor.hpp"
#include "server.hpp"

#include <algorithm>

/* Collects w and its popup descendants in stacking order. */
static void collect_subtree(
    const std::vector<std::unique_ptr<dm_window>>& siblings,
    dm_window* w, std::vector<dm_window*>& out) {
    out.push_back(w);

    for (const auto& child : siblings) {
        if (child->parent_id == w->win_id && child.get() != w) {
            collect_subtree(siblings, child.get(), out);
        }
    }
}

void server::scene_damage_window(dm_window* w) {
    if (w->mapped && w->current >= 0) {
        damage_list::rect r = decor::bounds(*w);
        m_damage.add(r.x, r.y, r.w, r.h);
    }
}

/* A newly mapped toplevel goes on top, a popup goes directly above
 * the top of its parent's subtree. */
void server::scene_map(dm_window* w) {
    if (w->parent_id == 0) {
        m_zorder.push_back(w);
        set_focus(w);
        return;
    }

    dm_client* c = window_owner(w);
    size_t insert_at = m_zorder.size();

    if (c) {
        for (size_t i = 0; i < m_zorder.size(); i++) {
            dm_window* z = m_zorder[i];
            if (z->win_id == w->parent_id ||
                (z->parent_id != 0 && z->parent_id == w->parent_id)) {
                insert_at = i + 1;
            }
        }
    }

    m_zorder.insert(m_zorder.begin() + static_cast<long>(insert_at), w);

    /* Grabbing popups take focus with a restore point, plain popups
     * leave the parent's focus alone */
    if (w->popup_flags & SWP_PF_GRAB) {
        m_grab_popup = w;
        m_focus_restore = m_focus;
        set_focus(w);
    }
}

/* Raising moves the window's whole toplevel subtree to the top,
 * preserving its internal order. */
void server::scene_raise(dm_window* w) {
    dm_client* c = window_owner(w);
    if (!c) {
        return;
    }

    dm_window* root = w;
    while (root->parent_id != 0) {
        dm_window* parent = nullptr;
        for (auto& win : c->windows) {
            if (win->win_id == root->parent_id) {
                parent = win.get();
                break;
            }
        }
        if (!parent) {
            break;
        }

        root = parent;
    }

    std::vector<dm_window*> subtree;
    collect_subtree(c->windows, root, subtree);

    if (!m_zorder.empty() && m_zorder.back() == subtree.back()) {
        return;
    }

    /* Remove subtree members, then append them in stacking order */
    m_zorder.erase(
        std::remove_if(m_zorder.begin(), m_zorder.end(),
                       [&subtree](dm_window* z) {
                           return std::find(subtree.begin(), subtree.end(),
                                            z) != subtree.end();
                       }),
        m_zorder.end());

    for (dm_window* member : subtree) {
        m_zorder.push_back(member);
        scene_damage_window(member);
    }
}
