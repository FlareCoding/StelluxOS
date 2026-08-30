/* The first widgets: label, button, and checkbox. Painting follows
 * the theme, measurement asks the text engine, and every state
 * change invalidates exactly the widget it touched.
 */
#include <stlxui/stlxui.h>

namespace ui {

/* Buttons pad their text, checkboxes draw a square glyph beside it,
 * text fields inset their content, scrolling steps per wheel notch */
constexpr int32_t BUTTON_PAD_X = 12;
constexpr int32_t CHECK_GLYPH = 16;
constexpr int32_t CHECK_GAP = 8;
constexpr int32_t INPUT_PAD_X = 8;
constexpr int32_t SCROLL_STEP = 24;
constexpr int32_t SCROLL_BAR_W = 4;
constexpr int32_t SCROLL_THUMB_MIN = 16;

/* Measurement runs outside a paint pass, and text metrics come from
 * process global state, so a transient painter serves them */
static size text_metrics(const std::string& text) {
    painter p;
    return p.measure_text(text, 0);
}

void label::set_text(std::string text) {
    if (m_text == text) {
        return;
    }

    m_text = std::move(text);
    invalidate_layout();
}

void label::set_color(color c) {
    if (m_color == c) {
        return;
    }

    m_color = c;
    invalidate();
}

size label::measure(size) {
    size text = text_metrics(m_text);
    const edge_insets& pad = m_style.padding;

    return { text.w + pad.left + pad.right,
             text.h + pad.top + pad.bottom };
}

void label::paint(painter& p) {
    widget::paint(p);

    size text = p.measure_text(m_text, 0);
    int32_t baseline = (m_frame.h - text.h) / 2 + p.font_ascent(0);
    color c = m_color != 0 ? m_color : theme::active().text;

    p.text({ m_style.padding.left, baseline }, m_text, 0, c);
}

void button::set_text(std::string text) {
    if (m_text == text) {
        return;
    }

    m_text = std::move(text);
    invalidate_layout();
}

size button::measure(size) {
    size text = text_metrics(m_text);

    return { text.w + 2 * BUTTON_PAD_X, theme::active().control_h };
}

void button::paint(painter& p) {
    const theme& t = theme::active();
    color bg = m_pressed ? t.surface_press
             : m_hover ? t.surface_hover : t.surface;

    p.fill({ 0, 0, m_frame.w, m_frame.h }, bg);
    if (focused()) {
        p.stroke({ 0, 0, m_frame.w, m_frame.h }, t.accent);
    }

    size text = p.measure_text(m_text, 0);
    int32_t baseline = (m_frame.h - text.h) / 2 + p.font_ascent(0);

    p.text({ (m_frame.w - text.w) / 2, baseline }, m_text, 0, t.text);
}

bool button::on_pointer_down(const pointer_event&) {
    m_pressed = true;
    focus();
    invalidate();

    return true;
}

bool button::on_pointer_up(const pointer_event& e) {
    bool inside = e.pos.x >= 0 && e.pos.y >= 0 &&
                  e.pos.x < m_frame.w && e.pos.y < m_frame.h;
    bool fire = m_pressed && inside;

    m_pressed = false;
    invalidate();

    if (fire && on_click) {
        on_click();
    }

    return true;
}

void button::on_pointer_enter() {
    m_hover = true;
    invalidate();
}

void button::on_pointer_leave() {
    m_hover = false;
    invalidate();
}

bool button::on_key_down(const key_event& e) {
    if (e.ch != '\r' && e.ch != ' ') {
        return false;
    }

    if (on_click) {
        on_click();
    }

    return true;
}

void checkbox::set_checked(bool v) {
    if (m_checked == v) {
        return;
    }

    m_checked = v;
    invalidate();
}

size checkbox::measure(size) {
    size text = text_metrics(m_text);
    int32_t h = text.h > CHECK_GLYPH ? text.h : CHECK_GLYPH;

    return { CHECK_GLYPH + CHECK_GAP + text.w, h };
}

void checkbox::paint(painter& p) {
    const theme& t = theme::active();
    int32_t gy = (m_frame.h - CHECK_GLYPH) / 2;
    rect glyph = { 0, gy, CHECK_GLYPH, CHECK_GLYPH };

    p.fill(glyph, m_checked ? t.accent : t.surface);
    if (m_hover || focused()) {
        p.stroke(glyph, focused() ? t.accent : t.surface_hover);
    }

    if (m_checked) {
        p.line({ 4, gy + 8 }, { 7, gy + 11 }, t.window_bg);
        p.line({ 7, gy + 11 }, { 12, gy + 5 }, t.window_bg);
    }

    size text = p.measure_text(m_text, 0);
    int32_t baseline = (m_frame.h - text.h) / 2 + p.font_ascent(0);

    p.text({ CHECK_GLYPH + CHECK_GAP, baseline }, m_text, 0, t.text);
}

bool checkbox::on_pointer_up(const pointer_event& e) {
    bool inside = e.pos.x >= 0 && e.pos.y >= 0 &&
                  e.pos.x < m_frame.w && e.pos.y < m_frame.h;
    if (!inside) {
        return true;
    }

    m_checked = !m_checked;
    focus();
    invalidate();

    if (on_change) {
        on_change(m_checked);
    }

    return true;
}

bool checkbox::on_key_down(const key_event& e) {
    if (e.ch != '\r' && e.ch != ' ') {
        return false;
    }

    m_checked = !m_checked;
    invalidate();

    if (on_change) {
        on_change(m_checked);
    }

    return true;
}

void text_input::set_text(std::string text) {
    if (m_text == text) {
        return;
    }

    m_text = std::move(text);
    m_cursor = m_text.size();
    invalidate();
}

void text_input::set_placeholder(std::string text) {
    m_placeholder = std::move(text);
    invalidate();
}

size text_input::measure(size) {
    size text = text_metrics(!m_text.empty() ? m_text : m_placeholder);

    return { text.w + 2 * INPUT_PAD_X, theme::active().control_h };
}

void text_input::paint(painter& p) {
    const theme& t = theme::active();

    p.fill({ 0, 0, m_frame.w, m_frame.h }, t.surface);
    p.stroke({ 0, 0, m_frame.w, m_frame.h },
             focused() ? t.accent : t.surface_hover);

    bool empty = m_text.empty();
    const std::string& shown = empty ? m_placeholder : m_text;
    size text = p.measure_text(shown, 0);
    int32_t baseline = (m_frame.h - text.h) / 2 + p.font_ascent(0);

    p.text({ INPUT_PAD_X, baseline }, shown, 0,
           empty ? t.text_dim : t.text);

    if (focused()) {
        size before = p.measure_text(
            std::string_view(m_text).substr(0, m_cursor), 0);
        int32_t cx = INPUT_PAD_X + before.w;

        p.fill({ cx, (m_frame.h - text.h) / 2, 1, text.h }, t.text);
    }
}

bool text_input::on_pointer_down(const pointer_event& e) {
    focus();

    /* The cursor lands at the boundary nearest the click */
    painter p;
    size_t best = m_text.size();
    for (size_t i = 0; i <= m_text.size(); i++) {
        size w = p.measure_text(std::string_view(m_text).substr(0, i), 0);
        if (INPUT_PAD_X + w.w >= e.pos.x) {
            best = i;
            break;
        }
    }

    m_cursor = best;
    invalidate();

    return true;
}

bool text_input::on_key_down(const key_event& e) {
    if (e.ch == '\r') {
        if (on_submit) {
            on_submit(m_text);
        }

        return true;
    }

    if (e.ch == '\b') {
        if (m_cursor > 0) {
            m_text.erase(m_cursor - 1, 1);
            m_cursor--;
            invalidate();

            if (on_change) {
                on_change(m_text);
            }
        }

        return true;
    }

    /* Editing keys arrive as usages since they carry no character */
    switch (e.usage) {
    case 0x4C:
        if (m_cursor < m_text.size()) {
            m_text.erase(m_cursor, 1);
            invalidate();

            if (on_change) {
                on_change(m_text);
            }
        }
        return true;
    case 0x50:
        if (m_cursor > 0) {
            m_cursor--;
            invalidate();
        }
        return true;
    case 0x4F:
        if (m_cursor < m_text.size()) {
            m_cursor++;
            invalidate();
        }
        return true;
    case 0x4A:
        m_cursor = 0;
        invalidate();
        return true;
    case 0x4D:
        m_cursor = m_text.size();
        invalidate();
        return true;
    default:
        break;
    }

    if (e.ch >= 32 && e.ch < 127) {
        m_text.insert(m_cursor, 1, static_cast<char>(e.ch));
        m_cursor++;
        invalidate();

        if (on_change) {
            on_change(m_text);
        }

        return true;
    }

    return false;
}

size scroll_view::measure(size avail) {
    return { avail.w, avail.h };
}

void scroll_view::paint(painter& p) {
    widget::paint(p);

    if (m_children.empty()) {
        return;
    }

    /* A proportional thumb on the right edge shows the position */
    int32_t content_h = m_children[0]->frame().h;
    if (content_h <= m_frame.h) {
        return;
    }

    const theme& t = theme::active();
    int32_t track = m_frame.h;
    int32_t thumb_h = track * m_frame.h / content_h;
    if (thumb_h < SCROLL_THUMB_MIN) {
        thumb_h = SCROLL_THUMB_MIN;
    }

    int32_t range = content_h - m_frame.h;
    int32_t thumb_y = range > 0
                    ? (track - thumb_h) * m_scroll.y / range : 0;

    p.fill({ m_frame.w - SCROLL_BAR_W, thumb_y, SCROLL_BAR_W, thumb_h },
           t.surface_hover);
}

bool scroll_view::on_scroll(const pointer_event& e) {
    if (m_children.empty()) {
        return false;
    }

    int32_t content_h = m_children[0]->frame().h;
    int32_t range = content_h - m_frame.h;
    if (range <= 0) {
        return false;
    }

    int32_t next = m_scroll.y - e.scroll_dy * SCROLL_STEP;
    if (next < 0) {
        next = 0;
    }
    if (next > range) {
        next = range;
    }

    if (next == m_scroll.y) {
        return true;
    }

    m_scroll.y = next;
    invalidate();

    return true;
}

void canvas::damage(const rect&) {
    invalidate();
}

void canvas::paint(painter& p) {
    widget::paint(p);

    if (on_paint) {
        on_paint(p, { m_frame.w, m_frame.h });
    }
}

bool canvas::on_pointer_down(const pointer_event& e) {
    focus();

    if (on_pointer) {
        return on_pointer(e);
    }

    return false;
}

bool canvas::on_key_down(const key_event& e) {
    if (on_key) {
        return on_key(e);
    }

    return false;
}

} // namespace ui
