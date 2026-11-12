#ifndef STLXDM_SHELL_CLIPBOARD_PANE_HPP
#define STLXDM_SHELL_CLIPBOARD_PANE_HPP

#include "../clipboard.hpp"
#include "clipboard_images.hpp"
#include "clipboard_search.hpp"
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
    clipboard_images*                      images = nullptr;
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

    /* Asks the searcher which of the texts `ids` names hold `query` past their first 64 KB. Returns the request's
     * number, 0 when the searcher takes none. */
    std::function<uint64_t(const std::string& query, const std::vector<uint64_t>& ids)> search_texts;
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
    bool                     has_text = false;       /* a text representation, which every app pastes */
    std::vector<std::string> files;
    bool                     is_image = false;
    uint32_t                 image_w = 0;
    uint32_t                 image_h = 0;
    uint64_t                 size = 0;
    std::vector<std::string> types;
    shared_picture           thumbnail;              /* an image's, held by whatever paints it */
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

    /* Lists the texts the searcher found holding the query past their first 64 KB, for the newest request only */
    void show_text_matches(const text_matches& matches);

    /* The clipboard's own keys: moving through the entries, Enter, Ctrl Enter, Ctrl P, Delete, Ctrl Shift Delete,
     * Ctrl K, and Esc while Ctrl K's list or the clear question is open. False for any other key. */
    bool on_key(const ui::key_event& e);

    std::vector<footer_item> footer_actions() const;
    std::string footer_end() const;
    std::string placeholder() const;

private:
    void ask_texts();
    void list(uint64_t selected_id);
    bool matches(const shown_entry& entry) const;
    const shown_entry& listed(size_t index) const { return m_entries[m_listed[index]]; }
    void build();
    void build_menu();
    void select(size_t index);
    clipboard_image image_of(const shown_entry& shown) const;
    void move_selection(int32_t rows);
    void choose(bool pastes);
    bool can_paste(const shown_entry& entry) const;
    bool on_menu_key(const ui::key_event& e);
    bool on_question_key(const ui::key_event& e);
    std::vector<clip_action> actions_of(const shown_entry& entry);
    void toggle_pin(uint64_t id, bool is_pinned);
    size_t unpinned_count() const;

    clipboard_source m_source;

    /* Every entry, the query, and the entries it lists as indexes into them, the selected one by its place there */
    std::vector<shown_entry> m_entries;
    std::string              m_query;
    std::vector<size_t>      m_listed;
    size_t                   m_selected = 0;
    clip_list*               m_list = nullptr;
    clip_preview*            m_preview = nullptr;

    /* The newest request to the searcher, whether its answer is still to come, and the texts answers have found */
    uint64_t              m_texts_request = 0;
    bool                  m_is_reading_texts = false;
    std::vector<uint64_t> m_text_matches;

    /* Each entry's row and where it starts in the list's content */
    std::vector<clip_row*> m_rows;
    std::vector<int32_t>   m_row_tops;

    /* Ctrl K's list in place of the entries, its chosen action, the question Clear Unpinned asks first, and the note of
     * a pin the pins' budget refused */
    bool                     m_is_menu_open = false;
    std::vector<clip_action> m_menu;
    size_t                   m_menu_choice = 0;
    bool                     m_is_asking = false;
    bool                     m_is_pin_refused = false;

    stlxgfx_surface_t* m_pin_glyph = nullptr;
};

#endif
