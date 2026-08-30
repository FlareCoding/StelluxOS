#ifndef STLXWIN_STLXWIN_H
#define STLXWIN_STLXWIN_H

/*
 * The Stellux windowing client library.
 *
 * An application connects to the display manager, creates windows,
 * paints frames into buffers, commits them with damage, and receives
 * events. Drawing is not provided here. A committed buffer is plain
 * pixels an app fills itself or through libstlxgfx.
 *
 * Everything an application waits on is the one connection fd, so the
 * whole library integrates into a poll loop alongside ptys, sockets,
 * and timers. The library never creates threads and never calls back.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct stlxwin_conn stlxwin_conn;
typedef struct stlxwin_window stlxwin_window;

typedef struct {
    int32_t x, y;
    int32_t w, h;
} stlxwin_rect;

/* A frame being painted. pixels is writable between begin_frame and
 * commit. The format is always 32-bit XRGB, rows stride bytes apart. */
typedef struct {
    uint32_t* pixels;
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;
} stlxwin_buffer;

enum {
    STLXWIN_WF_RESIZABLE  = 1u << 0,
    STLXWIN_WF_BORDERLESS = 1u << 1,
};

enum {
    /* Deliver one STLXWIN_EVT_FRAME after this commit is presented.
     * Request again on each commit for continuous animation. */
    STLXWIN_COMMIT_WANT_FRAME = 1u << 0,
};

enum {
    STLXWIN_PF_GRAB = 1u << 0,  /* outside input dismisses the popup */
};

typedef enum {
    STLXWIN_CURSOR_ARROW = 0,
    STLXWIN_CURSOR_IBEAM,
    STLXWIN_CURSOR_HAND,
    STLXWIN_CURSOR_RESIZE_H,
    STLXWIN_CURSOR_RESIZE_V,
    STLXWIN_CURSOR_NONE,
    STLXWIN_CURSOR_RESIZE_NWSE,
    STLXWIN_CURSOR_RESIZE_NESW,
} stlxwin_cursor;

typedef enum {
    STLXWIN_EVT_NONE = 0,
    STLXWIN_EVT_KEY_DOWN,
    STLXWIN_EVT_KEY_UP,
    STLXWIN_EVT_KEY_REPEAT,
    STLXWIN_EVT_POINTER_MOTION,
    STLXWIN_EVT_BUTTON_DOWN,
    STLXWIN_EVT_BUTTON_UP,
    STLXWIN_EVT_SCROLL,
    STLXWIN_EVT_POINTER_ENTER,
    STLXWIN_EVT_POINTER_LEAVE,
    STLXWIN_EVT_FOCUS_IN,
    STLXWIN_EVT_FOCUS_OUT,
    STLXWIN_EVT_CLOSE,           /* the user asked the window to close */
    STLXWIN_EVT_CONFIGURE,       /* size changed, repaint at the new size */
    STLXWIN_EVT_FRAME,           /* a WANT_FRAME commit was presented */
    STLXWIN_EVT_POPUP_DISMISSED, /* a grabbing child popup was dismissed */
    STLXWIN_EVT_DISCONNECTED,    /* the display manager is gone */
} stlxwin_event_type;

/* Key events carry the raw HID usage and the translated UTF-32
 * codepoint (0 when the key produces no character). Coordinates are
 * window content local. */
typedef struct {
    stlxwin_event_type type;
    stlxwin_window*    window;   /* NULL for DISCONNECTED */
    union {
        struct { uint16_t usage; uint8_t modifiers; uint32_t ch; } key;
        struct { int32_t x, y; } motion;
        struct { int32_t x, y; uint8_t button; } button;
        struct { int32_t x, y; int16_t dy; } scroll;
        struct { uint32_t width, height; } configure;
        struct { uint64_t time_ns; } frame;
    };
} stlxwin_event;

enum {
    STLXWIN_MOD_SHIFT = 1u << 0,
    STLXWIN_MOD_CTRL  = 1u << 1,
    STLXWIN_MOD_ALT   = 1u << 2,
    STLXWIN_MOD_SUPER = 1u << 3,
};

enum {
    STLXWIN_BTN_LEFT   = 0,
    STLXWIN_BTN_RIGHT  = 1,
    STLXWIN_BTN_MIDDLE = 2,
};

