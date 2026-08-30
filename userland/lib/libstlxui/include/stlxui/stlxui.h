#ifndef STLXUI_STLXUI_H
#define STLXUI_STLXUI_H

/**
 * stlxui: the Stellux widget toolkit.
 *
 * A retained widget tree with one-axis flexbox layout, damage aware
 * repainting, and no framework magic: widgets are objects mutated
 * through setters and wired with std::function callbacks. A setter
 * invalidates its widget, and the host repaints exactly the damaged
 * rectangles at the next flush. Trees render through a host, either a
 * window_host over an stlxwin window or an embedder's private host,
 * which is what keeps the toolkit independent of the window protocol.
 * Parents own children, add() lends a pointer that stays valid until
 * the parent dies or remove() runs, and nothing here is thread aware.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct stlxwin_conn;
struct stlxwin_window;

namespace ui {

/* Geometry */

struct point {
    int32_t x = 0, y = 0;
};

struct size {
    int32_t w = 0, h = 0;
};

struct rect {
    int32_t x = 0, y = 0;
    int32_t w = 0, h = 0;

    bool contains(point p) const {
        return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h;
    }
};

/* Colors are packed ARGB, matching the pinned surface format */
using color = uint32_t;

/* Layout */

enum class axis : uint8_t { row, column };

/**
 * One dimension of a widget's size: a fixed pixel count, the widget's
 * measured content, or a weighted share of the parent's free space.
 */
struct length {
    enum class kind : uint8_t { fixed, content, flex };

    kind k = kind::content;
    int32_t value = 0;     /* px for fixed, weight for flex */

    static length fixed(int32_t px) { return { kind::fixed, px }; }
    static length content() { return { kind::content, 0 }; }
    static length flex(int32_t weight = 1) { return { kind::flex, weight }; }
};

enum class align : uint8_t { start, center, end, stretch };

struct edge_insets {
    int32_t top = 0, right = 0, bottom = 0, left = 0;

    static edge_insets all(int32_t v) { return { v, v, v, v }; }
    static edge_insets xy(int32_t h, int32_t v) { return { v, h, v, h }; }
};

/**
 * Per widget layout parameters, plain data. Mutate through s() and the
 * next layout pass picks the change up.
 */
struct style {
    axis direction = axis::column;    /* containers: child flow axis */
    length main = length::content();  /* size along the parent's axis */
    length cross = length::content(); /* size across the parent's axis */
    align align_self = align::stretch;
    align justify = align::start;     /* containers: main axis packing */
    align align_items = align::stretch;
    edge_insets padding;
    int32_t gap = 0;
    color background = 0;             /* 0 is transparent */
};

/**
 * The palette and defaults every widget consults. One process global
 * instance, replaceable as a whole, no per widget cascade.
 */
struct theme {
    color window_bg = 0xFF1E1E2E;
    color surface = 0xFF313244;
    color surface_hover = 0xFF45475A;
    color surface_press = 0xFF585B70;
    color accent = 0xFF89B4FA;
    color danger = 0xFFF38BA8;
    color text = 0xFFCDD6F4;
    color text_dim = 0xFF585B70;
    uint32_t font_size = 14;
    int32_t control_h = 28;    /* default height of one line controls */

    static const theme& active();
    static void set_active(const theme& t);
};

/* Painting */

/**
 * Drawing interface handed to widget::paint. Coordinates are widget
 * local, the host pre applies translation and clipping, and every
 * operation clips to the widget's frame. Implemented over libstlxgfx.
 */
class painter {
public:
    void fill(const rect& r, color c);
    void stroke(const rect& r, color c);
    void line(point a, point b, color c);

    /**
     * @brief Draws UTF-8 text with the pen on the baseline.
     * @param baseline_origin Pen start, y is the baseline.
     * @param utf8 Text to draw.
     * @param font_size Face pixel size, 0 uses the theme size.
     * @param c Text color.
     */
    void text(point baseline_origin, std::string_view utf8,
              uint32_t font_size, color c);

