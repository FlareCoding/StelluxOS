/* Menus: a grabbing protocol popup hosting a column of items. The
 * toolkit owns the popup's whole life, closing it on selection,
 * escape, or the compositor reporting a press outside it.
 */
#include <stlxui/stlxui.h>

#include <stlxwin/stlxwin.h>

namespace ui {

/* One selectable line, highlight driven by the list */
class menu_row : public widget {
public:
    menu_row(std::string text, size_t index, bool enabled)
        : m_text(std::move(text)), m_index(index), m_enabled(enabled) {}

    std::function<void(size_t)> on_pick;
    std::function<void(size_t)> on_hot;

    void set_hot(bool hot) {
        if (m_hot == hot) {
            return;
        }

        m_hot = hot;
        invalidate();
    }

    size measure(size) override {
        painter p;
        size text = p.measure_text(m_text, 0);

        return { text.w + 2 * STLXTHEME_MENU_ROW_PADDING_X, STLXTHEME_MENU_ROW_HEIGHT };
    }

    void paint(painter& p) override {
        const theme& t = theme::active();
        bool is_lit = m_hot && m_enabled;

        if (is_lit) {
            p.rounded_rect({ 0, 0, m_frame.w, m_frame.h }, STLXTHEME_MENU_ITEM_RADIUS, t.surface_selected);
        }

        size text = p.measure_text(m_text, 0);
        int32_t baseline = (m_frame.h - text.h) / 2 + p.font_ascent(0);
        color fg = !m_enabled ? t.text_disabled : is_lit ? t.text : t.text_secondary;

        p.text({ STLXTHEME_MENU_ROW_PADDING_X, baseline }, m_text, 0, fg);
    }

    void on_pointer_enter() override {
        if (m_enabled && on_hot) {
            on_hot(m_index);
        }
    }

    bool on_pointer_up(const pointer_event&) override {
        if (m_enabled && on_pick) {
            on_pick(m_index);
        }

        return true;
    }

    bool on_pointer_down(const pointer_event&) override { return true; }

private:
    std::string m_text;
    size_t m_index = 0;
    bool m_enabled = true;
    bool m_hot = false;
};

/* The column of rows, owning keyboard navigation and the hot index */
class menu_list : public widget {
public:
    std::function<void(size_t)> on_pick;
    std::function<void()> on_dismiss;

    explicit menu_list(const std::vector<menu_item>& items) {
        m_style.direction = axis::column;
        m_style.padding = edge_insets::all(STLXTHEME_MENU_PADDING);
        m_style.background = theme::active().menu_bg;

        for (size_t i = 0; i < items.size(); i++) {
            m_enabled.push_back(items[i].enabled);

            menu_row* row = add<menu_row>(items[i].text, i,
                                          items[i].enabled);
            row->s().main = length::fixed(STLXTHEME_MENU_ROW_HEIGHT);
            row->on_pick = [this](size_t idx) { pick(idx); };
            row->on_hot = [this](size_t idx) { set_hot(idx); };
            m_rows.push_back(row);

            if (items[i].separator_after && i + 1 < items.size()) {
                widget* sep = add<box>();
                sep->s().main = length::fixed(STLXTHEME_MENU_SEPARATOR_HEIGHT);
            }
        }
    }

    bool focusable() const override { return true; }

    void paint(painter& p) override {
        widget::paint(p);
        p.stroke({ 0, 0, m_frame.w, m_frame.h }, theme::active().line);
    }

    bool on_key_down(const key_event& e) override {
        if (e.usage == 0x51) {
            move_hot(1);
            return true;
        }
        if (e.usage == 0x52) {
            move_hot(-1);
            return true;
        }

        if (e.ch == '\r') {
            if (m_hot >= 0 && m_enabled[static_cast<size_t>(m_hot)]) {
                pick(static_cast<size_t>(m_hot));
            }
            return true;
        }

        if (e.usage == 0x29) {
            if (on_dismiss) {
                on_dismiss();
            }
            return true;
        }

        return false;
    }

private:
    void set_hot(size_t idx) {
        if (m_hot >= 0) {
            m_rows[static_cast<size_t>(m_hot)]->set_hot(false);
        }

        m_hot = static_cast<int32_t>(idx);
        m_rows[idx]->set_hot(true);
    }

