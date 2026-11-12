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

/* The image entries' 32 px thumbnails, each made once and kept across openings while its entry lasts */
class clipboard_thumbnails {
public:
    clipboard_thumbnails() = default;
    ~clipboard_thumbnails();
    clipboard_thumbnails(const clipboard_thumbnails&) = delete;
    clipboard_thumbnails& operator=(const clipboard_thumbnails&) = delete;

    /* The thumbnail of an entry's PNG, made at the first ask, null when it cannot be decoded */
    const stlxgfx_surface_t* of(const clipboard_entry& entry);

    /* Drops the thumbnails of entries the history no longer keeps */
    void keep(const std::vector<const clipboard_entry*>& entries);

private:
    struct thumbnail {
        uint64_t           id;
        stlxgfx_surface_t* picture;
    };

    std::vector<thumbnail> m_thumbnails;
};

/* What Clipboard mode reads and does: the history, the icons its entries show, where Enter pastes, and the actions
 * the server runs on an entry */
struct clipboard_source {
    const clipboard_history*               history = nullptr;
    clipboard_thumbnails*                  thumbnails = nullptr;
    const stlxconf_t*                      conf = nullptr;
    const std::vector<stlxgfx_surface_t*>* launcher_icons = nullptr;
    const stlxgfx_surface_t*               text_icon = nullptr;
    const stlxgfx_surface_t*               image_icon = nullptr;
    const stlxgfx_surface_t*               folder_icon = nullptr;
    const stlxgfx_surface_t*               program_icon = nullptr;
    std::string                            focused_app;  /* empty with no window focused */
    std::string                            since;        /* when the history began, such as 09:58 */

    std::function<void(uint64_t id)> paste;
    std::function<void(uint64_t id)> copy;
    std::function<void(uint64_t id, bool pinned)> pin;
    std::function<void(uint64_t id)> remove;
    std::function<void()> clear;
};

/* One thing an entry's Ctrl K list does, with the keys that run it, space separated */
struct clip_action {
    std::string           label;
    std::string           keys;
    std::function<void()> run;
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
    bool                     is_too_large = false;   /* current, and past the whole budget alone */
    std::string              text;                   /* the first 64 KB of its text */
    uint64_t                 text_len = 0;
    std::vector<std::string> files;
    bool                     is_image = false;
    uint32_t                 image_w = 0;
    uint32_t                 image_h = 0;
    uint64_t                 size = 0;
    std::vector<std::string> types;
    const stlxgfx_surface_t* icon = nullptr;
};

/* Clipboard mode's body: the history's entries, pinned ones first, beside an exact preview of the selected one */
class clipboard_pane : public ui::box {
public:
    explicit clipboard_pane(clipboard_source source);
    ~clipboard_pane() override;

    /* Reads the history again, keeping the selected entry selected while it is still listed, else the one in its
     * place */
    void refresh();

    /* Lists only the entries whose text or file paths hold `query` in any case, the first selected, and every entry
     * while it is empty */
    void search(std::string query);

    /* The clipboard's own keys: moving through the entries, Enter, Ctrl Enter, Ctrl P, Delete, Ctrl Shift Delete,
     * Ctrl K, and Esc while Ctrl K's list or the clear question is open. False for any other key. */
    bool on_key(const ui::key_event& e);

    std::vector<footer_item> footer_actions() const;
    std::string footer_end() const;
    std::string placeholder() const;

private:
    void list(uint64_t selected_id);
    bool matches(const shown_entry& entry) const;
    const shown_entry& listed(size_t index) const { return m_entries[m_listed[index]]; }
    void build();
    void build_menu();
    void select(size_t index);
    stlxgfx_surface_t* decode_image(const shown_entry& shown) const;
    void move_selection(int32_t rows);
    void choose(bool pastes);
    bool on_menu_key(const ui::key_event& e);
    bool on_question_key(const ui::key_event& e);
    std::vector<clip_action> actions_of(const shown_entry& entry);
    size_t unpinned_count() const;

    clipboard_source m_source;

    /* Every entry, the query, and the entries it lists as indexes into them, the selected one by its place there */
    std::vector<shown_entry> m_entries;
    std::string              m_query;
    std::vector<size_t>      m_listed;
    size_t                   m_selected = 0;
    clip_list*               m_list = nullptr;
    clip_preview*            m_preview = nullptr;

    /* Each entry's row and where it starts in the list's content */
    std::vector<clip_row*> m_rows;
    std::vector<int32_t>   m_row_tops;

    /* Ctrl K's list in place of the entries, its chosen action, and the question Clear Unpinned asks first */
    bool                     m_is_menu_open = false;
    std::vector<clip_action> m_menu;
    size_t                   m_menu_choice = 0;
    bool                     m_is_asking = false;

    stlxgfx_surface_t* m_pin_glyph = nullptr;
};

#endif
