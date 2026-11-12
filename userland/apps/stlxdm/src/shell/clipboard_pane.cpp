#include "clipboard_pane.hpp"

#include "keycap.hpp"
#include "quick_match.hpp"

#include <stlxgfx/image.h>
#include <stlxtheme/theme.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <memory>
#include <sys/stat.h>

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
constexpr int32_t POCKET_MARK_INSET = 1;
constexpr int32_t META_HEIGHT = 30;
constexpr int32_t META_GAP = 18;
constexpr float META_SIZE = 11.5f;

constexpr int32_t EMPTY_GAP = 6;
constexpr int32_t EMPTY_PADDING_X = 24;

constexpr int32_t FILE_ROW_HEIGHT = 44;
constexpr int32_t FILE_ICON_SIZE = STLXTHEME_DOCK_ICON_SIZE;
constexpr int32_t FILE_TEXT_LEFT = FILE_ICON_SIZE + 12;
constexpr int32_t FILE_SIZE_GAP = 12;
constexpr int32_t MESSAGE_GAP = 10;
constexpr int32_t THUMBNAIL_SIZE = ENTRY_ICON_SIZE;

constexpr int32_t CHOICE_HEIGHT = 44;
constexpr int32_t CHOICE_PADDING_X = 12;
constexpr int32_t PIN_GLYPH_SIZE = 16;

/* The question Clear Unpinned asks in place, on cream at .03 inside a faint ring */
constexpr int32_t QUESTION_HEIGHT = 58;
constexpr int32_t QUESTION_MARGIN = 4;
constexpr int32_t QUESTION_PADDING_X = 12;
constexpr uint32_t QUESTION_FILL = 0x08FFF0D7;

/* A preview shows a text's first 64 KB, and a row names a text by its first line's first bytes */
constexpr uint64_t PREVIEW_BYTES_MAX = 64 * 1024;
constexpr size_t TITLE_BYTES_MAX = 256;
constexpr size_t TAB_WIDTH = 4;
constexpr int32_t PAGE_ENTRIES = 7;

constexpr int64_t NS_PER_MINUTE = 60ll * 1000000000ll;

constexpr uint16_t HID_USAGE_K = 0x0E;
constexpr uint16_t HID_USAGE_P = 0x13;
constexpr uint16_t HID_USAGE_ENTER = 0x28;
constexpr uint16_t HID_USAGE_ESCAPE = 0x29;
constexpr uint16_t HID_USAGE_PAGE_UP = 0x4B;
constexpr uint16_t HID_USAGE_DELETE = 0x4C;
constexpr uint16_t HID_USAGE_PAGE_DOWN = 0x4E;
constexpr uint16_t HID_USAGE_DOWN = 0x51;
constexpr uint16_t HID_USAGE_UP = 0x52;
constexpr uint8_t  MOD_SHIFT = 1u << 0;
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

/* Where a match sits under drawn text, from the text's pen */
struct mark_span {
    int32_t x;
    int32_t w;
};

/* One file a files entry names, as File Manager's rows show it, its name and folder fitted to the pocket with the
 * query's matches marked */
struct file_line {
    const stlxgfx_surface_t*      icon;
    std::string                   name_text;
    std::string                   folder_text;
    std::unique_ptr<text_picture> name;
    std::unique_ptr<text_picture> folder;
    std::unique_ptr<text_picture> size;
    std::vector<mark_span>        name_marks;
    std::vector<mark_span>        folder_marks;
};

/* One row of the pocket's text, bytes `start` to `end` of one of its lines */
struct text_row {
    size_t line;
    size_t start;
    size_t end;
};

/* Where a row of the pocket's text starts */
struct row_spot {
    size_t line;
    size_t start;
};

/* The selected entry exactly: its kind, where and when it came from, its text, image or files in a pocket, and a line
 * about it */
class clip_preview : public ui::widget {
public:
    clip_preview();
    ~clip_preview() override;

    /* Shows `entry` with what `query` matches marked, taking `image`, its decoded picture, when it is an image */
    void show(const shown_entry& entry, const clipboard_source& source, std::string_view query, int64_t now_ns,
              stlxgfx_surface_t* image);
    void paint(ui::painter& p) override;

private:
    void drop_pictures();
    void wrap(const ui::rect& inside);
    row_spot window_start(size_t row_chars) const;
    int32_t message_height() const;
    void paint_image(ui::painter& p, const ui::rect& inside);
    void paint_files(ui::painter& p, const ui::rect& inside) const;

    text_picture                               m_kind;
    text_picture                               m_from;
    text_picture                               m_tag;
    bool                                       m_is_current = false;
    std::string                                m_query;
    std::vector<file_line>                     m_files;

    /* The paragraphs above a copy past the budget, the first in semibold, the text's lines with their tabs widened,
     * and the meta line's parts, laid out once a paint knows the pocket's width, with the query's matches marked */
    std::vector<std::string>                   m_message;
    std::vector<std::string>                   m_text_lines;
    std::vector<std::string>                   m_meta_parts;
    int32_t                                    m_wrapped_w = -1;
    std::vector<std::unique_ptr<text_picture>> m_message_lines;
    std::vector<std::unique_ptr<text_picture>> m_rows;
    std::vector<std::vector<mark_span>>        m_row_marks;
    std::vector<std::unique_ptr<text_picture>> m_meta;

    /* An image entry's picture, and that picture fitted to the pocket at its last size */
    stlxgfx_surface_t* m_image = nullptr;
    stlxgfx_surface_t* m_fitted = nullptr;
};

/* One entry: its icon, its first line or what it is with the query's matches marked, where and when it was copied,
 * and a pin when pinned */
class clip_row : public ui::widget {
public:
    clip_row(const shown_entry& entry, const std::string& detail, std::string_view query,
             const stlxgfx_surface_t* pin_glyph);

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
    const stlxgfx_surface_t* m_pin_glyph;
    text_picture             m_title;
    std::vector<mark_span>   m_title_marks;
    text_picture             m_detail;
    bool                     m_is_selected = false;
};

namespace {

/* One action of Ctrl K's list, and at the right the keys that run it */
class clip_choice : public ui::widget {
public:
    explicit clip_choice(const clip_action& action)
        : m_label(action.label, STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT) {
        for (size_t at = 0; at < action.keys.size();) {
            size_t end = std::min(action.keys.find(' ', at), action.keys.size());
            m_keys.push_back(std::make_unique<keycap>(action.keys.substr(at, end - at)));
            at = end + 1;
        }
    }