    /**
     * @brief Measures UTF-8 text: advance width and line height.
     * @param utf8 Text to measure.
     * @param font_size Face pixel size, 0 uses the theme size.
     * @return Width and height in pixels.
     */
    size measure_text(std::string_view utf8, uint32_t font_size) const;

    /** @brief Font ascent in pixels, for baseline placement. */
    int32_t font_ascent(uint32_t font_size) const;

    void push_clip(const rect& r);
    void pop_clip();

private:
    friend class host;
    void* m_target = nullptr;    /* stlxgfx surface */
    point m_origin;
    std::vector<rect> m_clips;
};

/* Events, delivered widget local */

struct pointer_event {
    point pos;
    uint8_t button = 0;
    int16_t scroll_dy = 0;
};

struct key_event {
    uint16_t usage = 0;
    uint8_t modifiers = 0;    /* STLXWIN_MOD_* bits */
    uint32_t ch = 0;          /* translated codepoint, 0 when none */
};

class host;

/* Widget */

/**
 * The tree node every control derives from. A widget owns its style,
 * its children, and its layout results. Overridables cover measuring,
 * painting, and input. Handlers return true to stop the event from
 * bubbling to the parent.
 */
class widget {
public:
    widget() = default;
    virtual ~widget() = default;
    widget(const widget&) = delete;
    widget& operator=(const widget&) = delete;

    /**
     * @brief Constructs a child in place and adopts it.
     * @return Borrowed pointer, valid until the parent dies or
     * remove() destroys the child.
     */
    template <typename W, typename... Args>
    W* add(Args&&... args) {
        auto child = std::make_unique<W>(static_cast<Args&&>(args)...);
        W* raw = child.get();
        adopt(std::move(child));
        return raw;
    }

    /** @brief Removes and destroys one child. */
    void remove(widget* child);

    widget* parent() const { return m_parent; }

    style& s() { return m_style; }
    const style& s() const { return m_style; }

    /** @brief Marks the widget's pixels stale, repaint only. */
    void invalidate();

    /** @brief Marks geometry stale, relayout then repaint. */
    void invalidate_layout();

    /** @brief Layout result in parent coordinates, set by the host. */
    rect frame() const { return m_frame; }

    /** @brief Moves keyboard focus here when focusable. */
    void focus();
    bool focused() const;
    virtual bool focusable() const { return false; }

    /**
     * @brief Reports the content size for layout.
     * @param avail Space offered by the parent.
     * @return Desired size, the default wraps the children.
     */
    virtual size measure(size avail);

    /** @brief Paints this widget, the default fills the background. */
    virtual void paint(painter& p);

    virtual bool on_pointer_down(const pointer_event& e);
    virtual bool on_pointer_up(const pointer_event& e);
    virtual bool on_pointer_move(const pointer_event& e);
    virtual void on_pointer_enter();
    virtual void on_pointer_leave();
    virtual bool on_scroll(const pointer_event& e);
    virtual bool on_key_down(const key_event& e);
    virtual bool on_key_up(const key_event& e);
    virtual void on_focus_in() {}
    virtual void on_focus_out() {}

protected:
    void adopt(std::unique_ptr<widget> child);

    style m_style;
    rect m_frame;
    widget* m_parent = nullptr;
    host* m_host = nullptr;
    std::vector<std::unique_ptr<widget>> m_children;
    bool m_needs_paint = true;
    bool m_needs_layout = true;

    friend class host;
};

/* Built in widgets, the v1 set */

/** A plain container flowing children along one axis. */
class box : public widget {
public:
    explicit box(axis dir = axis::column) { m_style.direction = dir; }
};

/** One line of themed text. */
class label : public widget {
public:
    explicit label(std::string text) : m_text(std::move(text)) {}

    void set_text(std::string text);
    const std::string& text() const { return m_text; }
    void set_color(color c);

    size measure(size avail) override;
    void paint(painter& p) override;

private:
    std::string m_text;
    color m_color = 0;    /* 0 uses the theme text color */
};

/** A push button firing on_click on release or enter. */
class button : public widget {
public:
    explicit button(std::string text) : m_text(std::move(text)) {}

