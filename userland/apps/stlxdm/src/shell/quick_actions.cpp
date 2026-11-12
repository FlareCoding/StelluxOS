#include "quick_actions.hpp"

#include "chord.hpp"
#include "dock.hpp"
#include "draw.hpp"
#include "keycap.hpp"
#include "palette_parts.hpp"
#include "programs.hpp"
#include "quick_calc.hpp"
#include "quick_match.hpp"

#include <stlxtheme/theme.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <iterator>
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

constexpr int32_t FIELD_CHIPS_RIGHT = 6;
constexpr int32_t CHIP_HEIGHT = 32;
constexpr int32_t CHIP_MIN_WIDTH = 34;
constexpr int32_t CHIP_PADDING_X = 9;
constexpr int32_t CHIP_NAME_GAP = 7;
constexpr int32_t CHIP_GAP = 2;
constexpr int32_t CHIP_GLYPH_SIZE = 16;

constexpr int32_t ROW_HEIGHT = 44;
constexpr int32_t ROW_WIDTH = PALETTE_WIDTH - 2 * PALETTE_PADDING;
constexpr int32_t ROW_PADDING_LEFT = 8;
constexpr int32_t ROW_PADDING_RIGHT = 10;
constexpr int32_t ROW_GAP = 12;
constexpr int32_t ICON_SIZE = STLXTHEME_DOCK_ICON_SIZE;
constexpr int32_t TEXT_LEFT = ROW_PADDING_LEFT + ICON_SIZE + ROW_GAP;
constexpr int32_t CHOICE_TEXT_LEFT = 14;
constexpr int32_t LABEL_LINE_HEIGHT = 18;
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
constexpr size_t NARROWED_ROWS_MAX = 100;
constexpr size_t RECENT_MAX = 3;
constexpr uint32_t UNPICKED = UINT32_MAX;
constexpr int32_t PAGE_ROWS = 8;
constexpr int32_t NO_MATCH_PADDING_X = 40;
constexpr int32_t NO_MATCH_GAP = 4;
constexpr int32_t NOTE_HEIGHT = 32;

/* The strip a failed start shows under its row, on cream at .03 inside a faint ring */
constexpr int32_t STRIP_MARGIN_TOP = 4;
constexpr int32_t STRIP_HEIGHT = 40;
constexpr int32_t STRIP_PADDING_X = 12;
constexpr int32_t STRIP_MARK_SIZE = 16;
constexpr int32_t STRIP_TEXT_LEFT = STRIP_PADDING_X + STRIP_MARK_SIZE + 10;
constexpr uint32_t STRIP_FILL = 0x08FFF0D7;

constexpr int32_t FOOTER_HEIGHT = 36;
constexpr int32_t FOOTER_PADDING_LEFT = 14;
constexpr int32_t FOOTER_PADDING_RIGHT = 10;
constexpr int32_t FOOTER_KEY_GAP = 6;
constexpr int32_t FOOTER_ITEM_GAP = 18;

constexpr uint16_t HID_USAGE_C = 0x06;
constexpr uint16_t HID_USAGE_K = 0x0E;
constexpr uint16_t HID_USAGE_1 = 0x1E;
constexpr uint16_t HID_USAGE_ENTER = 0x28;
constexpr uint16_t HID_USAGE_ESCAPE = 0x29;
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

constexpr std::string_view PLACEHOLDER = "Search apps, files and actions";

constexpr const char* PROGRAM_DIRS[] = { "/bin", "/usr/bin" };
constexpr const char* PROGRAM_ICON_PATH = "/etc/res/icons/doc-exec-32.png";
constexpr const char* FOLDER_ICON_PATH = "/etc/res/icons/folder-32.png";
constexpr const char* TEXT_ICON_PATH = "/etc/res/icons/doc-text-32.png";
constexpr const char* IMAGE_ICON_PATH = "/etc/res/icons/doc-image-32.png";

/* The type the describer gives folders, which File Manager opens */
constexpr const char* FOLDER_TYPE = "inode/directory";

/* The most files one answer brings, which keeps a search's copy small */
constexpr size_t FILE_RESULTS_MAX = 50;

/* A handler's own arguments before the file's path */
constexpr int HANDLER_ARGS_MAX = 16;

/* Power... asks before restarting or shutting down, so the programs that do it at once, and init, stay unlisted */
constexpr const char* UNLISTED_PROGRAMS[] = { "init", "reboot", "shutdown" };

namespace {

/* The kinds rank in this order where matches are equally good, a calculation heads the list before any, the last
 * row of the files may offer the rest of them, and Ctrl K lists a row's actions as choices */
enum class row_kind : uint8_t { app, action, program, file, calculator, more_files, choice };

constexpr const char* KIND_HEADS[] = { "APPS", "ACTIONS", "PROGRAMS", "FILES", "CALCULATOR", "", "" };
constexpr const char* KIND_NAMES[] = { "Application", "Action", "Program", "File", "Calculation", "", "" };
constexpr const char* KIND_VERBS[] = { "Open", "Run", "Run in Terminal", "Open", "Copy", "Show Files Only", "Choose" };

/* What the chips narrow the rows to, each on its Ctrl digit, which again shows every kind */
enum class narrow_mode : uint8_t { all, apps, files, actions };

constexpr size_t CHIP_COUNT = 3;
constexpr narrow_mode CHIP_MODES[CHIP_COUNT] = { narrow_mode::apps, narrow_mode::files, narrow_mode::actions };
constexpr const char* CHIP_NAMES[CHIP_COUNT] = { "Apps", "Files", "Actions" };

/* A square of the apps glyph, its corners cut */
std::vector<glyph_point> glyph_square(float left, float top) {
    constexpr float SIDE = 5.5f;
    constexpr float CUT = 1.2f;

    return { { left + CUT, top }, { left + SIDE - CUT, top }, { left + SIDE, top + CUT },
             { left + SIDE, top + SIDE - CUT }, { left + SIDE - CUT, top + SIDE }, { left + CUT, top + SIDE },
             { left, top + SIDE - CUT }, { left, top + CUT } };
}

/* The chips' glyphs on the 16 unit grid of the mocks: four squares, a page with two lines cut out, and a bolt */
std::vector<std::vector<glyph_point>> chip_contours(narrow_mode mode) {
    switch (mode) {
    case narrow_mode::apps:
        return { glyph_square(1.5f, 1.5f), glyph_square(9.0f, 1.5f), glyph_square(1.5f, 9.0f),
                 glyph_square(9.0f, 9.0f) };
    case narrow_mode::files:
        return { { { 3.5f, 1 }, { 9, 1 }, { 13, 5 }, { 13, 13.5f }, { 12.6f, 14.6f }, { 11.5f, 15 }, { 3.5f, 15 },
                   { 2.4f, 14.6f }, { 2, 13.5f }, { 2, 2.5f }, { 2.4f, 1.4f } },
                 { { 5, 8 }, { 11, 8 }, { 11, 9.5f }, { 5, 9.5f } },
                 { { 5, 11 }, { 9.5f, 11 }, { 9.5f, 12.5f }, { 5, 12.5f } } };
    case narrow_mode::actions:
        return { { { 9.6f, 1 }, { 3, 9.2f }, { 7.3f, 9.2f }, { 6.4f, 15 }, { 13, 6.8f }, { 8.7f, 6.8f } } };
    case narrow_mode::all:
        break;
    }

    return {};
}

/* One thing the palette starts, as its row shows it */
struct palette_entry {
    row_kind                 kind = row_kind::app;
    uint32_t                 index = 0; /* into the configuration's launchers or actions */
    std::string              path;      /* the program a program's row runs in Terminal with `args`, or a file */
    std::string              args;
    size_t                   choice = 0; /* a choice's place among the actions Ctrl K lists */
    const char*              mime_type = nullptr; /* a file's, which picks its handler */
    std::string              copied;    /* what a calculation's row copies */
    std::string              label;
    std::string              subtitle;
    bool                     is_subtitle_path = false; /* drawn in Plex Mono */
    std::string              right;     /* at the right of a row without keycaps, such as a file's size */
    std::string              verb;      /* Enter's action in the footer, when not its kind's */
    std::string              keywords;
    const stlxgfx_surface_t* icon = nullptr; /* null draws `tile` on a tile, else the label's first letter */
    std::string              tile;
    std::vector<std::string> keys;           /* of a shortcut that runs the same */
    bool                     running = false;
    uint32_t                 recent = UNPICKED; /* its place among this session's picks, newest first */
    found_file               file;              /* what a file's row was made from, kept for this session's picks */
};

/* What tells two rows apart across a rebuild of the list */
std::string row_key(const palette_entry& entry) {
    return std::to_string(static_cast<int>(entry.kind)) + " " + entry.label + " " + entry.path;
}

/* What a pick is remembered by across openings: the launcher or action it is, or the program or file it opens */
std::string pick_key(const palette_entry& entry) {
    switch (entry.kind) {
    case row_kind::app:
        return "app " + std::to_string(entry.index);
    case row_kind::action:
        return "action " + std::to_string(entry.index);
    case row_kind::program:
        return "program " + entry.path;
    case row_kind::file:
        return "file " + entry.path;
    case row_kind::calculator:
    case row_kind::more_files:
    case row_kind::choice:
        break;
    }

    return {};
}

const char* subtitle_face(const palette_entry& entry) {
    return entry.is_subtitle_path ? STLXTHEME_MONO_FONT_PATH : STLXTHEME_UI_FONT_PATH;
}

/* A choice of Ctrl K's list has no icon, so its words start near the row's edge */
int32_t text_left_of(const palette_entry& entry) {
    return entry.kind == row_kind::choice ? CHOICE_TEXT_LEFT : TEXT_LEFT;
}

/* A query starting with / or ~/ names a folder up to its last slash and the start of an entry's name after it */
struct typed_path {
    bool        is_path = false;
    std::string folder;       /* with its trailing slash, ~ standing for the home folder */
    std::string prefix;
    std::string typed_folder; /* the folder as typed, which completion keeps */
};

typed_path parse_path(std::string_view query) {
    typed_path path;
    bool is_home = query == "~" || query.substr(0, 2) == "~/";
    if (query.empty() || (query[0] != '/' && !is_home)) {
        return path;
    }

    std::string expanded(query);
    if (is_home) {
        const char* home = getenv("HOME");
        std::string home_folder = home && home[0] && strcmp(home, "/") != 0 ? home : "";
        expanded = home_folder + (query.size() > 1 ? std::string(query.substr(1)) : "/");
    }

    size_t slash = expanded.rfind('/');
    size_t typed_slash = query.rfind('/');
    path.is_path = true;
    path.folder = expanded.substr(0, slash + 1);
    path.prefix = expanded.substr(slash + 1);
    path.typed_folder = typed_slash == std::string_view::npos ? "~/" : std::string(query.substr(0, typed_slash + 1));

    return path;
}

/* A folder as a head names it, without the trailing slash but for the root */
std::string shown_folder(const std::string& folder) {
    return folder.size() > 1 ? folder.substr(0, folder.size() - 1) : folder;
}

/* The search field, always focused while the palette is open, so it always shows the amber rule and the caret */
class query_field : public ui::widget {
public:
    /* `chip_glyphs` holds each chip's glyph at rest, then each chip's glyph when chosen */
    query_field(const stlxgfx_surface_t* glyph, const std::vector<stlxgfx_surface_t*>& chip_glyphs);

