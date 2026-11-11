#ifndef STLXDM_SHELL_KEYCAP_HPP
#define STLXDM_SHELL_KEYCAP_HPP

#include "draw.hpp"

#include <stlxconf/conf.h>
#include <stlxgfx/surface.h>
#include <stlxui/stlxui.h>

#include <cstdint>
#include <string>
#include <vector>

constexpr int32_t KEYCAP_HEIGHT = 18;
constexpr int32_t KEYCAP_GAP = 3;

/* One key of a shortcut, as a chip with the key's name */
class keycap {
public:
    explicit keycap(const std::string& name);
    ~keycap();

    keycap(const keycap&) = delete;
    keycap& operator=(const keycap&) = delete;

    int32_t width() const { return m_width; }

    void set_hover(bool hover);
    void paint(ui::painter& p, int32_t x, int32_t top) const;

private:
    text_picture       m_text;
    int32_t            m_width;
    stlxgfx_surface_t* m_ring;
    stlxgfx_surface_t* m_hover_ring;
    bool               m_hover = false;
};

/* The keys of the first live hotkey that runs the same thing, a program with the same args or the same action of
 * stlxdm's own, none when no hotkey does. A hotkey is live when its chord parses, as the server binds them. */
std::vector<std::string> shortcut_keys(const stlxconf_t& conf, stlxconf_action_kind_t kind, const char* path,
                                       const char* args);

#endif