    std::function<void()> on_click;

    void set_text(std::string text);
    bool focusable() const override { return true; }
    size measure(size avail) override;
    void paint(painter& p) override;
    bool on_pointer_down(const pointer_event& e) override;
    bool on_pointer_up(const pointer_event& e) override;
    void on_pointer_enter() override;
    void on_pointer_leave() override;
    bool on_key_down(const key_event& e) override;

private:
    std::string m_text;
    bool m_hover = false;
    bool m_pressed = false;
};

/** A labeled toggle firing on_change with the new state. */
class checkbox : public widget {
public:
    explicit checkbox(std::string text, bool checked = false)
        : m_text(std::move(text)), m_checked(checked) {}

    std::function<void(bool)> on_change;

    bool checked() const { return m_checked; }
    void set_checked(bool v);
    bool focusable() const override { return true; }
    size measure(size avail) override;
    void paint(painter& p) override;
    bool on_pointer_up(const pointer_event& e) override;
    bool on_key_down(const key_event& e) override;

private:
    std::string m_text;
    bool m_checked = false;
    bool m_hover = false;
};

/**
 * A single line text field. Characters arrive display manager
 * translated, so insertion is the codepoint from the event. Fires
 * on_change per edit and on_submit on enter.
 */
class text_input : public widget {
public:
    text_input() = default;

    std::function<void(const std::string&)> on_change;
    std::function<void(const std::string&)> on_submit;

    const std::string& text() const { return m_text; }
    void set_text(std::string text);
    void set_placeholder(std::string text);

    bool focusable() const override { return true; }
    size measure(size avail) override;
    void paint(painter& p) override;
    bool on_pointer_down(const pointer_event& e) override;
    bool on_key_down(const key_event& e) override;

private:
    std::string m_text;
    std::string m_placeholder;
    size_t m_cursor = 0;
};

/**
 * A vertical viewport over its first child, scrolled by wheel. The
 * child lays out at its content height and clips to the view.
 */
class scroll_view : public widget {
public:
    scroll_view() = default;

    size measure(size avail) override;
    void paint(painter& p) override;
    bool on_scroll(const pointer_event& e) override;

private:
    int32_t m_offset = 0;
};

/**
 * An app painted region, the escape hatch for plots and grids. The
 * paint callback draws widget local, damage() requests a partial
 * repaint of one region without touching the rest of the tree.
 */
class canvas : public widget {
public:
    std::function<void(painter&, size)> on_paint;
    std::function<bool(const pointer_event&)> on_pointer;
    std::function<bool(const key_event&)> on_key;

    bool focusable() const override { return true; }
    void damage(const rect& r);
    void paint(painter& p) override;
    bool on_pointer_down(const pointer_event& e) override;
    bool on_key_down(const key_event& e) override;
};

/* Menus */

struct menu_item {
    std::string text;
    std::function<void()> on_select;
    bool enabled = true;
    bool separator_after = false;
};

/**
 * A menu is a grabbing popup window hosting a column of items, owned
 * by the toolkit for its whole life: it dies on selection, escape, or
 * a press outside it. Requires the anchor to live in a window_host.
 */
class menu {
public:
    /** @brief Opens below the anchor widget. */
    static void open_at(widget* anchor, std::vector<menu_item> items);
};

/* Hosts and the app loop */

/**
 * Owns a widget tree and runs its lifecycle: layout when geometry is
 * stale, repaint of dirty subtrees, damage accumulation, focus, hover,
 * and event dispatch. Where pixels and events come from is the
 * subclass's business, which is what keeps trees protocol independent.
 */
class host {
public:
    virtual ~host() = default;

    widget* root() { return m_root.get(); }

    /** @brief Installs the tree, replacing any existing one. */
    void set_root(std::unique_ptr<widget> root);

    /** @brief Runs layout and repaint if anything is dirty. */
    virtual void flush() = 0;

protected:
    /* Layout, paint, and input plumbing shared by every host kind,
     * implemented by the toolkit core. paint_tree binds the painter
     * internally, since only host code may configure one, and paints
     * exactly the subtrees whose widgets invalidated. */
    void layout_tree(size viewport);
    void paint_tree(void* surface, std::vector<rect>& damage_out);