/**
 * @brief Connect to the display manager.
 * @return Connection, or NULL if no display manager is serving.
 */
stlxwin_conn* stlxwin_connect(const char* app_id);

/**
 * @brief Destroy every window on the connection and disconnect.
 */
void stlxwin_disconnect(stlxwin_conn* conn);

/**
 * @brief The connection's fd, readable when events are pending.
 * Poll it and call stlxwin_dispatch when it turns readable.
 */
int stlxwin_conn_fd(const stlxwin_conn* conn);

/**
 * @brief Drain protocol messages into the event queue. Never blocks.
 * @return Number of events queued, or -1 if the connection died.
 */
int stlxwin_dispatch(stlxwin_conn* conn);

/**
 * @brief Block until an event is queued or the timeout expires.
 * @param timeout_ns Nanoseconds, negative waits forever, 0 polls.
 * @return 1 with events queued, 0 on timeout, -1 if the connection died.
 */
int stlxwin_wait(stlxwin_conn* conn, int64_t timeout_ns);

/**
 * @brief Pop the next queued event. Never blocks.
 * @return 1 and fills *out, or 0 if the queue is empty.
 */
int stlxwin_next_event(stlxwin_conn* conn, stlxwin_event* out);

/**
 * @brief Create a toplevel window. It becomes visible on first commit.
 * @return Window, or NULL on protocol failure.
 */
stlxwin_window* stlxwin_window_create(stlxwin_conn* conn,
                                      uint32_t width, uint32_t height,
                                      const char* title, uint32_t flags);

/**
 * @brief Create a popup at (x, y) in parent content coordinates.
 * Popups stack above their parent, are undecorated, and die with it.
 * With STLXWIN_PF_GRAB the popup takes keyboard input and outside
 * input dismisses it, delivering POPUP_DISMISSED to the parent.
 */
stlxwin_window* stlxwin_popup_create(stlxwin_window* parent,
                                     int32_t x, int32_t y,
                                     uint32_t width, uint32_t height,
                                     uint32_t flags);

/**
 * @brief Destroy a window and, for a parent, its popups.
 */
void stlxwin_window_destroy(stlxwin_window* win);

void stlxwin_window_set_title(stlxwin_window* win, const char* title);
void stlxwin_window_set_min_size(stlxwin_window* win, uint32_t w, uint32_t h);
void stlxwin_window_set_max_size(stlxwin_window* win, uint32_t w, uint32_t h);
void stlxwin_window_set_fullscreen(stlxwin_window* win, int enabled);
void stlxwin_window_set_cursor(stlxwin_window* win, stlxwin_cursor cursor);

/**
 * @brief Current content size, tracking the latest configure.
 */
void stlxwin_window_get_size(const stlxwin_window* win,
                             uint32_t* w, uint32_t* h);

/**
 * @brief Acquire a buffer for the next frame, sized to the current
 * configure. Blocks dispatching until one is free, which paces a
 * render loop to the display manager. Events arriving meanwhile are
 * queued, not lost.
 * @return Buffer, or NULL if the window or connection is dead.
 */
stlxwin_buffer* stlxwin_begin_frame(stlxwin_window* win);

/**
 * @brief Non-blocking begin_frame.
 * @return Buffer, or NULL if none is currently free.
 */
stlxwin_buffer* stlxwin_try_begin_frame(stlxwin_window* win);

/**
 * @brief Submit a painted buffer. damage lists changed rectangles in
 * buffer coordinates, zero rects damages everything. The buffer
 * pointer is invalid after this call.
 * @return 0, or -1 if the window or connection is dead.
 */
int stlxwin_commit(stlxwin_window* win, stlxwin_buffer* buf,
                   const stlxwin_rect* damage, uint32_t n_damage,
                   uint32_t flags);

/**
 * @brief Replace the clipboard with UTF-8 text.
 * @return 0, or -1 on failure or text over the protocol limit.
 */
int stlxwin_clipboard_set(stlxwin_conn* conn, const char* text, size_t len);

/**
 * @brief Fetch the clipboard into a malloc'd buffer the caller frees.
 * @return Length, 0 if empty, -1 on failure.
 */
long stlxwin_clipboard_get(stlxwin_conn* conn, char** out_text);

#ifdef __cplusplus
}
#endif

#endif /* STLXWIN_STLXWIN_H */
