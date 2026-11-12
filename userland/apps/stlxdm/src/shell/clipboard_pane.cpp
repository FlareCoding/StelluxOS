#include "clipboard_pane.hpp"

#include <stlxtheme/theme.h>

#include <algorithm>
#include <ctime>
#include <memory>

constexpr int32_t COLUMN_WIDTH = 304;
constexpr int32_t COLUMN_PADDING_RIGHT = 4;
constexpr int32_t ENTRY_HEIGHT = 48;
constexpr int32_t ENTRY_PADDING_LEFT = 8;
constexpr int32_t ENTRY_PADDING_RIGHT = 10;
constexpr int32_t ENTRY_ICON_SIZE = STLXTHEME_DOCK_ICON_SIZE;
constexpr int32_t ENTRY_TEXT_LEFT = ENTRY_PADDING_LEFT + ENTRY_ICON_SIZE + 12;
constexpr int32_t ENTRY_TEXT_W = COLUMN_WIDTH - COLUMN_PADDING_RIGHT - ENTRY_TEXT_LEFT - ENTRY_PADDING_RIGHT;
constexpr int32_t ENTRY_TITLE_HEIGHT = 18;
constexpr float ENTRY_TEXT_SIZE = 12.5f;

constexpr int32_t PREVIEW_PADDING = 16;
constexpr int32_t PREVIEW_HEAD_HEIGHT = 34;
constexpr int32_t PREVIEW_HEAD_GAP = 6;
constexpr int32_t TAG_HEIGHT = 20;
constexpr int32_t TAG_PADDING_X = 8;
constexpr int32_t POCKET_PADDING = 14;
constexpr int32_t POCKET_LINE_HEIGHT = 20;
constexpr int32_t META_HEIGHT = 30;
constexpr int32_t META_GAP = 18;
constexpr float META_SIZE = 11.5f;

constexpr int32_t EMPTY_GAP = 6;

/* A preview shows a text's first 64 KB, and a line's first bytes are all the pocket can show */
constexpr uint64_t PREVIEW_BYTES_MAX = 64 * 1024;
constexpr size_t TITLE_BYTES_MAX = 256;
constexpr size_t LINE_BYTES_MAX = 512;
constexpr size_t TAB_WIDTH = 4;
constexpr int32_t PAGE_ENTRIES = 7;

constexpr int64_t NS_PER_MINUTE = 60ll * 1000000000ll;

constexpr uint16_t HID_USAGE_ENTER = 0x28;
constexpr uint16_t HID_USAGE_PAGE_UP = 0x4B;
constexpr uint16_t HID_USAGE_PAGE_DOWN = 0x4E;
constexpr uint16_t HID_USAGE_DOWN = 0x51;
constexpr uint16_t HID_USAGE_UP = 0x52;
constexpr uint8_t  MOD_CTRL = 1u << 1;

/* The entries under their section heads, scrolled to keep the selected one in view */
class clip_list : public ui::scroll_view {
public:
    clip_list() {
        m_content = add<ui::box>(ui::axis::column);
    }

    ui::box* content() { return m_content; }

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

/* The selected entry exactly: its kind, where and when it came from, its text in a pocket, and a line about it */
class clip_preview : public ui::widget {
public:
    clip_preview();

    void show(const shown_entry& entry, int64_t now_ns);
    void paint(ui::painter& p) override;

private:
    text_picture                               m_kind;
    text_picture                               m_from;
    text_picture                               m_tag;
    bool                                       m_is_current = false;
    std::vector<std::unique_ptr<text_picture>> m_lines;
    std::vector<std::unique_ptr<text_picture>> m_meta;
};

/* One entry: its icon, its first line or what it is, and where and when it was copied */
class clip_row : public ui::widget {
public:
    clip_row(const shown_entry& entry, const std::string& detail);

    std::function<void()> on_hover;
    std::function<void()> on_choose;

    void set_selected(bool is_selected) {
        if (is_selected != m_is_selected) {
            m_is_selected = is_selected;
            invalidate();
        }
    }

    ui::size measure(ui::size avail) override {
        return { avail.w, ENTRY_HEIGHT };
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
    const stlxgfx_surface_t* m_icon;
    text_picture             m_title;
    text_picture             m_detail;
    bool                     m_is_selected = false;
};

namespace {

/* What shows while nothing is listed, and what pins are for */
class clip_empty : public ui::widget {
public:
    clip_empty()
        : m_title("Nothing copied since Stellux started", STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE,
                  STLXTHEME_TEXT),
          m_note("Copies appear here as you make them, newest first. Pinned items stay until you unpin them, or until "
                 "Stellux restarts.", STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_SECONDARY) {}

