#ifndef STLXDM_SHELL_DIRECT_HOST_HPP
#define STLXDM_SHELL_DIRECT_HOST_HPP

#include <stlxui/stlxui.h>

#include <cstdint>
#include <vector>

/* The compositor's private host: a widget tree painted into a band of the screen or into a popover's picture,
 * events fed by the display manager's own router. This is the proof the toolkit does not depend on the protocol. */
class direct_host : public ui::host {
public:
    void flush() override {}

    bool dirty() const { return tree_dirty(); }
    void layout_now(int32_t w, int32_t h) { layout_tree({ w, h }); }
    void paint_now(void* surface, std::vector<ui::rect>& damage_out) {
        paint_tree(surface, damage_out);
    }

    void pointer_move(int32_t x, int32_t y) {
        dispatch_pointer_move({ x, y });
    }

    void pointer_button(int32_t x, int32_t y, uint8_t btn, bool down) {
        dispatch_pointer_button({ x, y }, btn, down);
    }

    /* Offers a key to the focused widget and its parents, true when one took it */
    bool key(const ui::key_event& e, bool down) { return dispatch_key(e, down); }

    /* Focuses the first focusable widget of a tree where none has the focus yet */
    void focus_first() { focus_next(); }

    /* Whether a widget that took a press owns the pointer until its release */
    bool holds_pointer() const { return m_pointer_grab != nullptr; }
};

#endif
