#ifndef STLXDM_SHELL_CLIPBOARD_PANE_HPP
#define STLXDM_SHELL_CLIPBOARD_PANE_HPP

#include "../clipboard.hpp"
#include "palette_parts.hpp"

#include <stlxconf/conf.h>
#include <stlxgfx/surface.h>
#include <stlxui/stlxui.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

/* What Clipboard mode reads and does: the history, the icons its entries show, where Enter pastes, and the actions
 * the server runs on an entry */
struct clipboard_source {
    const clipboard_history*               history = nullptr;
    const stlxconf_t*                      conf = nullptr;
    const std::vector<stlxgfx_surface_t*>* launcher_icons = nullptr;
    const stlxgfx_surface_t*               text_icon = nullptr;
    std::string                            focused_app;  /* empty with no window focused */
    std::string                            since;        /* when the history began, such as 09:58 */

    std::function<void(uint64_t id)> paste;
    std::function<void(uint64_t id)> copy;
};

class clip_list;
class clip_preview;
class clip_row;

/* What one entry shows, read from the history once per change */
struct shown_entry {
    uint64_t                 id = 0;
    std::string              kind;
    std::string              title;
    bool                     is_title_text = false;  /* the entry's own text, so it is drawn in Plex Mono */
    std::string              from;
    int64_t                  copied_ns = 0;
    bool                     is_current = false;
    bool                     is_pinned = false;
    std::string              text;                   /* the first 64 KB of its text */
    uint64_t                 text_len = 0;
    uint64_t                 size = 0;
    std::vector<std::string> types;
    const stlxgfx_surface_t* icon = nullptr;
};

/* Clipboard mode's body: the history's entries, pinned ones first, beside an exact preview of the selected one */
class clipboard_pane : public ui::box {
public:
    explicit clipboard_pane(clipboard_source source);

    /* Reads the history again, keeping the selected entry selected while it is still listed */
    void refresh();

    /* The clipboard's own keys: moving through the entries, Enter and Ctrl Enter. False for any other key. */
    bool on_key(const ui::key_event& e);

    std::vector<footer_item> footer_actions() const;
    std::string footer_end() const;
    std::string placeholder() const;

private:
    void build();
    void select(size_t index);
    void move_selection(int32_t rows);
    void choose(bool pastes);

    clipboard_source         m_source;
    std::vector<shown_entry> m_entries;
    size_t                   m_selected = 0;
    clip_list*               m_list = nullptr;
    clip_preview*            m_preview = nullptr;

    /* Each entry's row and where it starts in the list's content */
    std::vector<clip_row*> m_rows;
    std::vector<int32_t>   m_row_tops;
};

#endif