    void paint(ui::painter& p) override {
        int32_t top = (m_frame.h - 2 * ENTRY_TITLE_HEIGHT - EMPTY_GAP) / 2;
        m_title.draw(p, (m_frame.w - m_title.width()) / 2, top, ENTRY_TITLE_HEIGHT);
        m_note.draw(p, (m_frame.w - m_note.width()) / 2, top + ENTRY_TITLE_HEIGHT + EMPTY_GAP, ENTRY_TITLE_HEIGHT);
    }

private:
    text_picture m_title;
    text_picture m_note;
};

/* The hairline between the entries and the preview */
class column_rule : public ui::widget {
public:
    void paint(ui::painter& p) override {
        p.fill({ 0, 0, 1, m_frame.h }, STLXTHEME_LINE_SUBTLE);
    }
};

} // namespace

static int64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}

/* How long ago a copy was made, as the history's rows say it */
static std::string age_text(int64_t age_ns) {
    int64_t minutes = age_ns / NS_PER_MINUTE;
    if (minutes < 1) {
        return "now";
    }

    if (minutes < 60) {
        return std::to_string(minutes) + " min ago";
    }

    int64_t hours = minutes / 60;
    return hours < 24 ? std::to_string(hours) + " h ago" : std::to_string(hours / 24) + " days ago";
}

/* A line as the pocket draws it, its tabs widened to the next stop and its end cut past what a row can show */
static std::string expanded(std::string_view line) {
    std::string out;
    for (char c : line.substr(0, LINE_BYTES_MAX)) {
        if (c == '\t') {
            out.append(TAB_WIDTH - out.size() % TAB_WIDTH, ' ');
        } else if (c != '\r') {
            out += c;
        }
    }

    return out;
}

static size_t line_count(std::string_view text) {
    size_t breaks = static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
    return breaks + (!text.empty() && text.back() != '\n' ? 1 : 0);
}

static size_t character_count(std::string_view text) {
    return static_cast<size_t>(std::count_if(text.begin(), text.end(), [](char c) {
        return (static_cast<uint8_t>(c) & 0xC0) != 0x80;
    }));
}

/* The first line holding more than spaces, which names a text in its row */
static std::string first_line(std::string_view text) {
    size_t at = 0;
    while (at < text.size()) {
        size_t end = std::min(text.find('\n', at), text.size());
        std::string line = expanded(text.substr(at, std::min(end - at, TITLE_BYTES_MAX)));
        std::string_view shown = trimmed(line);
        if (!shown.empty()) {
            return std::string(shown);
        }

        at = end + 1;
    }

    return "Blank text";
}

/* A text that is one web address and nothing else */
static bool is_link(std::string_view text) {
    std::string_view word = trimmed(text);
    bool has_scheme = word.starts_with("https://") || word.starts_with("http://");
    return has_scheme && word.find_first_of(" \t\r\n") == std::string_view::npos;
}

/* An entry shows the icon of the launcher whose label it came from, else the text icon */
static const stlxgfx_surface_t* icon_for(const clipboard_source& source, const std::string& from) {
    for (uint32_t i = 0; i < source.conf->pin_count; i++) {
        bool has_icon = i < source.launcher_icons->size() && (*source.launcher_icons)[i];
        if (has_icon && from == source.conf->pins[i].label) {
            return (*source.launcher_icons)[i];
        }
    }

    return source.text_icon;
}

static shown_entry show_entry(const clipboard_source& source, const clipboard_entry& entry) {
    shown_entry shown;
    shown.id = entry.id;
    shown.from = entry.label;
    shown.copied_ns = entry.copied_ns;
    shown.is_current = source.history->is_current(entry.id);
    shown.is_pinned = entry.pinned;

    shown.size = entry.item.size();
    shown.icon = icon_for(source, entry.label);
    for (const clipboard_rep& rep : entry.item.reps()) {
        shown.types.push_back(rep.mime_type);
    }

    const clipboard_rep* text = entry.item.find(SWP_MIME_TEXT);
    if (text) {
        shown.text = read_bytes(*text, PREVIEW_BYTES_MAX);
        shown.text_len = text->len;
        shown.kind = is_link(shown.text) ? "Link" : "Text";
        shown.title = first_line(shown.text);
        shown.is_title_text = true;
        return shown;
    }

    shown.kind = "Data";
    shown.title = shown.types.empty() ? "Empty" : shown.types.front();
    return shown;
}

/* How long a text of more than one line is: its lines, or its size past the 64 KB a preview reads, whose lines alone
 * were counted */
static std::string text_length(const shown_entry& entry) {
    if (!entry.is_title_text) {
        return "";
    }

    if (entry.text_len > PREVIEW_BYTES_MAX) {
        return size_text(entry.text_len);
    }

    size_t lines = line_count(entry.text);
    return lines > 1 ? std::to_string(lines) + " lines" : "";
}

/* Where an entry came from and when, and how long a text of more than one line is */
static std::string detail_of(const shown_entry& entry, int64_t now_ns) {
    std::string detail = entry.from + ", " + age_text(now_ns - entry.copied_ns);
    std::string length = text_length(entry);
    if (!length.empty()) {
        detail += ", " + length;
    }

    return detail;
}

/* An entry's own text is drawn in Plex Mono, since it is the text exactly, and a description in Plex Sans */
static const char* title_face(const shown_entry& entry) {
    return entry.is_title_text ? STLXTHEME_MONO_FONT_PATH : STLXTHEME_UI_FONT_MEDIUM_PATH;
}

static float title_size(const shown_entry& entry) {
    return entry.is_title_text ? ENTRY_TEXT_SIZE : STLXTHEME_BODY_SIZE;
}

clip_row::clip_row(const shown_entry& entry, const std::string& detail)
    : m_icon(entry.icon),
      m_title(fitted(entry.title, title_face(entry), title_size(entry), ENTRY_TEXT_W), title_face(entry),
              title_size(entry), STLXTHEME_TEXT),
      m_detail(fitted(detail, STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, ENTRY_TEXT_W), STLXTHEME_UI_FONT_PATH,
               STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_SECONDARY) {
}

void clip_row::paint(ui::painter& p) {
    if (m_is_selected) {
        p.rounded_rect({ 0, 0, m_frame.w, m_frame.h }, STLXTHEME_CONTROL_RADIUS, STLXTHEME_SURFACE_SELECTED);
    }

    if (m_icon) {
        p.image({ ENTRY_PADDING_LEFT, (m_frame.h - ENTRY_ICON_SIZE) / 2 }, m_icon, STLXTHEME_INNER_RADIUS);
    }

    int32_t top = (m_frame.h - ENTRY_TITLE_HEIGHT - SUBTITLE_LINE_HEIGHT) / 2;
    m_title.draw(p, ENTRY_TEXT_LEFT, top, ENTRY_TITLE_HEIGHT);
    m_detail.draw(p, ENTRY_TEXT_LEFT, top + ENTRY_TITLE_HEIGHT, SUBTITLE_LINE_HEIGHT);
}

clip_preview::clip_preview()
    : m_kind("", STLXTHEME_UI_FONT_SEMIBOLD_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT),
      m_from("", STLXTHEME_UI_FONT_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT_SECONDARY),
      m_tag("On the clipboard", STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_SECONDARY) {
}

/* The pocket holds the lines its height fits, and the meta line counts a text's lines and characters */
void clip_preview::show(const shown_entry& entry, int64_t now_ns) {
    std::string age = age_text(now_ns - entry.copied_ns);
    m_kind.set_text(entry.kind);
    m_from.set_text("from " + entry.from + ", copied " + (age == "now" ? "just now" : age));
    m_is_current = entry.is_current;

    m_lines.clear();
    std::string_view text = entry.text;
    for (size_t at = 0; at < text.size() && m_lines.size() < 32;) {
        size_t end = std::min(text.find('\n', at), text.size());
        m_lines.push_back(std::make_unique<text_picture>(expanded(text.substr(at, end - at)), STLXTHEME_MONO_FONT_PATH,
                                                         ENTRY_TEXT_SIZE, STLXTHEME_TERMINAL_TEXT));
        at = end + 1;
    }

    std::vector<std::string> parts;
    if (entry.is_title_text && entry.text_len <= PREVIEW_BYTES_MAX) {
        size_t lines = line_count(entry.text);
        size_t characters = character_count(entry.text);
        parts = { std::to_string(lines) + (lines == 1 ? " line" : " lines"),
                  std::to_string(characters) + (characters == 1 ? " character" : " characters"), "UTF-8" };
    } else {
        parts = { size_text(entry.size), entry.types.empty() ? std::string() : entry.types.front() };
        if (entry.is_title_text) {
            parts.push_back("first 64 KB shown");
        }
    }

    m_meta.clear();
    for (const std::string& part : parts) {
        m_meta.push_back(std::make_unique<text_picture>(part, STLXTHEME_MONO_FONT_PATH, META_SIZE,
                                                        STLXTHEME_TEXT_DIM));
    }

    invalidate();
}

void clip_preview::paint(ui::painter& p) {
    int32_t right = m_frame.w - PREVIEW_PADDING;
    m_kind.draw(p, PREVIEW_PADDING, 0, PREVIEW_HEAD_HEIGHT);
    m_from.draw(p, PREVIEW_PADDING + m_kind.width() + PREVIEW_HEAD_GAP, 0, PREVIEW_HEAD_HEIGHT);
    if (m_is_current) {
        int32_t tag_w = m_tag.width() + 2 * TAG_PADDING_X;
        ui::rect tag = { right - tag_w, (PREVIEW_HEAD_HEIGHT - TAG_HEIGHT) / 2, tag_w, TAG_HEIGHT };
        p.rounded_stroke(tag, STLXTHEME_INNER_RADIUS, STLXTHEME_LINE_STRONG);
        m_tag.draw(p, tag.x + TAG_PADDING_X, tag.y, tag.h);
    }

    ui::rect pocket = { PREVIEW_PADDING, PREVIEW_HEAD_HEIGHT, right - PREVIEW_PADDING,
                        m_frame.h - PREVIEW_HEAD_HEIGHT - META_HEIGHT };
    p.rounded_rect(pocket, STLXTHEME_CONTROL_RADIUS, STLXTHEME_RECESS);
    p.rounded_stroke(pocket, STLXTHEME_CONTROL_RADIUS, STLXTHEME_LINE_SUBTLE);

    ui::rect inside = { pocket.x + POCKET_PADDING, pocket.y + POCKET_PADDING, pocket.w - 2 * POCKET_PADDING,
                        pocket.h - 2 * POCKET_PADDING };
    p.push_clip(inside);
    for (size_t i = 0; i < m_lines.size(); i++) {
        int32_t top = inside.y + static_cast<int32_t>(i) * POCKET_LINE_HEIGHT;
        if (top >= inside.y + inside.h) {
            break;
        }

        m_lines[i]->draw(p, inside.x, top, POCKET_LINE_HEIGHT);
    }

    p.pop_clip();

    int32_t x = PREVIEW_PADDING;
    for (const std::unique_ptr<text_picture>& part : m_meta) {
        part->draw(p, x, m_frame.h - META_HEIGHT, META_HEIGHT);
        x += part->width() + META_GAP;
    }
}

clipboard_pane::clipboard_pane(clipboard_source source)
    : ui::box(ui::axis::row), m_source(std::move(source)) {
    refresh();
}

void clipboard_pane::refresh() {
    uint64_t selected_id = m_selected < m_entries.size() ? m_entries[m_selected].id : 0;
    m_entries.clear();
    for (const clipboard_entry* entry : m_source.history->entries()) {
        m_entries.push_back(show_entry(m_source, *entry));
    }

    m_selected = 0;
    for (size_t i = 0; i < m_entries.size(); i++) {
        if (m_entries[i].id == selected_id) {
            m_selected = i;
        }
    }

    build();
}

/* The pinned entries head the column, then the rest since the history began, and an empty history says so */
void clipboard_pane::build() {
    while (!m_children.empty()) {
        remove(m_children.back().get());
    }

    m_list = nullptr;
    m_preview = nullptr;
    m_rows.clear();
    m_row_tops.clear();
    if (m_entries.empty()) {
        add<clip_empty>()->s().main = ui::length::flex();
        return;
    }

    m_list = add<clip_list>();
    m_list->s().main = ui::length::fixed(COLUMN_WIDTH);
    m_list->s().padding.right = COLUMN_PADDING_RIGHT;
    add<column_rule>()->s().main = ui::length::fixed(1);
    m_preview = add<clip_preview>();
    m_preview->s().main = ui::length::flex();

    int64_t now_ns = monotonic_ns();
    int32_t top = 0;
    for (size_t i = 0; i < m_entries.size(); i++) {
        bool starts_recent = !m_entries[i].is_pinned && (i == 0 || m_entries[i - 1].is_pinned);
        if (i == 0 && m_entries[i].is_pinned) {
            m_list->content()->add<section_head>("PINNED");
            top += HEAD_HEIGHT;
        }

        if (starts_recent) {
            m_list->content()->add<section_head>("RECENT")->set_end("since " + m_source.since);
            top += HEAD_HEIGHT;
        }

        clip_row* row = m_list->content()->add<clip_row>(m_entries[i], detail_of(m_entries[i], now_ns));
        m_rows.push_back(row);
        m_row_tops.push_back(top);
        top += ENTRY_HEIGHT;
        row->on_hover = [this, i]() {
            select(i);
        };

        row->on_choose = [this, i]() {
            select(i);
            choose(true);
        };
    }

    select(m_selected);
}

void clipboard_pane::select(size_t index) {
    if (!m_list || index >= m_entries.size()) {
        return;
    }

    m_selected = index;
    for (size_t i = 0; i < m_rows.size(); i++) {
        m_rows[i]->set_selected(i == index);
    }

    /* The first entry under a head reveals the head with it */
    bool follows_head = index == 0 || m_row_tops[index] != m_row_tops[index - 1] + ENTRY_HEIGHT;
    int32_t reveal_top = follows_head ? m_row_tops[index] - HEAD_HEIGHT : m_row_tops[index];
    int32_t row_bottom = m_row_tops[index] + ENTRY_HEIGHT;
    int32_t view_h = m_list->frame().h > 0 ? m_list->frame().h : PAGE_ENTRIES * ENTRY_HEIGHT;
    if (reveal_top < m_list->scroll_y()) {
        m_list->scroll_to(reveal_top);
    } else if (row_bottom > m_list->scroll_y() + view_h) {
        m_list->scroll_to(row_bottom - view_h);
    }

    m_preview->show(m_entries[index], monotonic_ns());
}

void clipboard_pane::move_selection(int32_t rows) {
    if (m_entries.empty()) {
        return;
    }

    int32_t last = static_cast<int32_t>(m_entries.size()) - 1;
    select(static_cast<size_t>(std::clamp(static_cast<int32_t>(m_selected) + rows, 0, last)));
}

/* Enter pastes into the focused window when there is one, and otherwise copies, as Ctrl Enter always does */
void clipboard_pane::choose(bool pastes) {
    if (m_selected >= m_entries.size()) {
        return;
    }

    uint64_t id = m_entries[m_selected].id;
    if (pastes && !m_source.focused_app.empty()) {
        m_source.paste(id);
    } else {
        m_source.copy(id);
    }
}

bool clipboard_pane::on_key(const ui::key_event& e) {
    bool is_ctrl = (e.modifiers & MOD_CTRL) != 0;
    switch (e.usage) {
    case HID_USAGE_UP:
        move_selection(-1);
        return true;
    case HID_USAGE_DOWN:
        move_selection(1);
        return true;
    case HID_USAGE_PAGE_UP:
        move_selection(-PAGE_ENTRIES);
        return true;
    case HID_USAGE_PAGE_DOWN:
        move_selection(PAGE_ENTRIES);
        return true;
    case HID_USAGE_ENTER:
        choose(!is_ctrl);
        return true;
    default:
        return false;
    }
}

std::vector<footer_item> clipboard_pane::footer_actions() const {
    if (m_entries.empty()) {
        return {};
    }

    if (m_source.focused_app.empty()) {
        return { { "Copy", "Enter" } };
    }

    return { { "Paste into " + m_source.focused_app, "Enter" }, { "Copy", "Ctrl Enter" } };
}

/* The count of entries, and the bytes the budget counts against its limit */
std::string clipboard_pane::footer_end() const {
    size_t count = m_entries.size();
    std::string items = count == 1 ? "1 item" : std::to_string(count) + " items";
    if (count == 0) {
        return items;
    }

    uint64_t limit_mb = m_source.history->byte_limit() >> 20;
    return items + ", " + size_text(m_source.history->kept_bytes()) + " of " + std::to_string(limit_mb) + " MB";
}

std::string clipboard_pane::placeholder() const {
    size_t count = m_entries.size();
    if (count == 0) {
        return "Search the clipboard history";
    }

    return count == 1 ? "Search 1 clipboard item" : "Search " + std::to_string(count) + " clipboard items";
}