    /* Steps to the next enabled row, wrapping at the ends */
    void move_hot(int32_t dir) {
        if (m_rows.empty()) {
            return;
        }

        int32_t count = static_cast<int32_t>(m_rows.size());
        int32_t idx = m_hot;
        for (int32_t step = 0; step < count; step++) {
            idx = (idx + dir + count) % count;
            if (m_enabled[static_cast<size_t>(idx)]) {
                set_hot(static_cast<size_t>(idx));
                return;
            }
        }
    }

    void pick(size_t idx) {
        if (on_pick) {
            on_pick(idx);
        }
    }

    std::vector<menu_row*> m_rows;
    std::vector<bool> m_enabled;
    int32_t m_hot = -1;
};

void menu::open_at(widget* anchor, std::vector<menu_item> items) {
    if (!anchor || !anchor->m_host || items.empty()) {
        return;
    }

    stlxwin_window* parent_win = anchor->m_host->protocol_window();
    if (!parent_win) {
        return;
    }

    window_host* anchor_host = nullptr;
    app* a = nullptr;
    {
        /* The anchor's host is a window_host when a window exists */
        anchor_host = static_cast<window_host*>(anchor->m_host);
        a = anchor_host->m_app;
    }
    if (!a || a->m_menu_host) {
        return;
    }

    /* The popup sits below the anchor in parent content space */
    point pos = { 0, 0 };
    for (widget* w = anchor; w != nullptr; w = w->m_parent) {
        pos.x += w->m_frame.x;
        pos.y += w->m_frame.y;

        if (w->m_parent) {
            pos.x -= w->m_parent->m_scroll.x;
            pos.y -= w->m_parent->m_scroll.y;
        }
    }

    /* Size from the widest item plus chrome */
    painter measure;
    int32_t width = 0;
    int32_t height = 2 * STLXTHEME_MENU_PADDING;
    for (const menu_item& it : items) {
        size text = measure.measure_text(it.text, 0);
        if (text.w + 2 * STLXTHEME_MENU_ROW_PADDING_X > width) {
            width = text.w + 2 * STLXTHEME_MENU_ROW_PADDING_X;
        }

        height += STLXTHEME_MENU_ROW_HEIGHT;
        if (it.separator_after) {
            height += STLXTHEME_MENU_SEPARATOR_HEIGHT;
        }
    }

    width += 2 * STLXTHEME_MENU_PADDING;

    auto host = std::unique_ptr<window_host>(new window_host(*a));
    host->m_win = stlxwin_popup_create(
        parent_win, pos.x, pos.y + anchor->m_frame.h,
        static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        STLXWIN_PF_GRAB);
    if (!host->m_win) {
        return;
    }

    /* Selection and escape only mark retirement, the app loop tears
     * the popup down after dispatch unwinds, then runs the selection
     * callback, since this code executes inside the dying host */
    window_host* raw = host.get();
    auto list = std::make_unique<menu_list>(items);
    menu_list* list_raw = list.get();

    std::vector<std::function<void()>> selects;
    for (menu_item& it : items) {
        selects.push_back(std::move(it.on_select));
    }

    list_raw->on_pick = [a, selects](size_t idx) {
        a->m_menu_retire = true;
        a->m_menu_after_close = selects[idx];
    };
    list_raw->on_dismiss = [a]() {
        a->m_menu_retire = true;
    };

    host->set_root(std::move(list));
    list_raw->focus();

    a->m_menu_host = raw;
    a->m_hosts.push_back(std::move(host));
}

} // namespace ui
