#include "quick_actions.hpp"

#include "chord.hpp"
#include "dock.hpp"
#include "draw.hpp"
#include "keycap.hpp"
#include "programs.hpp"
#include "quick_calc.hpp"
#include "quick_match.hpp"

#include <stlxtheme/theme.h>

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>

constexpr int32_t PALETTE_WIDTH = 800;
constexpr int32_t PALETTE_TOP = 152;
constexpr int32_t PALETTE_PADDING = 4;

constexpr int32_t FIELD_HEIGHT = 48;
constexpr int32_t FIELD_GLYPH_LEFT = 15;
constexpr int32_t FIELD_GLYPH_SIZE = 16;
constexpr int32_t FIELD_TEXT_LEFT = FIELD_GLYPH_LEFT + FIELD_GLYPH_SIZE + 12;
constexpr int32_t FIELD_PADDING_RIGHT = 12;
constexpr float QUERY_SIZE = 17.5f;

constexpr int32_t HEAD_HEIGHT = 28;
constexpr int32_t HEAD_PADDING_X = 10;
constexpr int32_t HEAD_PADDING_BOTTOM = 5;

constexpr int32_t ROW_HEIGHT = 44;
constexpr int32_t ROW_WIDTH = PALETTE_WIDTH - 2 * PALETTE_PADDING;
constexpr int32_t ROW_PADDING_LEFT = 8;
constexpr int32_t ROW_PADDING_RIGHT = 10;
constexpr int32_t ROW_GAP = 12;
constexpr int32_t ICON_SIZE = STLXTHEME_DOCK_ICON_SIZE;
constexpr int32_t TEXT_LEFT = ROW_PADDING_LEFT + ICON_SIZE + ROW_GAP;
constexpr int32_t LABEL_LINE_HEIGHT = 18;
constexpr int32_t SUBTITLE_LINE_HEIGHT = 16;
constexpr int32_t MARK_RADIUS = 2;

/* A running app's tick, the dock's, under the row's icon */
constexpr int32_t TICK_LEFT = 12;
constexpr int32_t TICK_DROP = 3;
constexpr int32_t TICK_WIDTH = 8;
constexpr int32_t TICK_HEIGHT = 2;
constexpr int32_t TICK_RADIUS = 1;

/* The list holds two section heads and eight rows before it scrolls, and shows six rows of a kind at most */
constexpr int32_t LIST_GAP = 4;
constexpr int32_t LIST_HEIGHT = 2 * HEAD_HEIGHT + 8 * ROW_HEIGHT;
constexpr size_t KIND_ROWS_MAX = 6;
constexpr size_t RECENT_MAX = 3;
constexpr uint32_t UNPICKED = UINT32_MAX;
constexpr int32_t PAGE_ROWS = 8;
constexpr int32_t NO_MATCH_PADDING_X = 40;

constexpr int32_t FOOTER_HEIGHT = 36;
constexpr int32_t FOOTER_PADDING_LEFT = 14;
constexpr int32_t FOOTER_PADDING_RIGHT = 10;
constexpr int32_t FOOTER_KEY_GAP = 6;

constexpr uint16_t HID_USAGE_ENTER = 0x28;
constexpr uint16_t HID_USAGE_BACKSPACE = 0x2A;
constexpr uint16_t HID_USAGE_TAB = 0x2B;
constexpr uint16_t HID_USAGE_HOME = 0x4A;
constexpr uint16_t HID_USAGE_PAGE_UP = 0x4B;
constexpr uint16_t HID_USAGE_DELETE = 0x4C;
constexpr uint16_t HID_USAGE_END = 0x4D;
constexpr uint16_t HID_USAGE_PAGE_DOWN = 0x4E;
constexpr uint16_t HID_USAGE_RIGHT = 0x4F;
constexpr uint16_t HID_USAGE_LEFT = 0x50;
constexpr uint16_t HID_USAGE_DOWN = 0x51;
constexpr uint16_t HID_USAGE_UP = 0x52;

constexpr uint8_t COMMAND_MODS = CHORD_MOD_CTRL | CHORD_MOD_ALT | CHORD_MOD_SUPER;

constexpr std::string_view PLACEHOLDER = "Search apps, actions and programs";
constexpr std::string_view ELLIPSIS = "\xE2\x80\xA6";

constexpr const char* PROGRAM_DIRS[] = { "/bin", "/usr/bin" };
constexpr const char* PROGRAM_ICON_PATH = "/etc/res/icons/doc-exec-32.png";

/* Power... asks before restarting or shutting down, so the programs that do it at once, and init, stay unlisted */
constexpr const char* UNLISTED_PROGRAMS[] = { "init", "reboot", "shutdown" };