    /* Fired by a click on a chip */
    std::function<void(narrow_mode mode)> on_chip;

    void show(const std::string& text, size_t cursor) {
        m_text = text;
        m_cursor = cursor;
        invalidate();
    }

    void show_mode(narrow_mode mode) {
        if (mode != m_mode) {
            m_mode = mode;
            invalidate();
        }
    }

    ui::size measure(ui::size avail) override {
        return { avail.w, FIELD_HEIGHT };
    }

    void paint(ui::painter& p) override;

    bool on_pointer_up(const ui::pointer_event& e) override;

private:
    ui::rect chip_rect(size_t chip) const;
    void paint_chips(ui::painter& p) const;

    const stlxgfx_surface_t*                   m_glyph;
    const std::vector<stlxgfx_surface_t*>&     m_chip_glyphs;
    std::vector<std::unique_ptr<text_picture>> m_chip_names;
    std::string                                m_text;
    size_t                                     m_cursor = 0;
    narrow_mode                                m_mode = narrow_mode::all;
};

query_field::query_field(const stlxgfx_surface_t* glyph, const std::vector<stlxgfx_surface_t*>& chip_glyphs)
    : m_glyph(glyph), m_chip_glyphs(chip_glyphs) {
    for (const char* name : CHIP_NAMES) {
        m_chip_names.push_back(std::make_unique<text_picture>(name, STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE,
                                                              STLXTHEME_TEXT));
    }
}

/* The chips sit at the field's end, a chosen one wide enough for its name after its glyph */
ui::rect query_field::chip_rect(size_t chip) const {
    int32_t widths[CHIP_COUNT];
    int32_t total = 0;
    for (size_t i = 0; i < CHIP_COUNT; i++) {
        bool is_chosen = CHIP_MODES[i] == m_mode;
        widths[i] = is_chosen ? 2 * CHIP_PADDING_X + CHIP_GLYPH_SIZE + CHIP_NAME_GAP + m_chip_names[i]->width()
                              : CHIP_MIN_WIDTH;
        total += widths[i] + (i > 0 ? CHIP_GAP : 0);
    }

    int32_t x = m_frame.w - FIELD_CHIPS_RIGHT - total;
    for (size_t i = 0; i < chip; i++) {
        x += widths[i] + CHIP_GAP;
    }

    return { x, (m_frame.h - CHIP_HEIGHT) / 2, widths[chip], CHIP_HEIGHT };
}

void query_field::paint_chips(ui::painter& p) const {
    for (size_t i = 0; i < CHIP_COUNT; i++) {
        ui::rect chip = chip_rect(i);
        bool is_chosen = CHIP_MODES[i] == m_mode;
        if (is_chosen) {
            p.rounded_rect(chip, STLXTHEME_INNER_RADIUS, STLXTHEME_SURFACE_ON);
            p.rounded_stroke(chip, STLXTHEME_INNER_RADIUS, STLXTHEME_LINE_ON);
            m_chip_names[i]->draw(p, chip.x + CHIP_PADDING_X + CHIP_GLYPH_SIZE + CHIP_NAME_GAP, chip.y, chip.h);
        }

        const stlxgfx_surface_t* glyph = m_chip_glyphs[is_chosen ? CHIP_COUNT + i : i];
        if (glyph) {
            p.image({ chip.x + CHIP_PADDING_X, chip.y + (chip.h - CHIP_GLYPH_SIZE) / 2 }, glyph);
        }
    }
}

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

    int32_t text_right = chip_rect(0).x - FIELD_PADDING_RIGHT;
    p.push_clip({ FIELD_TEXT_LEFT, 0, text_right - FIELD_TEXT_LEFT, m_frame.h });
    p.text({ FIELD_TEXT_LEFT, top + p.font_ascent(QUERY_SIZE) }, shown, QUERY_SIZE,
           is_empty ? STLXTHEME_TEXT_DIM : STLXTHEME_TEXT);
    p.fill({ FIELD_TEXT_LEFT + before_caret.w, top, 1, line.h }, STLXTHEME_TEXT);
    p.pop_clip();

    paint_chips(p);
}

bool query_field::on_pointer_up(const ui::pointer_event& e) {
    for (size_t i = 0; i < CHIP_COUNT; i++) {
        if (chip_rect(i).contains(e.pos) && on_chip) {
            on_chip(CHIP_MODES[i]);
            return true;
        }
    }

    return false;
}

/* One row: the icon, the label with what the query matched marked, the subtitle under it, and at the right the
 * keycaps of a shortcut that runs the same, else what the entry shows there or the kind the palette names */
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

    const char* right_text = entry.right.empty() ? kind_name : entry.right.c_str();
    if (m_keycaps.empty() && right_text) {
        m_kind.set_text(right_text);
        right_w = m_kind.width();
    }

    int32_t text_w = ROW_WIDTH - ROW_PADDING_RIGHT - right_w - (right_w > 0 ? ROW_GAP : 0) - text_left_of(entry);
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
    int32_t text_left = text_left_of(m_entry);
    paint_marks(p, label_top);

    m_label.draw(p, text_left, label_top, LABEL_LINE_HEIGHT);
    if (has_subtitle) {
        m_subtitle.draw(p, text_left, label_top + LABEL_LINE_HEIGHT, SUBTITLE_LINE_HEIGHT);
    }

    paint_right(p);
}

