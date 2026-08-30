/* The first widgets: label, button, and checkbox. Painting follows
 * the theme, measurement asks the text engine, and every state
 * change invalidates exactly the widget it touched.
 */
#include <stlxui/stlxui.h>

namespace ui {

/* Buttons pad their text, checkboxes draw a square glyph beside it */
constexpr int32_t BUTTON_PAD_X = 12;
constexpr int32_t CHECK_GLYPH = 16;
constexpr int32_t CHECK_GAP = 8;

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

} // namespace ui