    void set_selected(bool is_selected) {
        if (is_selected != m_is_selected) {
            m_is_selected = is_selected;
            invalidate();
        }
    }

    ui::size measure(ui::size avail) override {
        return { avail.w, CHOICE_HEIGHT };
    }

    void paint(ui::painter& p) override {
        if (m_is_selected) {
            p.rounded_rect({ 0, 0, m_frame.w, m_frame.h }, STLXTHEME_CONTROL_RADIUS, STLXTHEME_SURFACE_SELECTED);
        }

        m_label.draw(p, CHOICE_PADDING_X, 0, m_frame.h);
        int32_t x = m_frame.w - CHOICE_PADDING_X;
        for (size_t i = m_keys.size(); i-- > 0;) {
            x -= m_keys[i]->width();
            m_keys[i]->paint(p, x, (m_frame.h - KEYCAP_HEIGHT) / 2);
            x -= KEYCAP_GAP;
        }
    }

private:
    text_picture                         m_label;
    std::vector<std::unique_ptr<keycap>> m_keys;
    bool                                 m_is_selected = false;
};

/* Clear Unpinned's question, asked above the entries before anything goes */
class clip_question : public ui::widget {
public:
    explicit clip_question(size_t count)
        : m_ask("Clear " + std::to_string(count) + (count == 1 ? " unpinned item?" : " unpinned items?"),
                STLXTHEME_UI_FONT_SEMIBOLD_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT),
          m_note("Pinned items stay.", STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_SECONDARY) {}

    ui::size measure(ui::size avail) override {
        return { avail.w, QUESTION_HEIGHT };
    }

    void paint(ui::painter& p) override {
        ui::rect strip = { 0, QUESTION_MARGIN, m_frame.w, QUESTION_HEIGHT - 2 * QUESTION_MARGIN };
        p.rounded_rect(strip, STLXTHEME_CONTROL_RADIUS, QUESTION_FILL);
        p.rounded_stroke(strip, STLXTHEME_CONTROL_RADIUS, STLXTHEME_LINE_SUBTLE);

        int32_t top = strip.y + (strip.h - ENTRY_TITLE_HEIGHT - SUBTITLE_LINE_HEIGHT) / 2;
        m_ask.draw(p, QUESTION_PADDING_X, top, ENTRY_TITLE_HEIGHT);
        m_note.draw(p, QUESTION_PADDING_X, top + ENTRY_TITLE_HEIGHT, SUBTITLE_LINE_HEIGHT);
    }

private:
    text_picture m_ask;
    text_picture m_note;
};

/* What shows while nothing is listed, a title and a note under it, both fitted to the pane */
class clip_empty : public ui::widget {
public:
    clip_empty(std::string title, std::string note)
        : m_title_text(std::move(title)), m_note_text(std::move(note)),
          m_title("", STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT),
          m_note("", STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_SECONDARY) {}