void result_row::paint_icon(ui::painter& p) const {
    if (m_entry.kind == row_kind::choice) {
        return;
    }

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

        int32_t x = text_left_of(m_entry) + stlxgfx_text_width(font, label, run.start);
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

/* What kept a row from starting, in a strip under the row: a mark in the danger color, then the reason */
class failure_strip : public ui::widget {
public:
    explicit failure_strip(const std::string& message)
        : m_text("", STLXTHEME_UI_FONT_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT_SECONDARY),
          m_mark("!", STLXTHEME_UI_FONT_SEMIBOLD_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_DANGER_TEXT) {
        int32_t max_w = ROW_WIDTH - STRIP_TEXT_LEFT - STRIP_PADDING_X;
        m_text.set_text(fitted(message, STLXTHEME_UI_FONT_PATH, STLXTHEME_BODY_SIZE, max_w));
    }

    ui::size measure(ui::size avail) override {
        return { avail.w, STRIP_MARGIN_TOP + STRIP_HEIGHT };
    }

    void paint(ui::painter& p) override {
        ui::rect strip = { 0, STRIP_MARGIN_TOP, m_frame.w, STRIP_HEIGHT };
        p.rounded_rect(strip, STLXTHEME_CONTROL_RADIUS, STRIP_FILL);
        p.rounded_stroke(strip, STLXTHEME_CONTROL_RADIUS, STLXTHEME_LINE_SUBTLE);

        ui::point center = { STRIP_PADDING_X + STRIP_MARK_SIZE / 2, strip.y + strip.h / 2 };
        p.circle(center, STRIP_MARK_SIZE / 2, STLXTHEME_DANGER);
        m_mark.draw(p, center.x - m_mark.width() / 2, strip.y, strip.h);
        m_text.draw(p, STRIP_TEXT_LEFT, strip.y, strip.h);
    }

private:
    text_picture m_text;
    text_picture m_mark;
};

/* A quiet line under Ctrl K's choices, such as why a file opens where Enter opens it */
class note_line : public ui::widget {
public:
    explicit note_line(const std::string& text)
        : m_text("", STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_DIM) {
        m_text.set_text(fitted(text, STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, ROW_WIDTH - 2 * CHOICE_TEXT_LEFT));
    }

    ui::size measure(ui::size avail) override {
        return { avail.w, NOTE_HEIGHT };
    }

    void paint(ui::painter& p) override {
        m_text.draw(p, CHOICE_TEXT_LEFT, 0, m_frame.h);
    }

private:
    text_picture m_text;
};

/* What matched nothing, and under it a note, such as where files were searched, so a missing file is explained */
class no_match : public ui::widget {
public:
    no_match(const std::string& text, const std::string& note)
        : m_text("", STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT),
          m_note("", STLXTHEME_UI_FONT_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT_DIM) {
        int32_t max_w = ROW_WIDTH - 2 * NO_MATCH_PADDING_X;
        m_text.set_text(fitted(text, STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE, max_w));
        m_note.set_text(fitted(note, STLXTHEME_UI_FONT_PATH, STLXTHEME_BODY_SIZE, max_w));
    }

    ui::size measure(ui::size avail) override {
        return { avail.w, LIST_HEIGHT };
    }

    void paint(ui::painter& p) override {
        bool has_note = m_note.width() > 0;
        int32_t lines_h = has_note ? LABEL_LINE_HEIGHT + NO_MATCH_GAP + LABEL_LINE_HEIGHT : LABEL_LINE_HEIGHT;
        int32_t top = (m_frame.h - lines_h) / 2;
        m_text.draw(p, (m_frame.w - m_text.width()) / 2, top, LABEL_LINE_HEIGHT);
        m_note.draw(p, (m_frame.w - m_note.width()) / 2, top + LABEL_LINE_HEIGHT + NO_MATCH_GAP, LABEL_LINE_HEIGHT);
    }

private:
    text_picture m_text;
    text_picture m_note;
};

/* Footer items drawn as labels each followed by their keycaps */
class footer_group {
public:
    footer_group(const char* face_path, uint32_t color) : m_face_path(face_path), m_color(color) {}

    /* Returns whether the items changed */
    bool show(const std::vector<footer_item>& items);

    int32_t width(int32_t item_gap) const;
    void paint(ui::painter& p, int32_t x, int32_t box_h, int32_t item_gap) const;

private:
    const char*                                       m_face_path;
    uint32_t                                          m_color;
    std::vector<footer_item>                          m_shown;
    std::vector<std::unique_ptr<text_picture>>        m_labels;
    std::vector<std::vector<std::unique_ptr<keycap>>> m_keycaps;
};

bool footer_group::show(const std::vector<footer_item>& items) {
    if (items == m_shown) {
        return false;
    }

    m_shown = items;
    m_labels.clear();
    m_keycaps.clear();
    for (const footer_item& item : items) {
        m_labels.push_back(std::make_unique<text_picture>(item.label, m_face_path, STLXTHEME_CAPTION_SIZE, m_color));
        m_keycaps.emplace_back();
        for (size_t at = 0; at < item.keys.size();) {
            size_t end = std::min(item.keys.find(' ', at), item.keys.size());
            m_keycaps.back().push_back(std::make_unique<keycap>(item.keys.substr(at, end - at)));
            at = end + 1;
        }
    }

    return true;
}

int32_t footer_group::width(int32_t item_gap) const {
    int32_t width = 0;
    for (size_t i = 0; i < m_labels.size(); i++) {
        width += (i > 0 ? item_gap : 0) + m_labels[i]->width() + FOOTER_KEY_GAP;
        for (size_t k = 0; k < m_keycaps[i].size(); k++) {
            width += (k > 0 ? KEYCAP_GAP : 0) + m_keycaps[i][k]->width();
        }
    }

    return width;
}

void footer_group::paint(ui::painter& p, int32_t x, int32_t box_h, int32_t item_gap) const {
    int32_t key_top = (box_h - KEYCAP_HEIGHT) / 2;
    for (size_t i = 0; i < m_labels.size(); i++) {
        m_labels[i]->draw(p, x, 0, box_h);
        x += m_labels[i]->width() + FOOTER_KEY_GAP;
        for (const std::unique_ptr<keycap>& key : m_keycaps[i]) {
            key->paint(p, x, key_top);
            x += key->width() + KEYCAP_GAP;
        }

        x += item_gap - KEYCAP_GAP;
    }
}

/* The actions of the selected row and the mode at the left, each with its keys, and the palette's own at the right */
class palette_footer : public ui::widget {
public:
    palette_footer()
        : m_actions(STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_TEXT_SECONDARY),
          m_end(STLXTHEME_UI_FONT_PATH, STLXTHEME_TEXT_DIM) {}

    void show(const std::vector<footer_item>& actions, const std::vector<footer_item>& end) {
        bool has_changed = m_actions.show(actions);
        has_changed = m_end.show(end) || has_changed;
        if (has_changed) {
            invalidate();
        }
    }

    ui::size measure(ui::size avail) override {
        return { avail.w, FOOTER_HEIGHT };
    }

    void paint(ui::painter& p) override {
        p.fill({ 0, 0, m_frame.w, 1 }, STLXTHEME_LINE_SUBTLE);
        m_actions.paint(p, FOOTER_PADDING_LEFT, m_frame.h, FOOTER_ITEM_GAP);

        int32_t end_w = m_end.width(FOOTER_KEY_GAP);
        m_end.paint(p, m_frame.w - FOOTER_PADDING_RIGHT - end_w, m_frame.h, FOOTER_KEY_GAP);
    }

private:
    footer_group m_actions;
    footer_group m_end;
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
    /* `chip_glyphs` holds each chip's glyph at rest, then chosen, and `no_match_note` says where files are searched,
     * under the words of a query that matched nothing */
    palette_view(std::vector<palette_entry> entries, const stlxgfx_surface_t* glyph,
                 const std::vector<stlxgfx_surface_t*>& chip_glyphs, std::string no_match_note);

    /* What a row can do, its first on Enter, and what Ctrl K's list notes about it. The first must be set before
     * `show_rows()`, since the footer names its actions. */
    std::function<std::vector<row_action>(const palette_entry& entry)> actions_of;
    std::function<std::string(const palette_entry& entry)> note_of;

    /* Ask the file index for the files a query matches or the entries of a folder, returning the request's number,
     * and make a file's row, found or listed. All must be set before the first key. */
    std::function<uint64_t(const std::string& query)> search_files;
    std::function<uint64_t(const std::string& folder, const std::string& prefix)> list_files;
    std::function<palette_entry(const found_file& file, bool is_listed)> make_file_entry;

    /* Shows File Manager with a configuration file selected. Must be set before the first key. */
    std::function<void(const std::string& path)> reveal;

    /* Shows the rows the query matches, first once the callbacks are set, then after each change */
    void show_rows();

    /* Shows an answer to the newest file request, dropping an older one, and the walk's count in the files' head */
    void show_files(const file_answer* answer, const walk_state& walk);

    /* Keeps the palette open with a strip saying what kept the selected row from starting, and the file its launcher
     * or action came from. A vanished file's row leaves the list, its strip heading the list instead. */
    void show_failure(const std::string& message, const std::string& source_path, const std::string& gone_path);

    bool focusable() const override { return true; }
    bool on_key_down(const ui::key_event& e) override;

private:
    void edit(std::string query, size_t cursor);
    void move_caret(size_t cursor);
    void open_menu();
    void close_menu();
    void show_menu();
    void reselect(const std::string& key);
    void narrow(narrow_mode mode);
    bool is_shown_kind(row_kind kind) const;
    void show_start();
    void show_kind(narrow_mode mode);
    void show_matches(std::string_view query);
    void show_more_files(size_t total, size_t shown);
    void show_listing(const typed_path& path);
    std::string files_head_text() const;
    void complete();
    bool go_up();
    void update_footer();
    bool find_calculation(std::string_view query);
    bool find_command(std::string_view query);
    section_head* add_head(const char* title);
    void add_row(const palette_entry& entry, std::vector<match_run> runs, const char* kind_name);
    void add_strip();
    bool is_failed_row() const;
    void select(size_t index);
    void move_selection(int32_t rows);
    void choose(size_t index);
    void run_keyed(const char* keys);
    void erase_word();
    std::string walk_text() const;

    std::vector<palette_entry> m_entries;
    std::string                m_query;
    size_t                     m_caret = 0;
    std::string                m_no_match_note;

    /* The rows of the query's value, of a program the query names with words after it, and of the files past
     * those shown, while `find_calculation` and `find_command` found the first two and the files ran over */
    palette_entry m_calculation;
    palette_entry m_command;
    palette_entry m_more_files;

    /* The kind the chips narrow the rows to */
    narrow_mode m_mode = narrow_mode::all;

    /* Ctrl K's list while it is open: the actions of the row it opened on, their choices, the row's label and a
     * note about the row */
    bool                       m_is_menu_open = false;
    std::vector<row_action>    m_menu_actions;
    std::vector<palette_entry> m_menu_choices;
    std::string                m_menu_title;
    std::string                m_menu_note;
    std::string                m_menu_return; /* the key of the row the list opened on */

    /* The failure a strip shows until the query changes: its reason, the file the row's launcher or action came from,
     * and the key of the row it shows under, none when the strip heads the list */
    struct failure {
        std::string message;
        std::string source_path;
        std::string row_key;
    };

    failure m_failure;

    /* The newest file request, a search or a listing, the last answer's rows, which the query filters until the
     * newest is answered, the folder a listing's rows are in, and the walk the files' head counts */
    uint64_t                   m_file_request = 0;
    bool                       m_is_listing = false;
    bool                       m_is_file_answer_newest = false;
    std::vector<palette_entry> m_file_entries;
    size_t                     m_file_total = 0;
    std::string                m_listed_folder;
    walk_state                 m_walk;
    section_head*              m_files_head = nullptr;
    size_t                     m_files_shown = 0;

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

palette_view::palette_view(std::vector<palette_entry> entries, const stlxgfx_surface_t* glyph,
                           const std::vector<stlxgfx_surface_t*>& chip_glyphs, std::string no_match_note)
    : ui::box(ui::axis::column), m_entries(std::move(entries)), m_no_match_note(std::move(no_match_note)) {
    ui::box* body = add<ui::box>(ui::axis::column);
    body->s().padding = ui::edge_insets::all(PALETTE_PADDING);
    body->s().gap = LIST_GAP;

    m_field = body->add<query_field>(glyph, chip_glyphs);
    m_field->on_chip = [this](narrow_mode mode) {
        narrow(mode);
    };

    m_list = body->add<result_list>();
    m_list->s().main = ui::length::fixed(LIST_HEIGHT);
    m_footer = add<palette_footer>();
}

/* Esc backs out one level, Ctrl K's list, then a narrowed list, then the palette, and Backspace in an empty field
 * backs out of narrowing too. Typing leaves Ctrl K's list for the rows. */
bool palette_view::on_key_down(const ui::key_event& e) {
    bool is_command = (e.modifiers & COMMAND_MODS) != 0;
    bool is_ctrl = (e.modifiers & CHORD_MOD_CTRL) != 0;
    if (is_ctrl && e.usage == HID_USAGE_K) {
        if (m_is_menu_open) {
            close_menu();
        } else {
            open_menu();
        }

        return true;
    }

    if (m_is_menu_open && e.usage == HID_USAGE_ESCAPE) {
        close_menu();
        return true;
    }

    bool is_chord = is_ctrl && (e.usage == HID_USAGE_C || e.usage == HID_USAGE_ENTER);
    if (m_is_menu_open && is_chord) {
        std::string keys = e.usage == HID_USAGE_C ? "Ctrl C" : "Ctrl Enter";
        for (row_action& action : m_menu_actions) {
            if (action.keys == keys) {
                action.run();
                break;
            }
        }

        return true;
    }

    bool is_menu_key = e.usage == HID_USAGE_UP || e.usage == HID_USAGE_DOWN || e.usage == HID_USAGE_PAGE_UP ||
                       e.usage == HID_USAGE_PAGE_DOWN || e.usage == HID_USAGE_ENTER;
    if (m_is_menu_open && !is_menu_key) {
        close_menu();
    }

    bool is_chip_key = is_ctrl && e.usage >= HID_USAGE_1 && e.usage < HID_USAGE_1 + CHIP_COUNT;
    if (is_chip_key) {
        narrow(CHIP_MODES[e.usage - HID_USAGE_1]);
        return true;
    }

    bool backs_out = e.usage == HID_USAGE_ESCAPE || (e.usage == HID_USAGE_BACKSPACE && m_query.empty());
    if (backs_out && m_mode != narrow_mode::all) {
        narrow(narrow_mode::all);
        return true;
    }

    if (is_chord) {
        run_keyed(e.usage == HID_USAGE_C ? "Ctrl C" : "Ctrl Enter");
        return true;
    }

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
        complete();
        return true;
    case HID_USAGE_DELETE:
        if (m_caret < m_query.size()) {
            edit(m_query.substr(0, m_caret) + m_query.substr(m_caret + 1), m_caret);
        }

        return true;
    case HID_USAGE_BACKSPACE:
        if (e.modifiers & CHORD_MOD_CTRL) {
            erase_word();
        } else if (!go_up() && m_caret > 0) {
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
    m_failure = {};
    m_field->show(m_query, m_caret);

    std::string_view searched = trimmed(m_query);
    typed_path path = parse_path(searched);
    m_is_listing = path.is_path;
    m_is_file_answer_newest = false;
    if (path.is_path) {
        m_file_request = list_files(path.folder, path.prefix);
    } else {
        m_file_request = searched.empty() ? 0 : search_files(std::string(searched));
    }

    show_rows();
}

/* An answer to an older request is dropped, and a finished walk asks again so the fresh index answers. The
 * selected row stays selected across an answer when it is still shown. */
void palette_view::show_files(const file_answer* answer, const walk_state& walk) {
    bool has_walk_finished = m_walk.walking && !walk.walking;
    m_walk = walk;
    if (m_files_head) {
        m_files_head->set_end(files_head_text());
    }

    std::string_view searched = trimmed(m_query);
    if (has_walk_finished && !searched.empty() && !m_is_listing) {
        m_file_request = search_files(std::string(searched));
    }

    if (!answer || answer->request != m_file_request) {
        return;
    }

    std::string selected_key = m_rows.empty() ? std::string() : row_key(*m_row_entries[m_selected]);
    m_file_entries.clear();
    for (const found_file& file : answer->files) {
        bool is_kept = std::any_of(m_entries.begin(), m_entries.end(), [&file](const palette_entry& entry) {
            return entry.kind == row_kind::file && entry.path == file.path;
        });

        if (m_is_listing || !is_kept) {
            m_file_entries.push_back(make_file_entry(file, m_is_listing));
        }
    }

    m_listed_folder = m_is_listing ? parse_path(searched).folder : std::string();
    m_file_total = answer->total;
    m_is_file_answer_newest = true;
    show_rows();
    reselect(selected_key);
}

/* A failure from Ctrl K's list closes the list, so the strip shows under the row the list opened on */
void palette_view::show_failure(const std::string& message, const std::string& source_path,
                                const std::string& gone_path) {
    std::string selected = m_rows.empty() ? std::string() : row_key(*m_row_entries[m_selected]);
    if (m_is_menu_open) {
        m_is_menu_open = false;
        selected = m_menu_return;
    }

    m_failure = { message, source_path, gone_path.empty() ? selected : std::string() };

    auto is_gone = [&gone_path](const palette_entry& entry) {
        return entry.kind == row_kind::file && entry.path == gone_path;
    };

    m_file_entries.erase(std::remove_if(m_file_entries.begin(), m_file_entries.end(), is_gone), m_file_entries.end());
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), is_gone), m_entries.end());
    show_rows();
    reselect(selected);
}