namespace {

/* The kinds rank in this order where matches are equally good, and a calculation heads the list before any */
enum class row_kind : uint8_t { app, action, program, calculator };

constexpr const char* KIND_HEADS[] = { "APPS", "ACTIONS", "PROGRAMS", "CALCULATOR" };
constexpr const char* KIND_NAMES[] = { "Application", "Action", "Program", "Calculation" };
constexpr const char* KIND_VERBS[] = { "Open", "Run", "Run in Terminal", "Copy" };

/* One thing the palette starts, as its row shows it */
struct palette_entry {
    row_kind                 kind = row_kind::app;
    uint32_t                 index = 0; /* into the configuration's launchers or actions */
    std::string              program;   /* with `args`, what a program's row runs in Terminal */
    std::string              args;
    std::string              copied;    /* what a calculation's row copies */
    std::string              label;
    std::string              subtitle;
    bool                     is_subtitle_path = false; /* drawn in Plex Mono */
    std::string              keywords;
    const stlxgfx_surface_t* icon = nullptr; /* null draws `tile` on a tile, else the label's first letter */
    std::string              tile;
    std::vector<std::string> keys;           /* of a shortcut that runs the same */
    bool                     running = false;
    uint32_t                 recent = UNPICKED; /* its place among this session's picks, newest first */
};

/* What a pick is remembered by across openings: the launcher or action it is, or the program it runs */
std::string pick_key(const palette_entry& entry) {
    switch (entry.kind) {
    case row_kind::app:
        return "app " + std::to_string(entry.index);
    case row_kind::action:
        return "action " + std::to_string(entry.index);
    case row_kind::program:
        return "program " + entry.program;
    case row_kind::calculator:
        break;
    }

    return {};
}

/* The bytes of `text` that fit `max_w` pixels in the face, cut at a character boundary with room left for an
 * ellipsis when the whole does not fit */
size_t fitting_length(const std::string& text, const char* face_path, float em_px, int32_t max_w) {
    stlxgfx_font* font = shell_font(face_path, em_px);
    if (!font || stlxgfx_text_width(font, text.data(), text.size()) <= max_w) {
        return text.size();
    }

    int32_t ellipsis_w = stlxgfx_text_width(font, ELLIPSIS.data(), ELLIPSIS.size());
    size_t end = text.size();
    while (end > 0) {
        end--;
        while (end > 0 && (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80) {
            end--;
        }

        if (stlxgfx_text_width(font, text.data(), end) + ellipsis_w <= max_w) {
            break;
        }
    }

    return end;
}

/* `text` as it fits `max_w` pixels in the face, ending in an ellipsis when cut */
std::string fitted(const std::string& text, const char* face_path, float em_px, int32_t max_w) {
    size_t length = fitting_length(text, face_path, em_px, max_w);
    return length == text.size() ? text : text.substr(0, length) + std::string(ELLIPSIS);
}

std::string_view trimmed(std::string_view text) {
    size_t first = text.find_first_not_of(' ');
    if (first == std::string_view::npos) {
        return {};
    }

    size_t last = text.find_last_not_of(' ');
    return text.substr(first, last - first + 1);
}

const char* file_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

const char* subtitle_face(const palette_entry& entry) {
    return entry.is_subtitle_path ? STLXTHEME_MONO_FONT_PATH : STLXTHEME_UI_FONT_PATH;
}

/* The search field, always focused while the palette is open, so it always shows the amber rule and the caret */
class query_field : public ui::widget {
public:
    explicit query_field(const stlxgfx_surface_t* glyph) : m_glyph(glyph) {}

    void show(const std::string& text, size_t cursor) {
        m_text = text;
        m_cursor = cursor;
        invalidate();
    }

    ui::size measure(ui::size avail) override {
        return { avail.w, FIELD_HEIGHT };
    }

    void paint(ui::painter& p) override;

private:
    const stlxgfx_surface_t* m_glyph;
    std::string              m_text;
    size_t                   m_cursor = 0;
};

void query_field::paint(ui::painter& p) {
    p.rounded_rect({ 1, 1, m_frame.w - 2, m_frame.h - 2 }, STLXTHEME_CONTROL_RADIUS - 1, STLXTHEME_RECESS);
    p.rounded_stroke({ 0, 0, m_frame.w, m_frame.h }, STLXTHEME_CONTROL_RADIUS, STLXTHEME_LINE_STRONG);
    p.fill({ STLXTHEME_CONTROL_RADIUS, m_frame.h - 1 - STLXTHEME_FOCUS_RULE_WIDTH,
             m_frame.w - 2 * STLXTHEME_CONTROL_RADIUS, STLXTHEME_FOCUS_RULE_WIDTH }, STLXTHEME_ACCENT);

    if (m_glyph) {
        p.image({ FIELD_GLYPH_LEFT, (m_frame.h - FIELD_GLYPH_SIZE) / 2 }, m_glyph);
    }

    bool is_empty = m_text.empty();
    std::string_view shown = is_empty ? PLACEHOLDER : std::string_view(m_text);
    ui::size line = p.measure_text(shown, QUERY_SIZE);
    int32_t top = (m_frame.h - line.h) / 2;
    ui::size before_caret = p.measure_text(std::string_view(m_text).substr(0, m_cursor), QUERY_SIZE);

    p.push_clip({ FIELD_TEXT_LEFT, 0, m_frame.w - FIELD_TEXT_LEFT - FIELD_PADDING_RIGHT, m_frame.h });
    p.text({ FIELD_TEXT_LEFT, top + p.font_ascent(QUERY_SIZE) }, shown, QUERY_SIZE,
           is_empty ? STLXTHEME_TEXT_DIM : STLXTHEME_TEXT);
    p.fill({ FIELD_TEXT_LEFT + before_caret.w, top, 1, line.h }, STLXTHEME_TEXT);
    p.pop_clip();
}

class section_head : public ui::widget {
public:
    explicit section_head(const char* title)
        : m_title(title, STLXTHEME_UI_FONT_SEMIBOLD_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM) {}

    ui::size measure(ui::size avail) override {
        return { avail.w, HEAD_HEIGHT };
    }

    void paint(ui::painter& p) override {
        m_title.draw(p, HEAD_PADDING_X, HEAD_HEIGHT - HEAD_PADDING_BOTTOM - SUBTITLE_LINE_HEIGHT, SUBTITLE_LINE_HEIGHT);
    }

private:
    text_picture m_title;
};

/* One row: the icon, the label with what the query matched marked, the subtitle under it, and at the right the
 * keycaps of a shortcut that runs the same, else the row's kind when the palette names it */
class result_row : public ui::widget {
public:
    result_row(const palette_entry& entry, std::vector<match_run> runs, const char* kind_name);

    /* Fired when the pointer enters the row, and by a release over it */
    std::function<void()> on_hover;
    std::function<void()> on_choose;

    void set_selected(bool selected);

    ui::size measure(ui::size avail) override {
        return { avail.w, ROW_HEIGHT };
    }

    void paint(ui::painter& p) override;

    void on_pointer_enter() override {
        if (on_hover) {
            on_hover();
        }
    }

    bool on_pointer_up(const ui::pointer_event&) override {
        if (on_choose) {
            on_choose();
        }

        return true;
    }

private:
    void paint_icon(ui::painter& p) const;
    void paint_marks(ui::painter& p, int32_t label_top) const;
    void paint_right(ui::painter& p) const;

    const palette_entry&                 m_entry;
    std::vector<match_run>               m_runs;
    text_picture                         m_label;
    size_t                               m_label_kept = 0; /* bytes of the label before an ellipsis */
    text_picture                         m_subtitle;
    text_picture                         m_kind;
    text_picture                         m_initial;
    std::vector<std::unique_ptr<keycap>> m_keycaps;
    bool                                 m_selected = false;
};

result_row::result_row(const palette_entry& entry, std::vector<match_run> runs, const char* kind_name)
    : m_entry(entry), m_runs(std::move(runs)),
      m_label("", STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT),
      m_subtitle("", subtitle_face(entry), STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM),
      m_kind("", STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM),
      m_initial(entry.tile.empty() ? entry.label.substr(0, 1) : entry.tile, STLXTHEME_UI_FONT_MEDIUM_PATH,
                STLXTHEME_BODY_SIZE, STLXTHEME_TEXT_SECONDARY) {
    int32_t right_w = 0;
    for (const std::string& key : entry.keys) {
        m_keycaps.push_back(std::make_unique<keycap>(key));
        right_w += m_keycaps.back()->width() + (right_w > 0 ? KEYCAP_GAP : 0);
    }

    if (m_keycaps.empty() && kind_name) {
        m_kind.set_text(kind_name);
        right_w = m_kind.width();
    }

    int32_t text_w = ROW_WIDTH - ROW_PADDING_RIGHT - right_w - (right_w > 0 ? ROW_GAP : 0) - TEXT_LEFT;
    m_label_kept = fitting_length(entry.label, STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE, text_w);
    m_label.set_text(fitted(entry.label, STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE, text_w));
    m_subtitle.set_text(fitted(entry.subtitle, subtitle_face(entry), STLXTHEME_CAPTION_SIZE, text_w));
}

void result_row::set_selected(bool selected) {
    if (selected == m_selected) {
        return;
    }

    m_selected = selected;
    m_initial.set_color(selected ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY);
    for (const std::unique_ptr<keycap>& key : m_keycaps) {
        key->set_hover(selected);
    }

    invalidate();
}

void result_row::paint(ui::painter& p) {
    if (m_selected) {
        p.rounded_rect({ 0, 0, m_frame.w, m_frame.h }, STLXTHEME_CONTROL_RADIUS, STLXTHEME_SURFACE_SELECTED);
    }

    paint_icon(p);

    bool has_subtitle = !m_entry.subtitle.empty();
    int32_t lines_h = has_subtitle ? LABEL_LINE_HEIGHT + SUBTITLE_LINE_HEIGHT : LABEL_LINE_HEIGHT;
    int32_t label_top = (m_frame.h - lines_h) / 2;
    paint_marks(p, label_top);
    m_label.draw(p, TEXT_LEFT, label_top, LABEL_LINE_HEIGHT);
    if (has_subtitle) {
        m_subtitle.draw(p, TEXT_LEFT, label_top + LABEL_LINE_HEIGHT, SUBTITLE_LINE_HEIGHT);
    }

    paint_right(p);
}

void result_row::paint_icon(ui::painter& p) const {
    int32_t top = (m_frame.h - ICON_SIZE) / 2;
    if (m_entry.icon) {
        p.image({ ROW_PADDING_LEFT, top }, m_entry.icon);
    } else {
        ui::rect tile = { ROW_PADDING_LEFT, top, ICON_SIZE, ICON_SIZE };
        p.rounded_rect(tile, STLXTHEME_CONTROL_RADIUS, m_selected ? STLXTHEME_SURFACE_HOVER : STLXTHEME_SURFACE);
        p.rounded_stroke(tile, STLXTHEME_CONTROL_RADIUS, STLXTHEME_LINE_SUBTLE);
        m_initial.draw(p, ROW_PADDING_LEFT + (ICON_SIZE - m_initial.width()) / 2, top, ICON_SIZE);
    }

    if (m_entry.running) {
        ui::rect tick = { ROW_PADDING_LEFT + TICK_LEFT, top + ICON_SIZE + TICK_DROP, TICK_WIDTH, TICK_HEIGHT };
        p.rounded_rect(tick, TICK_RADIUS, STLXTHEME_ACCENT);
    }
}

void result_row::paint_marks(ui::painter& p, int32_t label_top) const {
    stlxgfx_font* font = shell_font(STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE);
    if (!font) {
        return;
    }

    const char* label = m_entry.label.data();
    for (const match_run& run : m_runs) {
        if (run.start + run.length > m_label_kept) {
            continue;
        }

        int32_t x = TEXT_LEFT + stlxgfx_text_width(font, label, run.start);
        int32_t w = stlxgfx_text_width(font, label + run.start, run.length);
        p.rounded_rect({ x, label_top, w, LABEL_LINE_HEIGHT }, MARK_RADIUS, STLXTHEME_MATCH_MARK);
    }
}

void result_row::paint_right(ui::painter& p) const {
    int32_t right = m_frame.w - ROW_PADDING_RIGHT;
    if (m_keycaps.empty()) {
        m_kind.draw(p, right - m_kind.width(), 0, m_frame.h);
        return;
    }

    int32_t top = (m_frame.h - KEYCAP_HEIGHT) / 2;
    for (size_t i = m_keycaps.size(); i-- > 0;) {
        right -= m_keycaps[i]->width();
        m_keycaps[i]->paint(p, right, top);
        right -= KEYCAP_GAP;
    }
}

class no_match : public ui::widget {
public:
    explicit no_match(const std::string& query)
        : m_text("", STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT) {
        std::string text = "No apps, actions or programs match " + query;
        m_text.set_text(fitted(text, STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE,
                               ROW_WIDTH - 2 * NO_MATCH_PADDING_X));
    }

    ui::size measure(ui::size avail) override {
        return { avail.w, LIST_HEIGHT };
    }

    void paint(ui::painter& p) override {
        m_text.draw(p, (m_frame.w - m_text.width()) / 2, 0, m_frame.h);
    }

private:
    text_picture m_text;
};

/* What Enter does to the selected row at the left, and how the palette closes at the right */
class palette_footer : public ui::widget {
public:
    palette_footer()
        : m_verb("", STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_SECONDARY),
          m_enter("Enter"), m_close("Esc closes", STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM) {}

    /* Names Enter's action, none while no row shows */
    void show_verb(const char* verb) {
        if (m_verb.set_text(verb ? verb : "")) {
            invalidate();
        }
    }

    ui::size measure(ui::size avail) override {
        return { avail.w, FOOTER_HEIGHT };
    }

    void paint(ui::painter& p) override {
        p.fill({ 0, 0, m_frame.w, 1 }, STLXTHEME_LINE_SUBTLE);
        if (m_verb.width() > 0) {
            m_verb.draw(p, FOOTER_PADDING_LEFT, 0, m_frame.h);
            m_enter.paint(p, FOOTER_PADDING_LEFT + m_verb.width() + FOOTER_KEY_GAP, (m_frame.h - KEYCAP_HEIGHT) / 2);
        }

        m_close.draw(p, m_frame.w - FOOTER_PADDING_RIGHT - m_close.width(), 0, m_frame.h);
    }

private:
    text_picture m_verb;
    keycap       m_enter;
    text_picture m_close;
};

/* The rows under the field, scrolled by the wheel and to keep the selected row in view */
class result_list : public ui::scroll_view {
public:
    result_list() {
        m_content = add<ui::box>(ui::axis::column);
    }

    ui::box* content() { return m_content; }

    /* Drops every row and shows the list from its top */
    void clear() {
        remove(m_content);
        m_content = add<ui::box>(ui::axis::column);
        scroll_to(0);
    }

    int32_t scroll_y() const { return m_scroll.y; }

    void scroll_to(int32_t y) {
        if (y != m_scroll.y) {
            m_scroll.y = y;
            invalidate();
        }
    }

private:
    ui::box* m_content;
};

/* The palette's content and its one focusable widget, which takes every key the hotkeys leave: typing edits the
 * query, Up and Down choose a row, Enter starts it */
class palette_view : public ui::box {
public:
    palette_view(std::vector<palette_entry> entries, const stlxgfx_surface_t* glyph);

    std::function<void(const palette_entry& entry)> on_choose;

    bool focusable() const override { return true; }
    bool on_key_down(const ui::key_event& e) override;

private:
    void edit(std::string query, size_t cursor);
    void move_caret(size_t cursor);
    void show_rows();
    void show_start();
    void show_matches(std::string_view query);
    bool find_calculation(std::string_view query);
    bool find_command(std::string_view query);
    void add_head(const char* title);
    void add_row(const palette_entry& entry, std::vector<match_run> runs, const char* kind_name);
    void select(size_t index);
    void move_selection(int32_t rows);
    void choose(size_t index);
    void erase_word();

    std::vector<palette_entry> m_entries;
    std::string                m_query;
    size_t                     m_caret = 0;

    /* The rows of the query's value and of a program the query names with words after it, while
     * `find_calculation` and `find_command` found them */
    palette_entry m_calculation;
    palette_entry m_command;

    query_field*    m_field;
    result_list*    m_list;
    palette_footer* m_footer;

    /* The shown rows in order, their entries, and the content row from which revealing each shows it, its section's
     * head included when it is the section's first row */
    std::vector<result_row*>           m_rows;
    std::vector<const palette_entry*>  m_row_entries;
    std::vector<int32_t>               m_row_reveal_tops;
    int32_t                            m_content_h = 0;
    bool                               m_follows_head = false;
    size_t                             m_selected = 0;
};

palette_view::palette_view(std::vector<palette_entry> entries, const stlxgfx_surface_t* glyph)
    : ui::box(ui::axis::column), m_entries(std::move(entries)) {
    ui::box* body = add<ui::box>(ui::axis::column);
    body->s().padding = ui::edge_insets::all(PALETTE_PADDING);
    body->s().gap = LIST_GAP;

    m_field = body->add<query_field>(glyph);
    m_list = body->add<result_list>();
    m_list->s().main = ui::length::fixed(LIST_HEIGHT);
    m_footer = add<palette_footer>();

    show_rows();
}

bool palette_view::on_key_down(const ui::key_event& e) {
    bool is_command = (e.modifiers & COMMAND_MODS) != 0;
    switch (e.usage) {
    case HID_USAGE_UP:
        move_selection(-1);
        return true;
    case HID_USAGE_DOWN:
        move_selection(1);
        return true;
    case HID_USAGE_PAGE_UP:
        move_selection(-PAGE_ROWS);
        return true;
    case HID_USAGE_PAGE_DOWN:
        move_selection(PAGE_ROWS);
        return true;
    case HID_USAGE_LEFT:
        move_caret(m_caret > 0 ? m_caret - 1 : 0);
        return true;
    case HID_USAGE_RIGHT:
        move_caret(std::min(m_caret + 1, m_query.size()));
        return true;
    case HID_USAGE_HOME:
        move_caret(0);
        return true;
    case HID_USAGE_END:
        move_caret(m_query.size());
        return true;
    case HID_USAGE_ENTER:
        choose(m_selected);
        return true;
    case HID_USAGE_TAB:
        return true;
    case HID_USAGE_DELETE:
        if (m_caret < m_query.size()) {
            edit(m_query.substr(0, m_caret) + m_query.substr(m_caret + 1), m_caret);
        }

        return true;
    case HID_USAGE_BACKSPACE:
        if (e.modifiers & CHORD_MOD_CTRL) {
            erase_word();
        } else if (m_caret > 0) {
            edit(m_query.substr(0, m_caret - 1) + m_query.substr(m_caret), m_caret - 1);
        }

        return true;
    default:
        break;
    }

    bool is_typed = !is_command && e.ch >= ' ' && e.ch < 0x7F;
    if (!is_typed) {
        return false;
    }

    std::string query = m_query;
    query.insert(m_caret, 1, static_cast<char>(e.ch));
    edit(std::move(query), m_caret + 1);

    return true;
}

void palette_view::edit(std::string query, size_t cursor) {
    m_query = std::move(query);
    m_caret = cursor;
    m_field->show(m_query, m_caret);
    show_rows();
}

void palette_view::move_caret(size_t cursor) {
    m_caret = cursor;
    m_field->show(m_query, m_caret);
}

/* Ctrl Backspace takes the spaces before the caret and the word before them */
void palette_view::erase_word() {
    size_t start = m_caret;
    while (start > 0 && m_query[start - 1] == ' ') {
        start--;
    }

    while (start > 0 && m_query[start - 1] != ' ') {
        start--;
    }

    edit(m_query.substr(0, start) + m_query.substr(m_caret), start);
}

/* Typing ranks every row by how well it matches, then by kind, then by this session's picks, and the best heads the
 * list as the top hit */
void palette_view::show_rows() {
    m_list->clear();
    m_rows.clear();
    m_row_entries.clear();
    m_row_reveal_tops.clear();
    m_content_h = 0;
    m_selected = 0;

    std::string_view query = trimmed(m_query);
    if (!query.empty()) {
        show_matches(query);
    } else {
        show_start();
    }

    select(0);
}

/* With nothing typed this session's picks show newest first, then the dock's other launchers in dock order */
void palette_view::show_start() {
    std::vector<const palette_entry*> recent;
    for (const palette_entry& entry : m_entries) {
        if (entry.recent != UNPICKED) {
            recent.push_back(&entry);
        }
    }

    std::sort(recent.begin(), recent.end(), [](const palette_entry* a, const palette_entry* b) {
        return a->recent < b->recent;
    });

    if (!recent.empty()) {
        add_head("RECENT");
    }

    for (const palette_entry* entry : recent) {
        add_row(*entry, {}, nullptr);
    }

    bool has_apps_head = false;
    for (const palette_entry& entry : m_entries) {
        if (entry.kind != row_kind::app || entry.recent != UNPICKED) {
            continue;
        }

        if (!has_apps_head) {
            add_head(KIND_HEADS[static_cast<size_t>(row_kind::app)]);
            has_apps_head = true;
        }

        add_row(entry, {}, nullptr);
    }
}

void palette_view::show_matches(std::string_view query) {
    struct found_entry {
        const palette_entry* entry;
        query_match          match;
    };

    std::vector<found_entry> found;
    for (const palette_entry& entry : m_entries) {
        query_match match = match_query(query, entry.label, entry.keywords);
        if (match.quality != match_quality::none) {
            found.push_back({ &entry, std::move(match) });
        }
    }

    bool has_calculation = find_calculation(query);
    bool has_command = find_command(query);
    if (found.empty() && !has_calculation && !has_command) {
        m_list->content()->add<no_match>(std::string(query));
        return;
    }

    std::stable_sort(found.begin(), found.end(), [](const found_entry& a, const found_entry& b) {
        if (a.match.quality != b.match.quality) {
            return a.match.quality < b.match.quality;
        }

        if (a.entry->kind != b.entry->kind) {
            return a.entry->kind < b.entry->kind;
        }

        return a.entry->recent < b.entry->recent;
    });

    if (has_calculation) {
        add_head(KIND_HEADS[static_cast<size_t>(row_kind::calculator)]);
        add_row(m_calculation, {}, nullptr);
    }

    if (has_command) {
        add_head(KIND_HEADS[static_cast<size_t>(row_kind::program)]);
        add_row(m_command, {}, nullptr);
    }

    size_t first_rest = 0;
    if (!has_calculation && !has_command) {
        add_head("TOP HIT");
        const found_entry& top = found.front();
        add_row(*top.entry, top.match.runs, KIND_NAMES[static_cast<size_t>(top.entry->kind)]);
        first_rest = 1;
    }

    for (row_kind kind : { row_kind::app, row_kind::action, row_kind::program }) {
        size_t shown = 0;
        for (size_t i = first_rest; i < found.size() && shown < KIND_ROWS_MAX; i++) {
            if (found[i].entry->kind != kind) {
                continue;
            }

            if (shown == 0) {
                add_head(KIND_HEADS[static_cast<size_t>(kind)]);
            }

            add_row(*found[i].entry, found[i].match.runs, nullptr);
            shown++;
        }
    }
}

/* An arithmetic query shows its value with commas between the thousands, and Enter copies it without them */
bool palette_view::find_calculation(std::string_view query) {
    double value = 0;
    if (!calculate(query, &value)) {
        return false;
    }

    m_calculation = palette_entry();
    m_calculation.kind = row_kind::calculator;
    m_calculation.label = shown_number(value);
    m_calculation.copied = copied_number(value);
    m_calculation.subtitle = std::string(query) + ", copied as " + m_calculation.copied;
    m_calculation.tile = "=";

    return true;
}

/* A query that names a program and goes on with words runs that program in Terminal with the words as its args,
 * split at spaces as every args value in stlxdm.conf is */
bool palette_view::find_command(std::string_view query) {
    size_t space = query.find(' ');
    if (space == std::string_view::npos) {
        return false;
    }

    std::string_view name = query.substr(0, space);
    for (const palette_entry& entry : m_entries) {
        if (entry.kind != row_kind::program || entry.label != name) {
            continue;
        }

        m_command = entry;
        m_command.label = "Run " + std::string(query);
        m_command.subtitle = entry.program + " in Terminal";
        m_command.is_subtitle_path = false;
        m_command.args = std::string(trimmed(query.substr(space + 1)));
        m_command.keys.clear();

        return true;
    }

    return false;
}

void palette_view::add_head(const char* title) {
    m_list->content()->add<section_head>(title);
    m_content_h += HEAD_HEIGHT;
    m_follows_head = true;
}

void palette_view::add_row(const palette_entry& entry, std::vector<match_run> runs, const char* kind_name) {
    size_t index = m_rows.size();
    result_row* row = m_list->content()->add<result_row>(entry, std::move(runs), kind_name);
    row->on_hover = [this, index]() {
        select(index);
    };

    row->on_choose = [this, index]() {
        choose(index);
    };

    m_rows.push_back(row);
    m_row_entries.push_back(&entry);
    m_row_reveal_tops.push_back(m_follows_head ? m_content_h - HEAD_HEIGHT : m_content_h);
    m_content_h += ROW_HEIGHT;
    m_follows_head = false;
}

void palette_view::select(size_t index) {
    if (m_rows.empty()) {
        m_footer->show_verb(nullptr);
        return;
    }

    m_rows[m_selected]->set_selected(false);
    m_selected = std::min(index, m_rows.size() - 1);
    m_rows[m_selected]->set_selected(true);
    m_footer->show_verb(KIND_VERBS[static_cast<size_t>(m_row_entries[m_selected]->kind)]);
}

/* Moves the selection without wrapping, scrolling the least that shows the newly selected row */
void palette_view::move_selection(int32_t rows) {
    if (m_rows.empty()) {
        return;
    }

    int32_t last = static_cast<int32_t>(m_rows.size()) - 1;
    select(static_cast<size_t>(std::clamp(static_cast<int32_t>(m_selected) + rows, 0, last)));

    int32_t top = m_row_reveal_tops[m_selected];
    int32_t bottom = top + ROW_HEIGHT;
    int32_t scroll = m_list->scroll_y();
    if (top < scroll) {
        scroll = top;
    } else if (bottom > scroll + LIST_HEIGHT) {
        scroll = bottom - LIST_HEIGHT;
    }

    m_list->scroll_to(scroll);
}

void palette_view::choose(size_t index) {
    if (index >= m_rows.size() || !on_choose) {
        return;
    }

    select(index);
    on_choose(*m_row_entries[index]);
}

/* The icon of the first launcher that runs `path` and has one, null when none does */
const stlxgfx_surface_t* icon_of_program(const stlxconf_t& conf, const std::vector<stlxgfx_surface_t*>& icons,
                                         const char* path) {
    for (uint32_t i = 0; i < conf.pin_count && i < icons.size(); i++) {
        if (icons[i] && strcmp(conf.pins[i].path, path) == 0) {
            return icons[i];
        }
    }

    return nullptr;
}

/* A launcher's row shows its program's path, and its program's file name finds it too */
palette_entry launcher_entry(const stlxconf_t& conf, uint32_t index, const stlxgfx_surface_t* icon, bool running) {
    const stlxconf_pin_t& pin = conf.pins[index];
    palette_entry entry;
    entry.kind = row_kind::app;
    entry.index = index;
    entry.label = pin.label[0] ? pin.label : file_name(pin.path);

    entry.subtitle = pin.path;
    entry.is_subtitle_path = true;
    entry.keywords = file_name(pin.path);
    entry.keys = shortcut_keys(conf, STLXCONF_ACTION_EXEC, pin.path, pin.args);

    entry.icon = icon;
    entry.running = running;

    return entry;
}

bool is_launched(const stlxconf_t& conf, const std::string& path) {
    for (uint32_t i = 0; i < conf.pin_count; i++) {
        if (path == conf.pins[i].path) {
            return true;
        }
    }

    return false;
}

/* Hidden files, the programs Power... stands in for, and a name already listed stay out */
bool is_left_out(const std::vector<palette_entry>& listed, const char* name) {
    if (name[0] == '.') {
        return true;
    }

    for (const char* unlisted : UNLISTED_PROGRAMS) {
        if (strcmp(name, unlisted) == 0) {
            return true;
        }
    }

    for (const palette_entry& program : listed) {
        if (program.label == name) {
            return true;
        }
    }

    return false;
}

/* The programs of /bin and /usr/bin that no launcher runs, by name, where /bin wins a name both hold */
std::vector<palette_entry> program_entries(const stlxconf_t& conf, const stlxgfx_surface_t* icon) {
    std::vector<palette_entry> programs;
    for (const char* dir_path : PROGRAM_DIRS) {
        DIR* dir = opendir(dir_path);
        if (!dir) {
            continue;
        }

        while (dirent* file = readdir(dir)) {
            std::string path = std::string(dir_path) + "/" + file->d_name;
            struct stat st;
            bool is_program = stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
            if (!is_program || is_left_out(programs, file->d_name) || is_launched(conf, path)) {
                continue;
            }

            palette_entry program;
            program.kind = row_kind::program;
            program.label = file->d_name;
            program.program = path;
            program.icon = icon;

            program.subtitle = path;
            program.is_subtitle_path = true;
            program.keys = shortcut_keys(conf, STLXCONF_ACTION_EXEC, TERMINAL_PATH, path.c_str());
            programs.push_back(std::move(program));
        }

        closedir(dir);
    }

    std::sort(programs.begin(), programs.end(), [](const palette_entry& a, const palette_entry& b) {
        return a.label < b.label;
    });

    return programs;
}

/* An action's row shows its subtitle, else the command it runs */
palette_entry action_entry(const stlxconf_t& conf, uint32_t index, const stlxgfx_surface_t* icon) {
    const stlxconf_action_t& action = conf.actions[index];
    palette_entry entry;
    entry.kind = row_kind::action;
    entry.index = index;
    entry.label = action.label[0] ? action.label : action.name;
    entry.icon = icon;

    stlxconf_action_kind_t kind = stlxconf_action_kind(action.action);
    entry.keywords = action.keywords;
    entry.keys = shortcut_keys(conf, kind, action.path, action.args);
    if (action.subtitle[0]) {
        entry.subtitle = action.subtitle;
    } else if (kind == STLXCONF_ACTION_EXEC) {
        entry.subtitle = action.args[0] ? std::string(action.path) + " " + action.args : action.path;
        entry.is_subtitle_path = true;
    }

    return entry;
}

} // namespace

quick_actions::quick_actions(const stlxconf_t& conf, const std::vector<stlxgfx_surface_t*>& launcher_icons)
    : m_conf(conf), m_launcher_icons(launcher_icons),
      m_search_glyph(make_search_glyph(FIELD_GLYPH_SIZE, STLXTHEME_TEXT_DIM)),
      m_program_icon(load_icon(PROGRAM_ICON_PATH)) {
    for (uint32_t i = 0; i < conf.action_count; i++) {
        const char* path = conf.actions[i].icon_path;
        m_action_icons.push_back(path[0] ? load_icon(path) : nullptr);
    }
}

quick_actions::~quick_actions() {
    for (stlxgfx_surface_t* icon : m_action_icons) {
        stlxgfx_destroy_surface(icon);
    }

    stlxgfx_destroy_surface(m_search_glyph);
    stlxgfx_destroy_surface(m_program_icon);
}

int32_t quick_actions::width() const {
    return PALETTE_WIDTH;
}

int32_t quick_actions::left(int32_t output_w) const {
    return (output_w - PALETTE_WIDTH) / 2;
}

int32_t quick_actions::top() const {
    return PALETTE_TOP;
}

/* A calculation is no pick, and a pick made again moves to the front */
void quick_actions::remember_pick(const std::string& key) {
    if (key.empty()) {
        return;
    }

    m_picks.erase(std::remove(m_picks.begin(), m_picks.end(), key), m_picks.end());
    m_picks.insert(m_picks.begin(), key);
    if (m_picks.size() > RECENT_MAX) {
        m_picks.pop_back();
    }
}

/* Launchers without a program and actions that name no action of their own, or open the palette itself, show no
 * row */
std::unique_ptr<ui::widget> quick_actions::build() {
    std::vector<palette_entry> entries;
    for (uint32_t i = 0; i < m_conf.pin_count; i++) {
        if (!m_conf.pins[i].path[0]) {
            continue;
        }

        const stlxgfx_surface_t* icon = i < m_launcher_icons.size() ? m_launcher_icons[i] : nullptr;
        bool running = i < 64 && ((m_running >> i) & 1) != 0;
        entries.push_back(launcher_entry(m_conf, i, icon, running));
    }

    for (uint32_t i = 0; i < m_conf.action_count; i++) {
        const stlxconf_action_t& action = m_conf.actions[i];
        stlxconf_action_kind_t kind = stlxconf_action_kind(action.action);
        bool runs_program = kind == STLXCONF_ACTION_EXEC && action.path[0];
        if (!runs_program && kind != STLXCONF_ACTION_POWER) {
            continue;
        }

        const stlxgfx_surface_t* icon = m_action_icons[i];
        if (!icon && runs_program) {
            icon = icon_of_program(m_conf, m_launcher_icons, action.path);
        }

        entries.push_back(action_entry(m_conf, i, icon));
    }

    for (palette_entry& program : program_entries(m_conf, m_program_icon)) {
        entries.push_back(std::move(program));
    }

    for (palette_entry& entry : entries) {
        auto pick = std::find(m_picks.begin(), m_picks.end(), pick_key(entry));
        entry.recent = pick == m_picks.end() ? UNPICKED : static_cast<uint32_t>(pick - m_picks.begin());
    }

    auto view = std::make_unique<palette_view>(std::move(entries), m_search_glyph);
    view->on_choose = [this](const palette_entry& entry) {
        remember_pick(pick_key(entry));
        switch (entry.kind) {
        case row_kind::app:
            on_launch(m_conf.pins[entry.index]);
            break;
        case row_kind::action:
            on_run(m_conf.actions[entry.index]);
            break;
        case row_kind::program:
            on_run_program(entry.program.c_str(), entry.args.c_str());
            break;
        case row_kind::calculator:
            on_copy(entry.copied);
            break;
        }
    };

    return view;
}