    /* Deepest widget under a window space point and the point in its
     * local space, or null outside the tree */
    widget* hit_at(point p, point* out_local);

    /* Whether any widget awaits layout or paint, checked before a
     * frame buffer is acquired since acquisition can block */
    bool tree_dirty() const;

    /* Invalidates every widget intersecting the given window space
     * rects, which is how a swapped in buffer catches up with the
     * frame it missed */
    void invalidate_rects(const std::vector<rect>& rects);
    void dispatch_pointer_move(point p);
    void dispatch_pointer_button(point p, uint8_t button, bool down);
    void dispatch_scroll(point p, int16_t dy);
    void dispatch_key(const key_event& e, bool down);
    void focus_next();

    std::unique_ptr<widget> m_root;
    widget* m_focus = nullptr;
    widget* m_hover = nullptr;
    widget* m_pointer_grab = nullptr;

    friend class widget;
};

/**
 * A host bound to one stlxwin window, created through ui::app. Resize
 * configures relayout the tree, damage commits per flush, and the
 * window closes when on_close allows it.
 */
class window_host : public host {
public:
    ~window_host() override;

    std::function<bool()> on_close;    /* return false to keep open */

    void flush() override;
    stlxwin_window* window() const { return m_win; }

private:
    friend class app;
    explicit window_host(class app& a);

    class app* m_app = nullptr;
    stlxwin_window* m_win = nullptr;

    /* Buffer parity: the previous frame's damage repaints into the
     * swapped in slot of the buffer pair */
    void* m_last_pixels = nullptr;
    uint32_t m_last_w = 0;
    uint32_t m_last_h = 0;
    std::vector<rect> m_last_damage;
};

/**
 * The application object: one connection, one poll loop, any number
 * of windows, external file descriptors, and one shot timers. run()
 * returns when the last window closes or quit() is called. Timers
 * exist only while pending, so an idle app never wakes.
 */
class app {
public:
    explicit app(const char* app_id);
    ~app();
    app(const app&) = delete;
    app& operator=(const app&) = delete;

    /** @brief Whether the display manager connection came up. */
    bool ok() const { return m_conn != nullptr; }

    /**
     * @brief Creates a window with a tree host over it.
     * @param w Content width in pixels.
     * @param h Content height in pixels.
     * @param title UTF-8 window title.
     * @param win_flags STLXWIN_WF_* bits.
     * @return Borrowed host pointer, owned by the app, or NULL.
     */
    window_host* create_window(uint32_t w, uint32_t h, const char* title,
                               uint32_t win_flags = 0);

    /** @brief Calls on_readable whenever fd polls readable. */
    void watch_fd(int fd, std::function<void()> on_readable);
    void unwatch_fd(int fd);

    /**
     * @brief Arms a one shot timer.
     * @param after_ns Delay from now in nanoseconds.
     * @param fn Callback run on the app loop.
     * @return Timer id for cancel_timer.
     */
    uint64_t set_timer(int64_t after_ns, std::function<void()> fn);
    void cancel_timer(uint64_t id);

    /** @brief Runs the loop until quit or the last window closes. */
    int run();
    void quit(int code = 0);

    stlxwin_conn* conn() const { return m_conn; }

private:
    friend class window_host;
    friend class menu;

    struct fd_watch {
        int fd;
        std::function<void()> on_readable;
    };

    struct timer {
        uint64_t id;
        uint64_t deadline_ns;
        std::function<void()> fn;
    };

    stlxwin_conn* m_conn = nullptr;
    std::vector<std::unique_ptr<window_host>> m_hosts;
    std::vector<fd_watch> m_watches;
    std::vector<timer> m_timers;
    uint64_t m_next_timer_id = 1;
    bool m_running = false;
    int m_exit_code = 0;
};

} // namespace ui

#endif /* STLXUI_STLXUI_H */
