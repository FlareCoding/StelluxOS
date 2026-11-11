#ifndef STLXDM_SHELL_POPOVER_HPP
#define STLXDM_SHELL_POPOVER_HPP

#include "../damage.hpp"
#include "../layers.hpp"
#include "../picture.hpp"
#include "direct_host.hpp"
#include "panel_picture.hpp"

#include <stlxgfx/surface.h>
#include <stlxui/stlxui.h>

#include <cstdint>
#include <memory>

/* A shell panel shown under the top bar above every window, such as the
 * system menu. It gives the popover host its content and its place, and the
 * host gives it glass, a shadow and the pointer's events. */
class popover_panel {
public:
    virtual ~popover_panel() = default;

    /* The panel's width, and its left edge on an output `output_w` wide */
    virtual int32_t width() const = 0;
    virtual int32_t left(int32_t output_w) const = 0;

    /* The output row the panel's top edge lies on, by default a few pixels under the bar */
    virtual int32_t top() const;

    /* Whether the panel takes the keyboard while open, its first focusable widget focused as it opens, where other
     * panels leave keys to the focused window */
    virtual bool takes_keys() const { return false; }

    /* Builds the content the host shows until the panel closes */
    virtual std::unique_ptr<ui::widget> build() = 0;

    /* Brings live values up to date, once a second while the panel is open */
    virtual void refresh() {}

    /* Runs after the content `build()` gave is gone, when the panel closed or could not be shown */
    virtual void closed() {}

    /* Whether the shell keeps the panel open on Esc, on a press outside both it and the bar's targets, when the
     * pointer enters another target, and after a program starts from it */
    virtual bool pinned() const { return false; }
};

/* The shown panel, a closing one included, as both compositors blend it.
 * `surface` is null while no panel shows. */
struct popover_layer {
    const stlxgfx_surface_t* surface = nullptr;
    twinned_picture*         picture = nullptr; /* the picture a GPU compose reads, null in the CPU's own memory */
    int32_t                  x = 0;             /* with `y`, the output pixel the first pixel lies at */
    int32_t                  y = 0;
    uint32_t                 opacity = 0;       /* the straight alpha pixels' blend opacity, 255 opaque */
};

/* The popover's blend layer on the CPU, bit for bit what compose.comp draws:
 * its straight alpha pixels over `back` inside `clip`, alphas scaled by the opacity */
void blend_popover_layer(stlxgfx_surface_t* back, const damage_list::rect& clip, const popover_layer& shown);

/* The popover as a blend layer of a GPU compose, empty without a picture the GPU reads */
layer_piece popover_layer_piece(const popover_layer& shown);

/* Shows one shell panel at a time above the windows of the main output. Opening a panel draws its shadow and glass
 * into a picture, where the panel's tree then repaints only what changes, so the slide and fade of opening and
 * closing repaint none of the picture, which the compose blends as a layer. */
class popover_host {
public:
    /* Makes pictures in `maker`'s memory from here on, or the CPU's own when null. A shown panel closes at once and
     * its picture drops, since the picture lies in the earlier maker's memory. */
    void use_picture_maker(picture_maker* maker);

    void set_output_width(int32_t width) { m_output_w = width; }

    /* Opens `panel` on glass from the main output's `frost`, first closing any shown panel at once. `panel` must
     * stay alive through the call to its `closed()`. When its picture cannot be made, it logs, drops the content,
     * runs the panel's `closed()` and shows no panel. */
    void open(popover_panel& panel, const stlxgfx_surface_t* frost);

    /* Fades the open panel out */
    void close();

    /* Closes the open panel with its fade already run, so the next flush takes it down before the frame composes */
    void close_now();

    /* The panel open or opening, null while none is or one is closing */
    popover_panel* panel() const;

    /* Whether `flush()` has work: a change of the tree, an animation frame or the region left by a panel closed at
     * once */
    bool dirty() const;

    /* Paints the tree's changes into the picture, advances an animation and
     * adds the output regions that changed to `damage` */
    void flush(damage_list& damage);

    popover_layer layer() const;

    /* Whether the open or opening panel itself, not its shadow, lies under (`x`, `y`). No point lies under a
     * closing panel, so a press during its fade reaches what is beneath it. */
    bool contains(int32_t x, int32_t y) const;

    void pointer_move(int32_t x, int32_t y);
    void pointer_button(int32_t x, int32_t y, uint8_t button, bool down);

    /* Offers a key to the open or opening panel, true when its tree took it */
    bool key(const ui::key_event& e, bool down);

    /* Lets the open panel update its live values, once a second */
    void refresh();

private:
    enum class phase { closed, opening, open, closing };

    void close_at_once();
    void paint_tree(damage_list& damage);
    damage_list::rect bounds() const;

    picture_maker* m_maker = nullptr;
    int32_t        m_output_w = 0;
    popover_panel* m_panel = nullptr;
    direct_host    m_host;
    panel_picture  m_picture;
    int32_t        m_left = 0;
    int32_t        m_top = 0;

    phase    m_phase = phase::closed;
    uint64_t m_phase_start_ns = 0;
    int32_t  m_slide = 0;
    uint32_t m_opacity = 0;

    /* What the last flush showed, and the region a panel closed at once left for the next flush */
    damage_list::rect m_shown = {};
    uint32_t          m_shown_opacity = 0;
    damage_list::rect m_owed = {};
};

#endif