std::string palette_view::walk_text() const {
    return m_walk.walking ? "indexing, " + shown_number(static_cast<double>(m_walk.found)) + " so far" : "";
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

    m_files_head = nullptr;
    m_files_shown = 0;
    m_content_h = 0;
    m_selected = 0;

    std::string_view query = trimmed(m_query);
    typed_path path = parse_path(query);
    if (!m_failure.message.empty() && m_failure.row_key.empty()) {
        add_strip();
    }

    if (m_is_menu_open) {
        show_menu();
    } else if (path.is_path) {
        show_listing(path);
    } else if (!query.empty()) {
        show_matches(query);
    } else if (m_mode != narrow_mode::all) {
        show_kind(m_mode);
    } else {
        show_start();
    }

    select(0);
}

/* Ctrl K lists the selected row's actions in place of the rows, each with its chord, as Alfred lists its actions */
void palette_view::open_menu() {
    if (m_rows.empty()) {
        return;
    }

    const palette_entry& entry = *m_row_entries[m_selected];
    m_menu_actions = actions_of(entry);
    if (m_menu_actions.empty()) {
        return;
    }

    m_menu_title = entry.label;
    m_menu_note = note_of ? note_of(entry) : std::string();
    m_menu_return = row_key(entry);
    m_menu_choices.clear();
    for (size_t i = 0; i < m_menu_actions.size(); i++) {
        palette_entry choice;
        choice.kind = row_kind::choice;
        choice.choice = i;
        choice.label = m_menu_actions[i].label;
        for (size_t at = 0; at < m_menu_actions[i].keys.size();) {
            size_t end = std::min(m_menu_actions[i].keys.find(' ', at), m_menu_actions[i].keys.size());
            choice.keys.push_back(m_menu_actions[i].keys.substr(at, end - at));
            at = end + 1;
        }

        m_menu_choices.push_back(std::move(choice));
    }

    m_is_menu_open = true;
    show_rows();
}

