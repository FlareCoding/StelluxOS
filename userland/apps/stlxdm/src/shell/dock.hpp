#ifndef STLXDM_SHELL_DOCK_HPP
#define STLXDM_SHELL_DOCK_HPP

#include "../damage.hpp"

#include <stlxconf/conf.h>
#include <stlxgfx/surface.h>
#include <stlxui/stlxui.h>

#include <cstdint>
#include <functional>

/* Where the dock lies on an output: its glass shelf of launcher tiles,
 * centered above the bottom edge and empty without launchers, and the
 * band of backdrop rows it paints, the shelf's shadow included */
struct dock_layout {
    damage_list::rect shelf;
    int32_t           band_y = 0;
    int32_t           band_h = 0;
};

dock_layout lay_out_dock(int32_t output_w, int32_t output_h, uint32_t launcher_count);

/* The tile of the launcher at `index` in output pixels */
damage_list::rect dock_tile_rect(const dock_layout& layout, uint32_t index);

/* Whether the path of the launcher `pin` ends with the file name `app_id`, which must not be null. A client must
 * give its executable's name as the app id in its HELLO for its windows to match a launcher. */
bool launches_app(const stlxconf_pin_t& pin, const char* app_id);

/* The icon of `pin`, else the default icon, at the dock's icon size.
 * Null when neither loads or memory runs out. */
stlxgfx_surface_t* load_launcher_icon(const stlxconf_pin_t& pin);

/* The root of the dock's tree: the backdrop's band rows with the shelf, its
 * shadow and its glass drawn over the wallpaper once, and the tiles laid out
 * on the shelf */
class dock_band : public ui::box {
public:
    /* Draws the band from `backdrop`, which must hold the wallpaper in the rows `layout` names, and the shelf on
     * glass from the output's `frost`. `picture()` is null when memory ran out. */
    dock_band(const stlxgfx_surface_t* backdrop, const stlxgfx_surface_t* frost, const dock_layout& layout);
    ~dock_band() override;

    const stlxgfx_surface_t* picture() const { return m_picture; }

    void paint(ui::painter& p) override;

private:
    stlxgfx_surface_t* m_picture = nullptr;
};

/* One launcher: its icon on a tile that lights under the pointer and darkens
 * while pressed, and an amber tick while it is the first launcher of an app
 * with a window, longer while one of that app's windows has the focus */
class dock_tile : public ui::widget {
public:
    /* `icon` and `pin` must outlive the tile. A null `icon` draws the label's first letter, or a question mark when
     * the label is empty. */
    dock_tile(const stlxgfx_surface_t* icon, const stlxconf_pin_t& pin, uint32_t index)
        : m_icon(icon), m_pin(pin), m_index(index) {}

    /* Fired when a press begun on the tile ends with a release over it, for a launcher with a path */
    std::function<void(const stlxconf_pin_t& pin)> on_launch;

    /* Fired when the pointer enters or leaves the tile */
    std::function<void(uint32_t index, bool entered)> on_hover;

    void show_running(bool running, bool focused);

    ui::size measure(ui::size avail) override;
    void paint(ui::painter& p) override;
    void on_pointer_enter() override;
    void on_pointer_leave() override;
    bool on_pointer_down(const ui::pointer_event& e) override;
    bool on_pointer_up(const ui::pointer_event& e) override;

private:
    const stlxgfx_surface_t* m_icon;
    const stlxconf_pin_t&    m_pin;
    uint32_t                 m_index;
    bool                     m_hover = false;
    bool                     m_pressed = false;
    bool                     m_running = false;
    bool                     m_focused = false;
};

#endif