    void paint(ui::painter& p) override {
        int32_t max_w = m_frame.w - 2 * EMPTY_PADDING_X;
        m_title.set_text(fitted(m_title_text, STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE, max_w));
        m_note.set_text(fitted(m_note_text, STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, max_w));

        int32_t top = (m_frame.h - 2 * ENTRY_TITLE_HEIGHT - EMPTY_GAP) / 2;
        m_title.draw(p, (m_frame.w - m_title.width()) / 2, top, ENTRY_TITLE_HEIGHT);
        m_note.draw(p, (m_frame.w - m_note.width()) / 2, top + ENTRY_TITLE_HEIGHT + EMPTY_GAP, ENTRY_TITLE_HEIGHT);
    }

private:
    std::string  m_title_text;
    std::string  m_note_text;
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

/* A line as the pocket draws it, its tabs widened to the next stop */
static std::string expanded(std::string_view line) {
    std::string out;
    for (char c : line) {
        if (c == '\t') {
            out.append(TAB_WIDTH - out.size() % TAB_WIDTH, ' ');
        } else if (c != '\r') {
            out += c;
        }
    }

    return out;
}

/* Where a row of at most `row_chars` characters from `start` ends: after its last space or slash when that leaves the
 * row at least half full, else at the edge, a space just past the edge hanging off the row */
static size_t row_end(std::string_view line, size_t start, size_t row_chars) {
    size_t end = start;
    size_t chars = 0;
    size_t after_break = start;
    while (end < line.size() && chars < row_chars) {
        char c = line[end++];
        while (end < line.size() && (static_cast<uint8_t>(line[end]) & 0xC0) == 0x80) {
            end++;
        }

        chars++;
        if ((c == ' ' || c == '/') && chars * 2 >= row_chars) {
            after_break = end;
        }
    }

    if (end == line.size()) {
        return end;
    }

    if (line[end] == ' ') {
        return end + 1;
    }

    return after_break > start ? after_break : end;
}

/* Where each row of one line starts */
static std::vector<size_t> row_starts(std::string_view line, size_t row_chars) {
    std::vector<size_t> starts = { 0 };
    for (size_t at = row_end(line, 0, row_chars); at < line.size(); at = row_end(line, at, row_chars)) {
        starts.push_back(at);
    }

    return starts;
}

/* The first `max_rows` rows of `lines` from the row starting at `first`, a blank line taking a row of its own */
static std::vector<text_row> wrap_rows(const std::vector<std::string>& lines, row_spot first, size_t row_chars,
                                       size_t max_rows) {
    std::vector<text_row> rows;
    size_t start = first.start;
    for (size_t i = first.line; i < lines.size() && rows.size() < max_rows; i++) {
        do {
            size_t end = row_end(lines[i], start, row_chars);
            rows.push_back({ i, start, end });
            start = end;
        } while (start < lines[i].size() && rows.size() < max_rows);

        start = 0;
    }

    return rows;
}

/* Each run of `text` that holds `query` in any case */
static std::vector<match_run> query_runs(std::string_view text, std::string_view query) {
    std::vector<match_run> runs;
    for (size_t at = find_folded(text, query); at != std::string_view::npos;
         at = find_folded(text, query, at + query.size())) {
        runs.push_back({ static_cast<uint32_t>(at), static_cast<uint32_t>(query.size()) });
    }

    return runs;
}

/* Where each of `runs` sits under `text` drawn in the face */
static std::vector<mark_span> spans_of(std::string_view text, const std::vector<match_run>& runs,
                                       const char* face_path, float em_px) {
    stlxgfx_font* font = shell_font(face_path, em_px);
    std::vector<mark_span> spans;
    if (!font) {
        return spans;
    }

    for (const match_run& run : runs) {
        spans.push_back({ stlxgfx_text_width(font, text.data(), run.start),
                          stlxgfx_text_width(font, text.data() + run.start, run.length) });
    }

    return spans;
}

static void paint_spans(ui::painter& p, const std::vector<mark_span>& spans, int32_t x, int32_t top, int32_t h) {
    for (const mark_span& span : spans) {
        p.rounded_rect({ x + span.x, top, span.w, h }, MARK_RADIUS, STLXTHEME_MATCH_MARK);
    }
}

/* Prose broken at its spaces into lines that fit `max_w` pixels in the face, a word wider than a line kept whole */
static std::vector<std::string> wrapped(const std::string& text, const char* face_path, float em_px, int32_t max_w) {
    stlxgfx_font* font = shell_font(face_path, em_px);
    std::vector<std::string> lines;
    std::string line;
    for (size_t at = 0; at < text.size();) {
        size_t end = std::min(text.find(' ', at), text.size());
        std::string word = text.substr(at, end - at);
        std::string joined = line.empty() ? word : line + " " + word;
        bool fits = !font || stlxgfx_text_width(font, joined.data(), joined.size()) <= max_w;
        if (fits || line.empty()) {
            line = std::move(joined);
        } else {
            lines.push_back(std::move(line));
            line = std::move(word);
        }

        at = end + 1;
    }

    if (!line.empty()) {
        lines.push_back(std::move(line));
    }

    return lines;
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

/* A folder shows the folder icon, a picture the image icon, a program the program icon, and the rest the text icon */
static const stlxgfx_surface_t* file_icon(const clipboard_source& source, const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return source.folder_icon;
    }

    const char* type = stlxconf_file_type(path.c_str()).mime_type;
    if (strncmp(type, "image/", strlen("image/")) == 0) {
        return source.image_icon;
    }

    return strcmp(type, "application/x-executable") == 0 ? source.program_icon : source.text_icon;
}

/* One file by its name, and several by their count and names, as "2 files: tar.c, tar.h" */
static std::string files_title(const std::vector<std::string>& files) {
    if (files.size() == 1) {
        return file_name(files.front().c_str());
    }

    std::string title = std::to_string(files.size()) + " files: ";
    for (size_t i = 0; i < files.size(); i++) {
        title += (i > 0 ? ", " : "") + std::string(file_name(files[i].c_str()));
    }

    return title;
}

/* An image shows as one and its thumbnail, files as File Manager's rows, and text as itself in its copier's icon */
static shown_entry show_entry(const clipboard_source& source, const clipboard_entry& entry) {
    shown_entry shown;
    shown.id = entry.id;
    shown.from = entry.label;
    shown.copied_ns = entry.copied_ns;
    shown.is_current = source.history->is_current(entry.id);
    shown.is_pinned = entry.pinned;

    shown.size = entry.item.size();
    shown.is_too_large = shown.is_current && shown.size > source.history->byte_limit();
    shown.icon = icon_for(source, entry.label);
    for (const clipboard_rep& rep : entry.item.reps()) {
        shown.types.push_back(rep.mime_type);
    }

    const clipboard_rep* text = entry.item.find(SWP_MIME_TEXT);
    if (text) {
        shown.text = read_bytes(*text, PREVIEW_BYTES_MAX);
        shown.text_len = text->len;
    }

    const clipboard_rep* png = entry.item.find("image/png");
    if (png) {
        shown.is_image = true;
        png_size(read_bytes(*png, 24), &shown.image_w, &shown.image_h);
        shown.kind = "Image";
        shown.title = "Image, " + std::to_string(shown.image_w) + " \xC3\x97 " + std::to_string(shown.image_h);

        const stlxgfx_surface_t* thumbnail = source.thumbnails->of(entry);
        shown.icon = thumbnail ? thumbnail : source.image_icon;
        return shown;
    }

    const clipboard_rep* list = entry.item.find("text/uri-list");
    shown.files = list ? uri_list_paths(read_bytes(*list, PREVIEW_BYTES_MAX)) : std::vector<std::string>();
    if (!shown.files.empty()) {
        shown.kind = shown.files.size() == 1 ? "File" : std::to_string(shown.files.size()) + " files";
        shown.title = files_title(shown.files);
        shown.icon = file_icon(source, shown.files.front());
        return shown;
    }

    if (text) {
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

/* Where an entry came from and when, and that a copy past the budget is kept only while current, or how long a text
 * of more than one line is */
static std::string detail_of(const shown_entry& entry, int64_t now_ns) {
    std::string detail = entry.from + ", " + (entry.is_pinned ? "pinned" : age_text(now_ns - entry.copied_ns));
    std::string length = text_length(entry);
    if (entry.is_too_large) {
        detail += ", on the clipboard only";
    } else if (!length.empty()) {
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

/* A pinned entry's title leaves room for its pin */
static int32_t title_width(const shown_entry& entry) {
    return ENTRY_TEXT_W - (entry.is_pinned ? PIN_GLYPH_SIZE + ENTRY_PADDING_RIGHT : 0);
}

/* Only the names in "2 files: tar.c, tar.h" are content, so its count is never marked */
clip_row::clip_row(const shown_entry& entry, const std::string& detail, std::string_view query,
                   const stlxgfx_surface_t* pin_glyph)
    : m_icon(entry.icon),
      m_pin_glyph(entry.is_pinned ? pin_glyph : nullptr),
      m_title("", title_face(entry), title_size(entry), STLXTHEME_TEXT),
      m_detail(fitted(detail, STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE, ENTRY_TEXT_W), STLXTHEME_UI_FONT_PATH,
               STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_SECONDARY) {
    std::string title = fitted(entry.title, title_face(entry), title_size(entry), title_width(entry));
    size_t content_start = entry.files.size() > 1 ? entry.title.find(": ") + 2 : 0;
    std::vector<match_run> runs = query_runs(title, query);
    std::erase_if(runs, [content_start](const match_run& run) {
        return run.start < content_start;
    });

    m_title_marks = spans_of(title, runs, title_face(entry), title_size(entry));
    m_title.set_text(std::move(title));
}

void clip_row::paint(ui::painter& p) {
    if (m_is_selected) {
        p.rounded_rect({ 0, 0, m_frame.w, m_frame.h }, STLXTHEME_CONTROL_RADIUS, STLXTHEME_SURFACE_SELECTED);
    }

    if (m_icon) {
        p.image({ ENTRY_PADDING_LEFT, (m_frame.h - ENTRY_ICON_SIZE) / 2 }, m_icon, STLXTHEME_INNER_RADIUS);
    }

    int32_t top = (m_frame.h - ENTRY_TITLE_HEIGHT - SUBTITLE_LINE_HEIGHT) / 2;
    paint_spans(p, m_title_marks, ENTRY_TEXT_LEFT, top, ENTRY_TITLE_HEIGHT);
    m_title.draw(p, ENTRY_TEXT_LEFT, top, ENTRY_TITLE_HEIGHT);
    m_detail.draw(p, ENTRY_TEXT_LEFT, top + ENTRY_TITLE_HEIGHT, SUBTITLE_LINE_HEIGHT);
    if (m_pin_glyph) {
        p.image({ m_frame.w - ENTRY_PADDING_RIGHT - PIN_GLYPH_SIZE, top + (ENTRY_TITLE_HEIGHT - PIN_GLYPH_SIZE) / 2 },
                m_pin_glyph);
    }
}

clip_preview::clip_preview()
    : m_kind("", STLXTHEME_UI_FONT_SEMIBOLD_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT),
      m_from("", STLXTHEME_UI_FONT_PATH, STLXTHEME_BODY_SIZE, STLXTHEME_TEXT_SECONDARY),
      m_tag("On the clipboard", STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_CAPTION_SIZE, STLXTHEME_TEXT_SECONDARY) {
}

clip_preview::~clip_preview() {
    drop_pictures();
}

void clip_preview::drop_pictures() {
    stlxgfx_destroy_surface(m_image);
    stlxgfx_destroy_surface(m_fitted);
    m_image = nullptr;
    m_fitted = nullptr;
}

/* What the meta line says: a text's lines and characters, an image's format, size and bytes, where files paste, or
 * that a long text shows its first 64 KB, or that a search's match lies past them */
static std::vector<std::string> meta_parts(const shown_entry& entry, std::string_view query) {
    if (entry.is_image) {
        return { "PNG", std::to_string(entry.image_w) + " \xC3\x97 " + std::to_string(entry.image_h),
                 size_text(entry.size) };
    }

    if (!entry.files.empty()) {
        return { entry.text_len > 0 ? "files in File Manager, paths in text" : "files in File Manager" };
    }

    if (entry.is_title_text && entry.text_len <= PREVIEW_BYTES_MAX) {
        size_t lines = line_count(entry.text);
        size_t characters = character_count(entry.text);
        return { std::to_string(lines) + (lines == 1 ? " line" : " lines"),
                 std::to_string(characters) + (characters == 1 ? " character" : " characters"), "UTF-8" };
    }

    std::vector<std::string> parts = { size_text(entry.size), entry.types.empty() ? "" : entry.types.front() };
    if (entry.is_title_text) {
        bool is_match_past = !query.empty() && find_folded(entry.text, query) == std::string_view::npos;
        parts.push_back(is_match_past ? "match past the first 64 KB" : "first 64 KB shown");
    }

    return parts;
}

/* The pocket holds what its height fits, a copy past the budget saying so above its text */
void clip_preview::show(const shown_entry& entry, const clipboard_source& source, std::string_view query,
                        int64_t now_ns, stlxgfx_surface_t* image) {
    drop_pictures();
    m_image = image;
    m_query = query;
    m_wrapped_w = -1;

    std::string age = age_text(now_ns - entry.copied_ns);
    m_kind.set_text(entry.kind);
    m_from.set_text("from " + entry.from + ", copied " + (age == "now" ? "just now" : age));
    m_is_current = entry.is_current;

    m_message.clear();
    if (entry.is_too_large) {
        std::string limit = std::to_string(source.history->byte_limit() >> 20) + " MB";
        m_message.push_back(size_text(entry.size) + " is past the " + limit + " the history keeps.");
        m_message.push_back("It stays while it is on the clipboard, and leaves the history when something else is "
                            "copied.");
    }

    m_files.clear();
    for (const std::string& path : entry.files) {
        struct stat st;
        bool is_there = stat(path.c_str(), &st) == 0;
        std::string folder = path.substr(0, path.rfind('/'));
        std::string size = !is_there ? "gone" : S_ISDIR(st.st_mode) ? "folder" : size_text(st.st_size);
        m_files.push_back({ file_icon(source, path), file_name(path.c_str()), folder.empty() ? "/" : folder,
                            std::make_unique<text_picture>("", STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE,
                                                           STLXTHEME_TEXT),
                            std::make_unique<text_picture>("", STLXTHEME_MONO_FONT_PATH, META_SIZE,
                                                           STLXTHEME_TEXT_DIM),
                            std::make_unique<text_picture>(size, STLXTHEME_UI_FONT_PATH, STLXTHEME_CAPTION_SIZE,
                                                           STLXTHEME_TEXT_SECONDARY),
                            {}, {} });
    }

    m_text_lines.clear();
    std::string_view text = entry.is_image || !entry.files.empty() ? std::string_view() : entry.text;
    for (size_t at = 0; at < text.size();) {
        size_t end = std::min(text.find('\n', at), text.size());
        m_text_lines.push_back(expanded(text.substr(at, end - at)));
        at = end + 1;
    }

    m_meta_parts = meta_parts(entry, query);
    invalidate();
}

/* Only a paint knows the pocket's width, so the message, the text's rows, the files' names and the meta line are laid
 * out then */
void clip_preview::wrap(const ui::rect& inside) {
    m_wrapped_w = inside.w;
    m_message_lines.clear();
    for (size_t i = 0; i < m_message.size(); i++) {
        const char* face = i == 0 ? STLXTHEME_UI_FONT_SEMIBOLD_PATH : STLXTHEME_UI_FONT_PATH;
        uint32_t color = i == 0 ? STLXTHEME_TEXT : STLXTHEME_TEXT_SECONDARY;
        for (std::string& line : wrapped(m_message[i], face, STLXTHEME_BODY_SIZE, inside.w)) {
            m_message_lines.push_back(std::make_unique<text_picture>(std::move(line), face, STLXTHEME_BODY_SIZE,
                                                                     color));
        }
    }

    /* Plex Mono gives every character one advance, so a row holds a count of them */
    stlxgfx_font* mono = shell_font(STLXTHEME_MONO_FONT_PATH, ENTRY_TEXT_SIZE);
    int32_t advance = mono ? std::max(1, stlxgfx_text_width(mono, "M", 1)) : 1;
    size_t row_chars = static_cast<size_t>(std::max(1, inside.w / advance));
    int32_t rows_h = std::max(0, inside.h - message_height());
    size_t rows_fit = static_cast<size_t>((rows_h + POCKET_LINE_HEIGHT - 1) / POCKET_LINE_HEIGHT);

    /* A match split across two rows is marked in both */
    m_rows.clear();
    m_row_marks.clear();
    size_t runs_line = SIZE_MAX;
    std::vector<match_run> line_runs;
    for (const text_row& row : wrap_rows(m_text_lines, window_start(row_chars), row_chars, rows_fit)) {
        if (row.line != runs_line) {
            runs_line = row.line;
            line_runs = query_runs(m_text_lines[row.line], m_query);
        }

        std::vector<match_run> runs;
        for (const match_run& run : line_runs) {
            size_t from = std::max<size_t>(run.start, row.start);
            size_t to = std::min<size_t>(run.start + run.length, row.end);
            if (from < to) {
                runs.push_back({ static_cast<uint32_t>(from - row.start), static_cast<uint32_t>(to - from) });
            }
        }

        std::string shown = m_text_lines[row.line].substr(row.start, row.end - row.start);
        m_row_marks.push_back(spans_of(shown, runs, STLXTHEME_MONO_FONT_PATH, ENTRY_TEXT_SIZE));
        m_rows.push_back(std::make_unique<text_picture>(std::move(shown), STLXTHEME_MONO_FONT_PATH, ENTRY_TEXT_SIZE,
                                                        STLXTHEME_TERMINAL_TEXT));
    }

    for (file_line& file : m_files) {
        int32_t text_w = inside.w - FILE_TEXT_LEFT - file.size->width() - FILE_SIZE_GAP;
        std::string name = fitted(file.name_text, STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE, text_w);
        std::string folder = fitted(file.folder_text, STLXTHEME_MONO_FONT_PATH, META_SIZE, text_w);

        file.name_marks = spans_of(name, query_runs(name, m_query), STLXTHEME_UI_FONT_MEDIUM_PATH, STLXTHEME_BODY_SIZE);
        file.folder_marks = spans_of(folder, query_runs(folder, m_query), STLXTHEME_MONO_FONT_PATH, META_SIZE);
        file.name->set_text(std::move(name));
        file.folder->set_text(std::move(folder));
    }

    /* The meta line keeps its parts whole while they fit, and the first that does not ends in an ellipsis */
    m_meta.clear();
    int32_t meta_left = m_frame.w - 2 * PREVIEW_PADDING;
    for (const std::string& part : m_meta_parts) {
        std::string shown = fitted(part, STLXTHEME_MONO_FONT_PATH, META_SIZE, meta_left);
        bool is_cut = shown != part;
        m_meta.push_back(std::make_unique<text_picture>(std::move(shown), STLXTHEME_MONO_FONT_PATH, META_SIZE,
                                                        STLXTHEME_TEXT_DIM));
        meta_left -= m_meta.back()->width() + META_GAP;
        if (is_cut || meta_left <= 0) {
            break;
        }
    }
}

/* A search shows the text from one row above its first match, so the match lands near the top with a row of what
 * leads up to it */
row_spot clip_preview::window_start(size_t row_chars) const {
    for (size_t i = 0; i < m_text_lines.size() && !m_query.empty(); i++) {
        size_t at = find_folded(m_text_lines[i], m_query);
        if (at == std::string_view::npos) {
            continue;
        }

        std::vector<size_t> starts = row_starts(m_text_lines[i], row_chars);
        size_t row = static_cast<size_t>(std::upper_bound(starts.begin(), starts.end(), at) - starts.begin()) - 1;
        if (row > 0) {
            return { i, starts[row - 1] };
        }

        return i > 0 ? row_spot{ i - 1, row_starts(m_text_lines[i - 1], row_chars).back() } : row_spot{ 0, 0 };
    }

    return { 0, 0 };
}

int32_t clip_preview::message_height() const {
    int32_t lines_h = static_cast<int32_t>(m_message_lines.size()) * POCKET_LINE_HEIGHT;
    return m_message_lines.empty() ? 0 : lines_h + MESSAGE_GAP;
}

/* The image fits the pocket whole and is never shown larger than it is */
void clip_preview::paint_image(ui::painter& p, const ui::rect& inside) {
    double scale = std::min({ 1.0, static_cast<double>(inside.w) / m_image->width,
                              static_cast<double>(inside.h) / m_image->height });
    uint32_t w = std::max(1u, static_cast<uint32_t>(m_image->width * scale));
    uint32_t h = std::max(1u, static_cast<uint32_t>(m_image->height * scale));
    if (!m_fitted || m_fitted->width != w || m_fitted->height != h) {
        stlxgfx_destroy_surface(m_fitted);
        m_fitted = stlxgfx_create_surface(w, h, 32, 16, 8, 0);
        if (!m_fitted) {
            return;
        }

        stlxgfx_blit_scaled(m_fitted, 0, 0, w, h, m_image, 0, 0, m_image->width, m_image->height);
    }

    int32_t x = inside.x + (inside.w - static_cast<int32_t>(w)) / 2;
    int32_t y = inside.y + (inside.h - static_cast<int32_t>(h)) / 2;
    p.image({ x, y }, m_fitted, STLXTHEME_INNER_RADIUS);
}

void clip_preview::paint_files(ui::painter& p, const ui::rect& inside) const {
    for (size_t i = 0; i < m_files.size(); i++) {
        int32_t top = inside.y + static_cast<int32_t>(i) * FILE_ROW_HEIGHT;
        if (top + FILE_ROW_HEIGHT > inside.y + inside.h) {
            break;
        }

        const file_line& file = m_files[i];
        if (file.icon) {
            p.image({ inside.x, top + (FILE_ROW_HEIGHT - FILE_ICON_SIZE) / 2 }, file.icon);
        }

        int32_t text_top = top + (FILE_ROW_HEIGHT - ENTRY_TITLE_HEIGHT - SUBTITLE_LINE_HEIGHT) / 2;
        paint_spans(p, file.name_marks, inside.x + FILE_TEXT_LEFT, text_top, ENTRY_TITLE_HEIGHT);
        paint_spans(p, file.folder_marks, inside.x + FILE_TEXT_LEFT, text_top + ENTRY_TITLE_HEIGHT,
                    SUBTITLE_LINE_HEIGHT);
        file.name->draw(p, inside.x + FILE_TEXT_LEFT, text_top, ENTRY_TITLE_HEIGHT);
        file.folder->draw(p, inside.x + FILE_TEXT_LEFT, text_top + ENTRY_TITLE_HEIGHT, SUBTITLE_LINE_HEIGHT);
        file.size->draw(p, inside.x + inside.w - file.size->width(), top, FILE_ROW_HEIGHT);
    }
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
    if (inside.w != m_wrapped_w) {
        wrap(inside);
    }

    p.push_clip(inside);
    for (size_t i = 0; i < m_message_lines.size(); i++) {
        m_message_lines[i]->draw(p, inside.x, inside.y + static_cast<int32_t>(i) * POCKET_LINE_HEIGHT,
                                 POCKET_LINE_HEIGHT);
    }

    inside.y += message_height();
    inside.h -= message_height();
    if (m_image) {
        paint_image(p, inside);
    } else if (!m_files.empty()) {
        paint_files(p, inside);
    }

    for (size_t i = 0; i < m_rows.size(); i++) {
        int32_t top = inside.y + static_cast<int32_t>(i) * POCKET_LINE_HEIGHT;
        paint_spans(p, m_row_marks[i], inside.x, top + POCKET_MARK_INSET, POCKET_LINE_HEIGHT - 2 * POCKET_MARK_INSET);
        m_rows[i]->draw(p, inside.x, top, POCKET_LINE_HEIGHT);
    }

    p.pop_clip();

    int32_t x = PREVIEW_PADDING;
    for (const std::unique_ptr<text_picture>& part : m_meta) {
        part->draw(p, x, m_frame.h - META_HEIGHT, META_HEIGHT);
        x += part->width() + META_GAP;
    }
}

/* A thumbnail covers its square with the middle of the picture, as the history's mock crops a wide capture */
static stlxgfx_surface_t* cover_thumbnail(const clipboard_rep& png) {
    std::string bytes = read_bytes(png, png.len);
    stlxgfx_surface_t* picture = stlxgfx_decode_image(bytes.data(), bytes.size());
    if (!picture) {
        return nullptr;
    }

    uint32_t side = std::min(picture->width, picture->height);
    stlxgfx_surface_t* thumbnail = stlxgfx_create_surface(THUMBNAIL_SIZE, THUMBNAIL_SIZE, 32, 16, 8, 0);
    if (thumbnail) {
        stlxgfx_blit_scaled(thumbnail, 0, 0, THUMBNAIL_SIZE, THUMBNAIL_SIZE, picture,
                            static_cast<int32_t>((picture->width - side) / 2),
                            static_cast<int32_t>((picture->height - side) / 2), side, side);
    }

    stlxgfx_destroy_surface(picture);
    return thumbnail;
}

clipboard_thumbnails::~clipboard_thumbnails() {
    for (const thumbnail& made : m_thumbnails) {
        stlxgfx_destroy_surface(made.picture);
    }
}

/* A picture that cannot be decoded is remembered as none, so it is decoded once */
const stlxgfx_surface_t* clipboard_thumbnails::of(const clipboard_entry& entry) {
    for (const thumbnail& made : m_thumbnails) {
        if (made.id == entry.id) {
            return made.picture;
        }
    }

    const clipboard_rep* png = entry.item.find("image/png");
    m_thumbnails.push_back({ entry.id, png ? cover_thumbnail(*png) : nullptr });
    return m_thumbnails.back().picture;
}

void clipboard_thumbnails::keep(const std::vector<const clipboard_entry*>& entries) {
    std::erase_if(m_thumbnails, [&entries](const thumbnail& made) {
        bool is_kept = std::any_of(entries.begin(), entries.end(), [&made](const clipboard_entry* entry) {
            return entry->id == made.id;
        });

        if (!is_kept) {
            stlxgfx_destroy_surface(made.picture);
        }

        return !is_kept;
    });
}

/* A pushpin on the 16 unit grid: its head, its body, the flared base, and the needle */
static std::vector<std::vector<glyph_point>> pin_contours() {
    return { { { 5, 1.5f }, { 11, 1.5f }, { 11, 3 }, { 10, 3 }, { 10, 8 }, { 12.5f, 8 }, { 12.5f, 9.5f },
               { 8.6f, 9.5f }, { 8.6f, 15 }, { 7.4f, 15 }, { 7.4f, 9.5f }, { 3.5f, 9.5f }, { 3.5f, 8 }, { 6, 8 },
               { 6, 3 }, { 5, 3 } } };
}

clipboard_pane::clipboard_pane(clipboard_source source)
    : ui::box(ui::axis::row), m_source(std::move(source)) {
    m_pin_glyph = make_shape_glyph(PIN_GLYPH_SIZE, STLXTHEME_TEXT_DIM, pin_contours());
    refresh();
}

clipboard_pane::~clipboard_pane() {
    stlxgfx_destroy_surface(m_pin_glyph);
}

void clipboard_pane::refresh() {
    uint64_t selected_id = m_selected < m_listed.size() ? listed(m_selected).id : 0;
    std::vector<const clipboard_entry*> entries = m_source.history->entries();
    m_source.thumbnails->keep(entries);
    m_entries.clear();
    for (const clipboard_entry* entry : entries) {
        m_entries.push_back(show_entry(m_source, *entry));
    }

    ask_texts();
    list(selected_id);
}

void clipboard_pane::search(std::string query) {
    if (query == m_query) {
        return;
    }

    m_query = std::move(query);
    m_is_menu_open = false;
    m_is_asking = false;
    m_text_matches.clear();
    ask_texts();
    list(0);
}

/* The texts past 64 KB whose first 64 KB miss the query go to the searcher, and once one request was made a query
 * with none asks too, which stops the older search */
void clipboard_pane::ask_texts() {
    std::vector<uint64_t> ids;
    for (const shown_entry& entry : m_entries) {
        bool is_partly_read = entry.is_title_text && entry.text_len > PREVIEW_BYTES_MAX;
        if (!m_query.empty() && is_partly_read && find_folded(entry.text, m_query) == std::string_view::npos) {
            ids.push_back(entry.id);
        }
    }

    if (!ids.empty() || m_texts_request != 0) {
        m_texts_request = m_source.search_texts(m_query, ids);
    }

    m_is_reading_texts = !ids.empty() && m_texts_request != 0;
}

void clipboard_pane::show_text_matches(const text_matches& matches) {
    if (matches.request != m_texts_request || !m_is_reading_texts) {
        return;
    }

    m_is_reading_texts = false;
    m_text_matches = matches.ids;
    list(m_selected < m_listed.size() ? listed(m_selected).id : 0);
}

/* Lists the entries the query matches, keeping `selected_id` selected while it is listed, else the entry that took
 * its place, and the first without one */
void clipboard_pane::list(uint64_t selected_id) {
    size_t place = selected_id == 0 ? 0 : m_selected;
    m_listed.clear();
    for (size_t i = 0; i < m_entries.size(); i++) {
        if (!matches(m_entries[i])) {
            continue;
        }

        if (m_entries[i].id == selected_id) {
            place = m_listed.size();
        }

        m_listed.push_back(i);
    }

    m_selected = m_listed.empty() ? 0 : std::min(place, m_listed.size() - 1);
    build();
}

/* Text matches by its first 64 KB or by what the searcher found past them, files by their paths, and an image never */
bool clipboard_pane::matches(const shown_entry& entry) const {
    if (m_query.empty()) {
        return true;
    }

    if (!entry.files.empty()) {
        return std::any_of(entry.files.begin(), entry.files.end(), [this](const std::string& path) {
            return find_folded(path, m_query) != std::string_view::npos;
        });
    }

    bool is_found_past = std::find(m_text_matches.begin(), m_text_matches.end(), entry.id) != m_text_matches.end();
    return entry.is_title_text && (find_folded(entry.text, m_query) != std::string_view::npos || is_found_past);
}

/* The pinned entries head the column, then the rest since the history began, and a search lists its matches alone */
void clipboard_pane::build() {
    while (!m_children.empty()) {
        remove(m_children.back().get());
    }

    m_list = nullptr;
    m_preview = nullptr;
    m_rows.clear();
    m_row_tops.clear();
    if (m_entries.empty()) {
        add<clip_empty>("Nothing copied since Stellux started", "Copies appear here as you make them, newest first. "
                        "Pinned items stay until you unpin them, or until Stellux restarts.")->s().main =
            ui::length::flex();
        return;
    }

    if (m_listed.empty()) {
        add<clip_empty>("No clipboard items match " + m_query, "Only text and file paths are searched.")->s().main =
            ui::length::flex();
        return;
    }

    m_list = add<clip_list>();
    m_list->s().main = ui::length::fixed(COLUMN_WIDTH);
    m_list->s().padding.right = COLUMN_PADDING_RIGHT;
    add<column_rule>()->s().main = ui::length::fixed(1);
    m_preview = add<clip_preview>();
    m_preview->s().main = ui::length::flex();

    if (m_is_menu_open) {
        build_menu();
        return;
    }

    int64_t now_ns = monotonic_ns();
    int32_t top = 0;
    if (m_is_asking) {
        m_list->content()->add<clip_question>(unpinned_count());
        top += QUESTION_HEIGHT;
    }

    for (size_t i = 0; i < m_listed.size(); i++) {
        const shown_entry& entry = listed(i);
        bool has_heads = m_query.empty();
        if (has_heads && i == 0 && entry.is_pinned) {
            m_list->content()->add<section_head>("PINNED");
            top += HEAD_HEIGHT;
        }

        if (has_heads && !entry.is_pinned && (i == 0 || listed(i - 1).is_pinned)) {
            m_list->content()->add<section_head>("RECENT")->set_end("since " + m_source.since);
            top += HEAD_HEIGHT;
        }

        clip_row* row = m_list->content()->add<clip_row>(entry, detail_of(entry, now_ns), m_query, m_pin_glyph);
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
    if (!m_list || index >= m_row_tops.size()) {
        return;
    }

    m_selected = index;
    for (size_t i = 0; i < m_rows.size(); i++) {
        m_rows[i]->set_selected(i == index);
    }

    /* The first entry reveals the top of the list, and one under a head reveals the head with it */
    bool follows_head = index == 0 || m_row_tops[index] != m_row_tops[index - 1] + ENTRY_HEIGHT;
    int32_t reveal_top = index == 0 ? 0 : follows_head ? m_row_tops[index] - HEAD_HEIGHT : m_row_tops[index];
    int32_t row_bottom = m_row_tops[index] + ENTRY_HEIGHT;
    int32_t view_h = m_list->frame().h > 0 ? m_list->frame().h : PAGE_ENTRIES * ENTRY_HEIGHT;
    if (reveal_top < m_list->scroll_y()) {
        m_list->scroll_to(reveal_top);
    } else if (row_bottom > m_list->scroll_y() + view_h) {
        m_list->scroll_to(row_bottom - view_h);
    }

    m_preview->show(listed(index), m_source, m_query, monotonic_ns(), decode_image(listed(index)));
}

/* An image entry's whole picture, decoded for the preview each time it is selected */
stlxgfx_surface_t* clipboard_pane::decode_image(const shown_entry& shown) const {
    const clipboard_entry* entry = shown.is_image ? m_source.history->find(shown.id) : nullptr;
    const clipboard_rep* png = entry ? entry->item.find("image/png") : nullptr;
    if (!png) {
        return nullptr;
    }

    std::string bytes = read_bytes(*png, png->len);
    return stlxgfx_decode_image(bytes.data(), bytes.size());
}

void clipboard_pane::move_selection(int32_t rows) {
    if (m_listed.empty()) {
        return;
    }

    int32_t last = static_cast<int32_t>(m_listed.size()) - 1;
    select(static_cast<size_t>(std::clamp(static_cast<int32_t>(m_selected) + rows, 0, last)));
}

/* Enter pastes into the focused window when there is one, and otherwise copies, as Ctrl Enter always does */
void clipboard_pane::choose(bool pastes) {
    if (m_selected >= m_listed.size()) {
        return;
    }

    uint64_t id = listed(m_selected).id;
    if (pastes && !m_source.focused_app.empty()) {
        m_source.paste(id);
    } else {
        m_source.copy(id);
    }
}

/* Ctrl K's list names the entry it acts on, as "TEXT FROM SHEEN, 9 LINES", and replaces the entries until a choice
 * or Esc */
void clipboard_pane::build_menu() {
    const shown_entry& entry = listed(m_selected);
    std::string head = entry.kind + " from " + entry.from;
    std::string length = text_length(entry);
    if (!length.empty()) {
        head += ", " + length;
    }

    std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) {
        return static_cast<char>(toupper(c));
    });

    m_list->content()->add<section_head>(head.c_str());
    for (size_t i = 0; i < m_menu.size(); i++) {
        m_list->content()->add<clip_choice>(m_menu[i])->set_selected(i == m_menu_choice);
    }

    m_preview->show(entry, m_source, m_query, monotonic_ns(), decode_image(entry));
}

/* Enter pastes or copies, then come pinning, removing and clearing, each with the key that runs it outside the list */
std::vector<clip_action> clipboard_pane::actions_of(const shown_entry& entry) {
    uint64_t id = entry.id;
    std::vector<clip_action> actions;
    if (!m_source.focused_app.empty()) {
        actions.push_back({ "Paste into " + m_source.focused_app, "Enter", [this]() { choose(true); } });
    }

    actions.push_back({ "Copy", actions.empty() ? "Enter" : "Ctrl Enter", [this]() { choose(false); } });
    actions.push_back({ entry.is_pinned ? "Unpin" : "Pin", "Ctrl P", [this, id, pinned = entry.is_pinned]() {
        m_source.pin(id, !pinned);
    } });
    actions.push_back({ "Delete", "Delete", [this, id]() { m_source.remove(id); } });
    if (unpinned_count() > 0) {
        actions.push_back({ "Clear Unpinned\xE2\x80\xA6", "Ctrl Shift Delete", [this]() {
            m_is_asking = true;
            build();
        } });
    }

    return actions;
}

size_t clipboard_pane::unpinned_count() const {
    return static_cast<size_t>(std::count_if(m_entries.begin(), m_entries.end(), [](const shown_entry& entry) {
        return !entry.is_pinned;
    }));
}

bool clipboard_pane::on_menu_key(const ui::key_event& e) {
    bool is_ctrl = (e.modifiers & MOD_CTRL) != 0;
    switch (e.usage) {
    case HID_USAGE_UP:
    case HID_USAGE_DOWN:
        m_menu_choice = e.usage == HID_USAGE_UP ? (m_menu_choice + m_menu.size() - 1) % m_menu.size()
                                                : (m_menu_choice + 1) % m_menu.size();
        build();
        return true;
    case HID_USAGE_ENTER: {
        clip_action action = m_menu[m_menu_choice];
        m_is_menu_open = false;
        build();
        action.run();
        return true;
    }
    case HID_USAGE_ESCAPE:
        m_is_menu_open = false;
        build();
        return true;
    default:
        if (is_ctrl && e.usage == HID_USAGE_K) {
            m_is_menu_open = false;
            build();
            return true;
        }

        return false;
    }
}

/* The question takes Enter, which clears, and Esc, which keeps, and any other key keeps too */
bool clipboard_pane::on_question_key(const ui::key_event& e) {
    m_is_asking = false;
    if (e.usage == HID_USAGE_ENTER) {
        build();
        m_source.clear();
        return true;
    }

    build();
    return e.usage == HID_USAGE_ESCAPE;
}

bool clipboard_pane::on_key(const ui::key_event& e) {
    if (m_listed.empty()) {
        return false;
    }

    if (m_is_menu_open) {
        return on_menu_key(e);
    }

    if (m_is_asking) {
        return on_question_key(e);
    }

    bool is_ctrl = (e.modifiers & MOD_CTRL) != 0;
    bool is_shift = (e.modifiers & MOD_SHIFT) != 0;
    const shown_entry& entry = listed(m_selected);
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
    case HID_USAGE_DELETE:
        if (is_ctrl && is_shift) {
            m_is_asking = unpinned_count() > 0;
            build();
        } else {
            m_source.remove(entry.id);
        }

        return true;
    default:
        break;
    }

    if (is_ctrl && e.usage == HID_USAGE_P) {
        m_source.pin(entry.id, !entry.is_pinned);
        return true;
    }

    if (is_ctrl && e.usage == HID_USAGE_K) {
        m_menu = actions_of(entry);
        m_menu_choice = 0;
        m_is_menu_open = true;
        build();
        return true;
    }

    return false;
}

std::vector<footer_item> clipboard_pane::footer_actions() const {
    if (m_listed.empty()) {
        return {};
    }

    if (m_is_asking) {
        return { { "Clear", "Enter" }, { "Keep", "Esc" } };
    }

    if (m_is_menu_open) {
        return { { "Choose", "Enter" }, { "Back", "Esc" } };
    }

    std::vector<footer_item> items;
    if (m_source.focused_app.empty()) {
        items.push_back({ "Copy", "Enter" });
    } else {
        items.push_back({ "Paste into " + m_source.focused_app, "Enter" });
        items.push_back({ "Copy", "Ctrl Enter" });
    }

    items.push_back({ listed(m_selected).is_pinned ? "Unpin" : "Pin", "Ctrl P" });
    items.push_back({ "Actions", "Ctrl K" });
    return items;
}

/* The count of entries and the bytes the budget counts against its limit, or how many of them a search lists while
 * the searcher may find more */
std::string clipboard_pane::footer_end() const {
    if (!m_query.empty()) {
        std::string listed = std::to_string(m_listed.size()) + " of " + std::to_string(m_entries.size());
        return m_is_reading_texts ? listed + ", searching" : listed;
    }

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