/* Closing the list selects again the row it opened on */
void palette_view::close_menu() {
    m_is_menu_open = false;
    show_rows();
    reselect(m_menu_return);
}

void palette_view::reselect(const std::string& key) {
    for (size_t i = 0; i < m_rows.size(); i++) {
        if (row_key(*m_row_entries[i]) == key) {
            select(i);
            move_selection(0);
            return;
        }
    }
}

void palette_view::show_menu() {
    section_head* head = add_head("ACTIONS");
    head->set_end(m_menu_title);
    for (const palette_entry& choice : m_menu_choices) {
        add_row(choice, {}, nullptr);
    }

    if (!m_menu_note.empty()) {
        m_list->content()->add<note_line>(m_menu_note);
    }
}

/* A chip's key or click narrows the rows to its kind, and again, or narrowing to all, shows every kind */
void palette_view::narrow(narrow_mode mode) {
    m_mode = mode == m_mode ? narrow_mode::all : mode;
    m_field->show_mode(m_mode);
    show_rows();
}

bool palette_view::is_shown_kind(row_kind kind) const {
    switch (m_mode) {
    case narrow_mode::apps:
        return kind == row_kind::app || kind == row_kind::program;
    case narrow_mode::files:
        return kind == row_kind::file;
    case narrow_mode::actions:
        return kind == row_kind::action;
    case narrow_mode::all:
        break;
    }

    return true;
}

/* With nothing typed a chip lists its kind whole: the launchers and then the programs, every action, or this
 * session's picked files */
void palette_view::show_kind(narrow_mode mode) {
    std::vector<const palette_entry*> shown;
    for (const palette_entry& entry : m_entries) {
        if (is_shown_kind(entry.kind)) {
            shown.push_back(&entry);
        }
    }

    bool is_files = mode == narrow_mode::files;
    if (shown.empty()) {
        std::string text = is_files ? "Type a name to search the files" : "Nothing to list";
        m_list->content()->add<no_match>(text, is_files ? m_no_match_note : "");
        return;
    }

    for (size_t i = 0; i < shown.size(); i++) {
        if (i == 0 || shown[i]->kind != shown[i - 1]->kind) {
            section_head* head = add_head(KIND_HEADS[static_cast<size_t>(shown[i]->kind)]);
            m_files_head = shown[i]->kind == row_kind::file ? head : m_files_head;
        }

        add_row(*shown[i], {}, nullptr);
    }

    m_files_shown = is_files ? shown.size() : 0;
    if (m_files_head) {
        m_files_head->set_end(files_head_text());
    }
}

/* The files' head says how far a walk has come, and narrowed to files how many of the indexed show */
std::string palette_view::files_head_text() const {
    if (m_walk.walking) {
        return walk_text();
    }

    if (m_mode != narrow_mode::files || m_walk.found == 0) {
        return "";
    }

    return shown_number(static_cast<double>(m_files_shown)) + " of " +
           shown_number(static_cast<double>(m_walk.found)) + " indexed";
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

    std::vector<const palette_entry*> candidates;
    for (const palette_entry& entry : m_entries) {
        candidates.push_back(&entry);
    }

    for (const palette_entry& entry : m_file_entries) {
        if (m_listed_folder.empty()) {
            candidates.push_back(&entry);
        }
    }

    std::vector<found_entry> found;
    for (const palette_entry* entry : candidates) {
        query_match match = match_query(query, entry->label, entry->keywords);
        if (match.quality != match_quality::none && is_shown_kind(entry->kind)) {
            found.push_back({ entry, std::move(match) });
        }
    }

    /* Nothing matching says so only once the files for this very query are in */
    bool is_all = m_mode == narrow_mode::all;
    bool has_calculation = is_all && find_calculation(query);
    bool has_command = (is_all || m_mode == narrow_mode::apps) && find_command(query);
    if (found.empty() && !has_calculation && !has_command) {
        if (m_is_file_answer_newest) {
            std::string text = "No apps, actions or files match " + std::string(query);
            m_list->content()->add<no_match>(text, m_no_match_note);
        }

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
    if (is_all && !has_calculation && !has_command) {
        add_head("TOP HIT");
        const found_entry& top = found.front();
        add_row(*top.entry, top.match.runs, KIND_NAMES[static_cast<size_t>(top.entry->kind)]);
        first_rest = 1;
    }

    size_t rows_max = is_all ? KIND_ROWS_MAX : NARROWED_ROWS_MAX;
    for (row_kind kind : { row_kind::app, row_kind::action, row_kind::program, row_kind::file }) {
        size_t shown = 0;
        size_t matched = 0;
        for (size_t i = first_rest; i < found.size(); i++) {
            if (found[i].entry->kind != kind) {
                continue;
            }

            matched++;
            if (shown == rows_max) {
                continue;
            }

            if (shown == 0) {
                section_head* head = add_head(KIND_HEADS[static_cast<size_t>(kind)]);
                m_files_head = kind == row_kind::file ? head : m_files_head;
            }

            add_row(*found[i].entry, found[i].match.runs, nullptr);
            shown++;
        }

        /* The index's count holds the top hit too when the top hit is a file */
        if (kind == row_kind::file) {
            bool is_top_file = first_rest == 1 && found.front().entry->kind == row_kind::file;
            size_t found_files = matched + (is_top_file ? 1 : 0);
            size_t total = m_is_file_answer_newest ? std::max(m_file_total, found_files) : found_files;
            m_files_shown = shown;
            show_more_files(total - (is_top_file ? 1 : 0), shown);
        }
    }

    if (m_files_head) {
        m_files_head->set_end(files_head_text());
    }
}

/* With every kind showing, files past the six shown are offered by one last row, which narrows to files */
void palette_view::show_more_files(size_t total, size_t shown) {
    if (m_mode != narrow_mode::all || total <= shown || shown == 0) {
        return;
    }

    size_t rest = total - shown;
    m_more_files = palette_entry();
    m_more_files.kind = row_kind::more_files;
    m_more_files.label = "Show " + shown_number(static_cast<double>(rest)) + (rest == 1 ? " more file" : " more files");
    m_more_files.tile = "+";
    m_more_files.keys = { "Ctrl", "2" };

    add_row(m_more_files, {}, nullptr);
}

/* A typed path lists its folder's entries, folders first, whose names start with what follows its last slash */
void palette_view::show_listing(const typed_path& path) {
    std::vector<std::pair<const palette_entry*, query_match>> shown;
    if (m_listed_folder == path.folder) {
        for (const palette_entry& entry : m_file_entries) {
            query_match match = match_query(path.prefix, entry.label, "");
            if (path.prefix.empty() || match.quality == match_quality::prefix) {
                shown.emplace_back(&entry, std::move(match));
            }
        }
    }

    std::string folder = shown_folder(path.folder);
    if (shown.empty()) {
        if (m_is_file_answer_newest) {
            std::string text = path.prefix.empty() ? "Nothing to show in " + folder
                                                   : "Nothing in " + folder + " starts with " + path.prefix;
            m_list->content()->add<no_match>(text, "");
        }

        return;
    }

    add_head(("IN " + folder).c_str());
    for (auto& [entry, match] : shown) {
        add_row(*entry, std::move(match.runs), nullptr);
    }
}

/* Tab completes the query to the selected row's name, or to a file's path, which browses its folder. A typed path
 * keeps its folder as typed, and a folder's slash lists the folder. */
void palette_view::complete() {
    row_kind kind = m_rows.empty() ? row_kind::more_files : m_row_entries[m_selected]->kind;
    if (kind == row_kind::more_files || kind == row_kind::choice) {
        return;
    }

    const palette_entry& entry = *m_row_entries[m_selected];
    bool is_folder = entry.mime_type && strcmp(entry.mime_type, FOLDER_TYPE) == 0;
    typed_path path = parse_path(trimmed(m_query));

    std::string completed = entry.kind == row_kind::file ? entry.path : entry.label;
    completed = path.is_path ? path.typed_folder + entry.label : completed;
    completed += is_folder ? "/" : "";
    edit(completed, completed.size());
}

/* Backspace right after a typed folder's slash goes up to its parent */
bool palette_view::go_up() {
    bool is_after_slash = m_caret == m_query.size() && m_query.size() > 1 && m_query.back() == '/';
    if (!is_after_slash || !parse_path(trimmed(m_query)).is_path) {
        return false;
    }

    size_t parent_end = m_query.rfind('/', m_query.size() - 2);
    if (parent_end == std::string::npos) {
        return false;
    }

    edit(m_query.substr(0, parent_end + 1), parent_end + 1);
    return true;
}

/* Enter's action for the selected row, and for a typed path how to complete a name and go up. The right side names
 * the chips' keys, or Esc while they narrow the rows. */
void palette_view::update_footer() {
    if (m_is_menu_open) {
        m_footer->show({ { "Choose", "Enter" } }, { { "Back", "Esc" } });
        return;
    }

    std::vector<footer_item> items;
    bool is_path = parse_path(trimmed(m_query)).is_path;
    if (is_failed_row()) {
        bool is_launcher = m_row_entries[m_selected]->kind == row_kind::app;
        items.push_back({ "Try Again", "Enter" });
        if (!m_failure.source_path.empty()) {
            items.push_back({ is_launcher ? "Show Launcher File" : "Show Action File", "Ctrl Enter" });
        }
    } else if (!m_rows.empty()) {
        const palette_entry& entry = *m_row_entries[m_selected];
        std::vector<row_action> actions = actions_of(entry);
        for (const row_action& action : actions) {
            bool is_named = action.keys == "Enter" || (action.keys == "Ctrl Enter" && !is_path);
            if (is_named) {
                items.push_back({ action.label, action.keys });
            }
        }

        if (actions.empty()) {
            items.push_back({ KIND_VERBS[static_cast<size_t>(entry.kind)], "Enter" });
        } else if (!is_path) {
            items.push_back({ "Actions", "Ctrl K" });
        }
    }

    if (is_path) {
        items.push_back({ "Complete", "Tab" });
        items.push_back({ "Up a folder", "Backspace" });
    }

    std::vector<footer_item> end;
    if (m_mode != narrow_mode::all) {
        end.push_back({ "All kinds", "Esc" });
    } else if (!is_path) {
        end.push_back({ "Narrow", "Ctrl 1" });
        end.push_back({ "to", "Ctrl 3" });
    }

    m_footer->show(items, end);
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
        m_command.subtitle = entry.path + " in Terminal";
        m_command.is_subtitle_path = false;
        m_command.args = std::string(trimmed(query.substr(space + 1)));
        m_command.keys.clear();

        return true;
    }

    return false;
}

section_head* palette_view::add_head(const char* title) {
    section_head* head = m_list->content()->add<section_head>(title);
    m_content_h += HEAD_HEIGHT;
    m_follows_head = true;

    return head;
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

    if (!m_failure.message.empty() && row_key(entry) == m_failure.row_key) {
        add_strip();
    }
}

void palette_view::add_strip() {
    m_list->content()->add<failure_strip>(m_failure.message);
    m_content_h += STRIP_MARGIN_TOP + STRIP_HEIGHT;
}

void palette_view::select(size_t index) {
    if (m_rows.empty()) {
        update_footer();
        return;
    }

    m_rows[m_selected]->set_selected(false);
    m_selected = std::min(index, m_rows.size() - 1);
    m_rows[m_selected]->set_selected(true);
    update_footer();
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

/* The row of the files past those shown narrows to files instead of starting anything */
void palette_view::choose(size_t index) {
    if (index >= m_rows.size()) {
        return;
    }

    select(index);
    const palette_entry& entry = *m_row_entries[index];
    if (entry.kind == row_kind::more_files) {
        narrow(narrow_mode::files);
        return;
    }

    if (entry.kind == row_kind::choice) {
        m_menu_actions[entry.choice].run();
        return;
    }

    std::vector<row_action> actions = actions_of(entry);
    if (!actions.empty()) {
        actions.front().run();
    }
}

bool palette_view::is_failed_row() const {
    bool has_row_strip = !m_failure.message.empty() && !m_failure.row_key.empty();
    return has_row_strip && !m_rows.empty() && row_key(*m_row_entries[m_selected]) == m_failure.row_key;
}

/* Runs the selected row's action `keys` name, where a calculation copies on Ctrl C as on Enter, and Ctrl Enter on a
 * row that failed to start shows the file its launcher or action came from */
void palette_view::run_keyed(const char* keys) {
    if (m_rows.empty()) {
        return;
    }

    bool shows_source = strcmp(keys, "Ctrl Enter") == 0 && is_failed_row() && !m_failure.source_path.empty();
    if (shows_source) {
        reveal(m_failure.source_path);
        return;
    }

    const palette_entry& entry = *m_row_entries[m_selected];
    std::vector<row_action> actions = actions_of(entry);
    for (row_action& action : actions) {
        if (action.keys == keys) {
            action.run();
            return;
        }
    }

    bool copies_first = entry.kind == row_kind::calculator && strcmp(keys, "Ctrl C") == 0;
    if (copies_first && !actions.empty()) {
        actions.front().run();
    }
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
            program.path = path;
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

/* A configuration file as a strip names it, relative to stlxdm's own folder, such as conf.d/browser.conf */
std::string shown_source(const std::string& path) {
    std::string folder = STLXCONF_DROP_IN_DIR;
    folder = folder.substr(0, folder.rfind('/') + 1);

    return path.compare(0, folder.size(), folder) == 0 ? path.substr(folder.size()) : path;
}

/* The label of the first launcher that runs `path`, else the program's file name */
std::string program_label(const stlxconf_t& conf, const char* path) {
    for (uint32_t i = 0; i < conf.pin_count; i++) {
        if (strcmp(conf.pins[i].path, path) == 0 && conf.pins[i].label[0]) {
            return conf.pins[i].label;
        }
    }

    return file_name(path);
}

std::string items_text(uint64_t count) {
    return count == 1 ? "1 item" : shown_number(static_cast<double>(count)) + " items";
}

/* Where files are searched, from the skip list, said under a query that matched nothing */
std::string skip_note(const char* skip) {
    std::vector<std::string> folders;
    for (const char* at = skip; *at;) {
        const char* end = strchr(at, ' ');
        end = end ? end : at + strlen(at);
        if (end > at) {
            folders.emplace_back(at, end);
        }

        at = *end ? end + 1 : end;
    }

    std::string note = "Files are searched in /";
    for (size_t i = 0; i < folders.size(); i++) {
        note += i == 0 ? " except " : i + 1 == folders.size() ? " and " : ", ";
        note += folders[i];
    }

    return folders.empty() ? note : note + ", the [quick_actions] skip list";
}

/* The icons file rows show, by what the describer said each file is */
struct file_icons {
    const stlxgfx_surface_t* folder;
    const stlxgfx_surface_t* text;
    const stlxgfx_surface_t* image;
    const stlxgfx_surface_t* program;
};

const stlxgfx_surface_t* icon_of_type(const file_icons& icons, const char* mime_type) {
    if (strcmp(mime_type, FOLDER_TYPE) == 0) {
        return icons.folder;
    }

    if (strncmp(mime_type, "image/", 6) == 0) {
        return icons.image;
    }

    bool is_program = strcmp(mime_type, "application/x-executable") == 0 ||
                      strcmp(mime_type, "application/x-shellscript") == 0;
    return is_program ? icons.program : icons.text;
}

/* A found file's row shows the folder it is in, which tells apart two files of one name, and a listed one, whose
 * folder every row shares, its first entries or its type. Its size shows at the right. */
palette_entry file_entry(const found_file& file, const file_icons& icons, std::string verb, bool is_listed) {
    palette_entry entry;
    entry.kind = row_kind::file;
    entry.label = std::string(file.name());
    entry.path = file.path;
    entry.mime_type = file.mime_type ? file.mime_type : "application/octet-stream";
    entry.file = file;

    std::string folder = file.name_start > 1 ? file.path.substr(0, file.name_start - 1) : "/";
    std::string listed = file.is_folder ? file.preview : file.description ? file.description : "";
    entry.subtitle = is_listed ? listed : folder;
    entry.is_subtitle_path = !is_listed || file.is_folder;
    entry.right = file.is_folder ? items_text(file.size) : size_text(file.size);

    entry.verb = std::move(verb);
    entry.icon = icon_of_type(icons, entry.mime_type);

    return entry;
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

/* The index names each answered file's type on its searcher, so a row never reads a file on this thread */
quick_actions::quick_actions(const stlxconf_t& conf, const std::vector<stlxgfx_surface_t*>& launcher_icons)
    : m_conf(conf), m_launcher_icons(launcher_icons),
      m_search_glyph(make_search_glyph(FIELD_GLYPH_SIZE, STLXTHEME_TEXT_DIM)),
      m_program_icon(load_icon(PROGRAM_ICON_PATH)), m_folder_icon(load_icon(FOLDER_ICON_PATH)),
      m_text_icon(load_icon(TEXT_ICON_PATH)), m_image_icon(load_icon(IMAGE_ICON_PATH)) {
    for (uint32_t i = 0; i < conf.action_count; i++) {
        const char* path = conf.actions[i].icon_path;
        m_action_icons.push_back(path[0] ? load_icon(path) : nullptr);
    }

    for (uint32_t color : { STLXTHEME_TEXT_DIM, STLXTHEME_TEXT }) {
        for (narrow_mode mode : CHIP_MODES) {
            m_chip_glyphs.push_back(make_shape_glyph(CHIP_GLYPH_SIZE, color, chip_contours(mode)));
        }
    }

    bool is_started = m_files.start("/", conf.quick_actions_skip, [](found_file& file) {
        if (file.is_folder) {
            file.mime_type = FOLDER_TYPE;
            file.description = "Folder";
            return;
        }

        stlxconf_file_type_t type = stlxconf_file_type(file.path.c_str());
        file.mime_type = type.mime_type;
        file.description = type.description;
    });

    if (!is_started) {
        printf("stlxdm: Quick Actions finds no files, since its file index could not start\r\n");
    }
}

quick_actions::~quick_actions() {
    for (stlxgfx_surface_t* icon : m_action_icons) {
        stlxgfx_destroy_surface(icon);
    }

    for (stlxgfx_surface_t* glyph : m_chip_glyphs) {
        stlxgfx_destroy_surface(glyph);
    }

    for (stlxgfx_surface_t* icon : { m_search_glyph, m_program_icon, m_folder_icon, m_text_icon, m_image_icon }) {
        stlxgfx_destroy_surface(icon);
    }
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

/* A picked file is kept as found, since no answer may hold it at the next opening */
void quick_actions::remember_file(const found_file& file) {
    auto same_path = [&file](const found_file& kept) {
        return kept.path == file.path;
    };

    m_picked_files.erase(std::remove_if(m_picked_files.begin(), m_picked_files.end(), same_path), m_picked_files.end());
    m_picked_files.insert(m_picked_files.begin(), file);
    if (m_picked_files.size() > RECENT_MAX) {
        m_picked_files.pop_back();
    }
}

void quick_actions::closed() {
    m_show_files = nullptr;
    m_show_failure = nullptr;
}

void quick_actions::pump_files() {
    file_answer answer;
    walk_state walk;
    bool has_answer = m_files.take(&answer, &walk);
    if (m_show_files) {
        m_show_files(has_answer ? &answer : nullptr, walk);
    }
}

/* A folder opens in File Manager, and a file in the handler of its type or the nearest it derives from, else File
 * Manager shows it selected in its folder */
void quick_actions::open_file(const std::string& path, const char* mime_type) {
    bool is_folder = strcmp(mime_type, FOLDER_TYPE) == 0;
    const stlxconf_file_handler_t* handler = nullptr;
    if (!is_folder) {
        handler = stlxconf_find_file_opener(&m_conf, mime_type, nullptr);
    }

    if (!handler) {
        on_exec(FILE_MANAGER_PATH, { path });
        return;
    }

    open_with(*handler, path);
}

void quick_actions::open_with(const stlxconf_file_handler_t& handler, const std::string& path) {
    char args[sizeof(handler.args)];
    memcpy(args, handler.args, sizeof(args));
    const char* argv[HANDLER_ARGS_MAX];
    int argc = stlxconf_split_args(args, argv, HANDLER_ARGS_MAX);

    std::vector<std::string> handler_args(argv, argv + argc);
    handler_args.push_back(path);
    on_exec(handler.path, handler_args);
}

/* A program that is missing, or is no program, keeps the palette open with a strip that says so and names the file
 * its launcher or action came from, which Ctrl Enter then shows */
bool quick_actions::is_startable(const char* verb, const std::string& label, const char* path, const char* owner,
                                 const char* source_path) {
    struct stat st;
    bool exists = stat(path, &st) == 0;
    if (exists && S_ISREG(st.st_mode)) {
        return true;
    }

    std::string message = "Couldn't " + std::string(verb) + " " + label + ": " + path;
    message += exists ? " isn't a program." : " is missing.";
    std::string source = source_path ? source_path : "";
    if (owner && !source.empty()) {
        message += " Its " + std::string(owner) + " is " + shown_source(source) + ".";
    }

    if (m_show_failure) {
        m_show_failure(message, source, "");
    }

    return false;
}

/* A file gone since the walk leaves the list and this session's picks, and its strip says so */
bool quick_actions::is_still_there(const found_file& file) {
    struct stat st;
    if (stat(file.path.c_str(), &st) == 0) {
        return true;
    }

    m_picks.erase(std::remove(m_picks.begin(), m_picks.end(), "file " + file.path), m_picks.end());
    m_picked_files.erase(std::remove_if(m_picked_files.begin(), m_picked_files.end(), [&file](const found_file& kept) {
        return kept.path == file.path;
    }), m_picked_files.end());

    if (m_show_failure) {
        std::string folder = file.name_start > 1 ? file.path.substr(0, file.name_start - 1) : "/";
        m_show_failure(std::string(file.name()) + " is gone from " + folder + " since the files were indexed.", "",
                       file.path);
    }

    return false;
}

row_action quick_actions::show_in_files(const std::string& path) {
    return { "Show in File Manager", "Ctrl Enter", [this, path]() {
        on_exec(FILE_MANAGER_PATH, { path });
    } };
}

row_action quick_actions::copy(const std::string& label, const std::string& text) {
    return { label, "Ctrl C", [this, text]() {
        on_copy(text);
    } };
}

/* A launcher starts as the dock starts it, and its program shows in File Manager or copies as a path */
std::vector<row_action> quick_actions::launcher_actions(uint32_t index, const std::string& pick) {
    const stlxconf_pin_t& pin = m_conf.pins[index];
    std::string label = pin.label[0] ? pin.label : file_name(pin.path);
    row_action open = { "Open", "Enter", [this, &pin, label, pick]() {
        if (!is_startable("start", label, pin.path, "launcher", pin.source_path)) {
            return;
        }

        remember_pick(pick);
        on_launch(pin);
    } };

    return { open, show_in_files(pin.path), copy("Copy Path", pin.path) };
}

std::vector<row_action> quick_actions::action_actions(uint32_t index, const std::string& pick) {
    const stlxconf_action_t& action = m_conf.actions[index];
    bool runs_program = stlxconf_action_kind(action.action) == STLXCONF_ACTION_EXEC;
    std::string label = action.label[0] ? action.label : action.name;
    std::vector<row_action> actions = { { "Run", "Enter", [this, &action, runs_program, label, pick]() {
        if (runs_program && !is_startable("run", label, action.path, "action", action.source_path)) {
            return;
        }

        remember_pick(pick);
        on_run(action);
    } } };

    if (runs_program) {
        std::string command = action.args[0] ? std::string(action.path) + " " + action.args : action.path;
        actions.push_back(copy("Copy Command", command));
    }

    return actions;
}

std::vector<row_action> quick_actions::program_actions(const std::string& path, const std::string& args,
                                                       const std::string& pick) {
    row_action run = { "Run in Terminal", "Enter", [this, path, args, pick]() {
        if (!is_startable("run", file_name(path.c_str()), path.c_str(), nullptr, nullptr)) {
            return;
        }

        remember_pick(pick);
        on_run_program(path.c_str(), args.c_str());
    } };

    return { run, show_in_files(path), copy("Copy Path", path) };
}

/* A file opens as Enter says, then in the handler of each type further up its chain, which is Open With, and copies
 * as its path or as a file */
std::vector<row_action> quick_actions::file_actions(const found_file& file, const std::string& pick) {
    const char* mime_type = file.mime_type ? file.mime_type : "application/octet-stream";
    row_action open = { file_verb(mime_type), "Enter", [this, file, mime_type, pick]() {
        if (!is_still_there(file)) {
            return;
        }

        const stlxconf_file_handler_t* handler = nullptr;
        if (strcmp(mime_type, FOLDER_TYPE) != 0) {
            handler = stlxconf_find_file_opener(&m_conf, mime_type, nullptr);
        }

        if (handler && !is_startable("open", std::string(file.name()), handler->path, nullptr, nullptr)) {
            return;
        }

        remember_pick(pick);
        remember_file(file);
        open_file(file.path, mime_type);
    } };

    const char* handled = nullptr;
    const stlxconf_file_handler_t* first = nullptr;
    bool is_folder = strcmp(mime_type, FOLDER_TYPE) == 0;
    if (!is_folder) {
        first = stlxconf_find_file_opener(&m_conf, mime_type, &handled);
    }

    /* A file no program opens already shows in File Manager on Enter */
    std::vector<row_action> actions = { open };
    if (first || is_folder) {
        actions.push_back(show_in_files(file.path));
    }

    std::vector<std::string> offered = { first ? first->path : "" };
    for (const char* type = first ? stlxconf_parent_type(handled) : nullptr; type; type = stlxconf_parent_type(type)) {
        const stlxconf_file_handler_t* other = stlxconf_find_file_handler(&m_conf, type);
        if (!other || std::find(offered.begin(), offered.end(), other->path) != offered.end()) {
            continue;
        }

        offered.push_back(other->path);
        actions.push_back({ "Open in " + program_label(m_conf, other->path), "", [this, other, path = file.path]() {
            open_with(*other, path);
        } });
    }

    actions.push_back(copy("Copy Path", file.path));
    actions.push_back({ is_folder ? "Copy Folder" : "Copy File", "", [this, file]() {
        if (is_still_there(file)) {
            on_copy_file(file.path);
        }
    } });

    return actions;
}

std::string quick_actions::file_verb(const char* mime_type) const {
    if (strcmp(mime_type, FOLDER_TYPE) == 0) {
        return "Open in File Manager";
    }

    const stlxconf_file_handler_t* handler = stlxconf_find_file_opener(&m_conf, mime_type, nullptr);
    return handler ? "Open in " + program_label(m_conf, handler->path) : "Show in File Manager";
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

    file_icons icons = { m_folder_icon, m_text_icon, m_image_icon, m_program_icon };
    for (const found_file& file : m_picked_files) {
        entries.push_back(file_entry(file, icons, file_verb(file.mime_type), false));
    }

    for (palette_entry& entry : entries) {
        auto pick = std::find(m_picks.begin(), m_picks.end(), pick_key(entry));
        entry.recent = pick == m_picks.end() ? UNPICKED : static_cast<uint32_t>(pick - m_picks.begin());
    }

    m_files.rewalk();
    auto view = std::make_unique<palette_view>(std::move(entries), m_search_glyph, m_chip_glyphs,
                                               skip_note(m_conf.quick_actions_skip));
    view->search_files = [this](const std::string& query) {
        return m_files.search(query, FILE_RESULTS_MAX);
    };

    view->list_files = [this](const std::string& folder, const std::string& prefix) {
        return m_files.list(folder, prefix);
    };

    view->make_file_entry = [this, icons](const found_file& file, bool is_listed) {
        return file_entry(file, icons, file_verb(file.mime_type), is_listed);
    };

    palette_view* shown = view.get();
    m_show_files = [shown](const file_answer* answer, const walk_state& walk) {
        shown->show_files(answer, walk);
    };

    m_show_failure = [shown](const std::string& message, const std::string& source_path, const std::string& gone_path) {
        shown->show_failure(message, source_path, gone_path);
    };

    view->reveal = [this](const std::string& path) {
        on_exec(FILE_MANAGER_PATH, { path });
    };

    view->actions_of = [this](const palette_entry& entry) -> std::vector<row_action> {
        std::string pick = pick_key(entry);
        switch (entry.kind) {
        case row_kind::app:
            return launcher_actions(entry.index, pick);
        case row_kind::action:
            return action_actions(entry.index, pick);
        case row_kind::program:
            return program_actions(entry.path, entry.args, pick);
        case row_kind::file:
            return file_actions(entry.file, pick);
        case row_kind::calculator:
            return { { "Copy", "Enter", [this, value = entry.copied]() {
                on_copy(value);
            } } };
        case row_kind::more_files:
        case row_kind::choice:
            break;
        }

        return {};
    };

    view->note_of = [this](const palette_entry& entry) {
        return entry.kind == row_kind::file ? file_note(entry.file) : std::string();
    };

    view->show_rows();
    return view;
}

/* Why a file opens where Enter opens it: the handler of its own type, of the nearest type it derives from, or none */
std::string quick_actions::file_note(const found_file& file) const {
    const char* mime_type = file.mime_type ? file.mime_type : "application/octet-stream";
    if (strcmp(mime_type, FOLDER_TYPE) == 0) {
        return "";
    }

    const char* handled = nullptr;
    const stlxconf_file_handler_t* handler = stlxconf_find_file_opener(&m_conf, mime_type, &handled);
    if (!handler) {
        return std::string("No program opens ") + mime_type + " files, so Enter shows it in File Manager.";
    }

    std::string opener = program_label(m_conf, handler->path) + " opens it as the handler for " + handled;
    if (strcmp(handled, mime_type) == 0) {
        return opener + ".";
    }

    return opener + ", the nearest type " + mime_type + " derives from.";
}
