#ifndef STLXWIN_STLXWIN_H
#define STLXWIN_STLXWIN_H

/*
 * The Stellux windowing client library.
 *
 * An application connects to the display manager, creates windows,
 * paints frames into buffers, commits them with damage, and receives
 * events. Drawing is not provided here. A committed buffer is plain
 * pixels an app fills itself or through libstlxgfx, or a GPU buffer
 * the display manager reads only once its acquire point is reached
 * and gives back by raising its release timeline.
 *
 * Everything an application waits on, except a GPU buffer's release,
 * is the one connection fd, so the whole library integrates into a
 * poll loop alongside ptys, sockets, and timers. The library never
 * creates threads and never calls back.
 *
 * The GPU calls and stlxwin_window_configured may run on any thread,
 * and a window or connection must outlive every such call on it.
 * Every other call stays on the thread that dispatches, and events go
 * only to it. A call that sends may wait up to 100 ms on a full
 * socket, and others may wait behind it.
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
 * commit. Rows lie stride bytes apart, in 32-bit XRGB, or premultiplied
 * ARGB for a window made with STLXWIN_WF_PREMULTIPLIED or
 * STLXWIN_PF_PREMULTIPLIED. */
typedef struct {
    uint32_t* pixels;
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;
} stlxwin_buffer;

/* STLXWIN_WF_GLASS lays the display manager's glass material under a borderless window, as the display manager does
 * under every decorated window's frame, and premultiplied pixels show it through their alpha. Premultiplied pixels
 * need STLXWIN_WF_GLASS even on a decorated window, or the display manager disconnects the client. */
enum {
    STLXWIN_WF_RESIZABLE     = 1u << 0,
    STLXWIN_WF_BORDERLESS    = 1u << 1,
    STLXWIN_WF_GLASS         = 1u << 2,
    STLXWIN_WF_PREMULTIPLIED = 1u << 3,  /* stlxwin_buffer pixels hold premultiplied ARGB, GPU buffers stay XRGB */
};

enum {
    /* Deliver one STLXWIN_EVT_FRAME after this commit is presented.
     * Request again on each commit for continuous animation. */
    STLXWIN_COMMIT_WANT_FRAME = 1u << 0,
};

enum {
    STLXWIN_PF_GRAB          = 1u << 0,  /* outside input dismisses the popup */
    STLXWIN_PF_GLASS         = 1u << 1,  /* as STLXWIN_WF_GLASS */
    STLXWIN_PF_PREMULTIPLIED = 1u << 2,  /* as STLXWIN_WF_PREMULTIPLIED */
    STLXWIN_PF_ROUNDED       = 1u << 3,  /* glass rounded by STLXTHEME_MENU_RADIUS to match the popup's panel */
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
    STLXWIN_EVT_MODES,           /* a watched output's modes arrived, read them with stlxwin_modes_get */
    STLXWIN_EVT_MODE_RESULT,     /* the display manager's answer to a mode request or keep */
    STLXWIN_EVT_GPU_INFO,        /* which GPU buffers the display manager takes changed, see stlxwin_gpu_info_get */
    STLXWIN_EVT_OUTPUTS,         /* what stlxwin_outputs_get gives changed, read it again */
    STLXWIN_EVT_LAYOUT_RESULT,   /* the display manager's answer to a layout request */
    STLXWIN_EVT_WINDOW_OUTPUT,   /* a toplevel's output or position changed, see stlxwin_window_get_output */
} stlxwin_event_type;

#define STLXWIN_MODES_MAX   32u  /* the most modes an output lists */
#define STLXWIN_OUTPUTS_MAX 16u  /* the most outputs a display drives */

enum {
    STLXWIN_OUTPUT_LIT     = 1u << 0,  /* the output shows a picture */
    STLXWIN_OUTPUT_PRIMARY = 1u << 1,  /* gfxfb's primary output */
};

enum {
    STLXWIN_SET_MODE_TRIAL = 1u << 0,  /* set back after 15 seconds unless kept */
};

/* What the display manager did for a mode request or keep */
enum {
    STLXWIN_MODE_SET        = 1,  /* asked without a trial */
    STLXWIN_MODE_TRIAL      = 2,  /* asked as a trial, its timer running when the result is 0 */
    STLXWIN_MODE_KEPT       = 3,  /* the trial's mode kept, nothing asked */
    STLXWIN_MODE_SET_BACK   = 4,  /* the trial's previous mode asked for again */
    STLXWIN_MODE_DEFAULT    = 5,  /* a set-back failed, so the output's default was asked for */
    STLXWIN_MODE_NO_BUFFERS = 6,  /* no desktop buffers at the new size, so the previous mode was asked for */
    STLXWIN_MODE_BUSY       = 7,  /* refused, nothing asked: a trial is running */
    STLXWIN_MODE_NO_TRIAL   = 8,  /* refused, nothing asked: no trial of this connection's on the output */
    STLXWIN_MODE_INVALID    = 9,  /* refused, nothing asked: an unknown flag or keep value */
};

/* A picture size and refresh rate an output can light at */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint64_t refresh_millihertz; /* 0 when unknown */
} stlxwin_mode;

typedef struct {
    uint32_t     output_count;
    uint32_t     flags;   /* STLXWIN_OUTPUT_* */
    stlxwin_mode current; /* zero while the output is dark */
} stlxwin_modes_info;

enum {
    STLXWIN_DESKTOP_MAIN = 1u << 0,  /* the main output, carrying the bar and the dock, its top-left at (0, 0) */
};

/* An output's place in desktop coordinates, which count monitor pixels
 * right and down from the main output's top-left */
typedef struct {
    uint32_t     output;
    uint32_t     flags;              /* STLXWIN_DESKTOP_* */
    int32_t      x, y;
    uint32_t     width, height;      /* the mode's size, the last one while the output relights */
    uint64_t     refresh_millihertz; /* 0 when unknown */
    stlxwin_rect work;               /* the main output's place less the bar and the dock, else the place */
    char         monitor[32];        /* "maker name", NUL-terminated, empty when the output reports no identity */
} stlxwin_output;

/* One output's top-left in a layout request */
typedef struct {
    uint32_t output;
    int32_t  x, y;
} stlxwin_layout_entry;

/* What the display manager did for a layout request, the first refusal
 * that applies. A generation numbers each change of the outputs. */
enum {
    STLXWIN_LAYOUT_APPLIED = 1,  /* applied and stored */
    STLXWIN_LAYOUT_UNSAVED = 2,  /* applied but not stored, so kept for its monitors until the display manager exits */
    STLXWIN_LAYOUT_STALE   = 3,  /* the generation is not the current one: nothing changed */
    STLXWIN_LAYOUT_INVALID = 4,  /* entries not exactly the listed outputs with one as main, or a corner past 1 << 20 */
    STLXWIN_LAYOUT_OVERLAP = 5,  /* two places overlap */
    STLXWIN_LAYOUT_APART   = 6,  /* an output does not connect to the main output */
};

/* Raised when a change to the GPU calls, stlxwin_window_configured or their types breaks the mesa kit's library */
#define STLXWIN_GPU_ABI_VERSION 1u

enum {
    STLXWIN_GPU_ACCEPTS   = 1u << 0,  /* the display manager takes buffers from /dev/gpuN, N being the device field */
    STLXWIN_GPU_CPU_READS = 1u << 1,  /* buffers must be mappable, and system memory reads fastest */
    STLXWIN_GPU_GPU_READS = 1u << 2,  /* the GPU reads buffers, mappable or not, and video memory reads fastest */
};

/* Which GPU buffers the display manager takes */
typedef struct {
    uint32_t flags;   /* STLXWIN_GPU_* */
    uint32_t device;  /* N of /dev/gpuN, 0 without STLXWIN_GPU_ACCEPTS */
} stlxwin_gpu_info;

#define STLXWIN_WINDOWS_MAX       32u     /* windows and popups a connection holds at once */
#define STLXWIN_GPU_BUFFERS_MAX   16u     /* GPU buffers a connection holds, a detached one until released */
#define STLXWIN_GPU_DIMENSION_MAX 16384u  /* largest width or height of a GPU buffer's picture */

enum {
    STLXWIN_FMT_XRGB8888 = 1,  /* 32-bit pixels, the top byte unused */
};

/* A GPU buffer's picture and the handles the display manager uses to
 * show it. The caller keeps its handles, the library passes copies. */
typedef struct {
    int      buffer;         /* buffer handle, GPU_RIGHT_READ at least */
    int      acquire;        /* timeline handle the client's rendering signals, GPU_RIGHT_READ at least */
    int      release;        /* timeline handle the display manager signals, GPU_RIGHT_READ and GPU_RIGHT_WRITE */
    uint32_t width, height;  /* the picture's pixels, each 1 to STLXWIN_GPU_DIMENSION_MAX */
    uint32_t pitch;          /* bytes from row to row, a multiple of 4, at least width * 4, times height under 4 GiB */
    uint32_t format;         /* STLXWIN_FMT_XRGB8888 */
    uint64_t offset;         /* bytes from the buffer's start to the picture, a multiple of 4 */
} stlxwin_gpu_buffer_desc;

/* Key events carry the raw HID usage and the translated UTF-32
 * codepoint (0 when the key produces no character). Coordinates are
 * window content local. */
typedef struct {
    stlxwin_event_type type;
    stlxwin_window*    window;   /* NULL for DISCONNECTED, MODES, MODE_RESULT, GPU_INFO, OUTPUTS and LAYOUT_RESULT */
    union {
        struct { uint16_t usage; uint8_t modifiers; uint32_t ch; } key;
        struct { int32_t x, y; } motion;
        struct { int32_t x, y; uint8_t button; } button;
        struct { int32_t x, y; int16_t dy; } scroll;
        struct { uint32_t width, height; } configure;
        struct { uint64_t time_ns; } frame;
        struct { uint32_t output; } modes;
        struct {
            uint32_t     output;
            int32_t      result;   /* 0, or the negative errno the display gave the last mode asked for */
            uint32_t     outcome;  /* STLXWIN_MODE_* */
            stlxwin_mode mode;     /* the mode the output is lit at when answered, zero while it is dark */
        } mode_result;
        struct {
            uint32_t outcome;      /* STLXWIN_LAYOUT_* */
            uint32_t generation;   /* the newest OUTPUTS' when answered */
        } layout_result;
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
 * @brief Destroy every window on the connection and disconnect, first
 * raising each GPU buffer's release timeline to its last committed
 * point.
 */
void stlxwin_disconnect(stlxwin_conn* conn);

/**
 * @brief The connection's fd, readable when events are pending.
 * Poll it and call stlxwin_dispatch when it turns readable.
 */
int stlxwin_conn_fd(const stlxwin_conn* conn);

/**
 * @brief The main output's size as the newest OUTPUTS gives it, or the
 * screen size reported on connect by a display manager that sends no
 * OUTPUTS, so toolkits can size and place windows before any configure
 * arrives.
 */
void stlxwin_screen_size(const stlxwin_conn* conn, uint32_t* w, uint32_t* h);

/**
 * @brief Copy the generation and up to capacity of the outputs, in
 * ascending index, as the connect handshake read them or the newest
 * STLXWIN_EVT_OUTPUTS changed them.
 * @return The number of outputs listed, which may exceed capacity, or 0
 * with *generation 0 from a display manager that sends no OUTPUTS.
 */
long stlxwin_outputs_get(stlxwin_conn* conn, uint32_t* generation,
                         stlxwin_output* outputs, size_t capacity);

/**
 * @brief Ask the display manager to place each output at its entry's
 * top-left, with main as the main output, each keeping its mode's size.
 * The entries must name each output stlxwin_outputs_get lists once, and
 * generation must be the one it gave with them. The request is answered
 * with one STLXWIN_EVT_LAYOUT_RESULT.
 * @return 0, or -1 for a count above STLXWIN_OUTPUTS_MAX or when the
 * message cannot be sent.
 */
int stlxwin_layout_set(stlxwin_conn* conn, uint32_t generation, uint32_t main,
                       const stlxwin_layout_entry* entries, uint32_t count);

/**
 * @brief Which GPU buffers the display manager takes, as the connect
 * handshake read it or the newest STLXWIN_EVT_GPU_INFO changed it.
 * Flags are 0 from a display manager that sends no GPU_INFO.
 */
stlxwin_gpu_info stlxwin_gpu_info_get(stlxwin_conn* conn);

/**
 * @brief Drain protocol messages into the event queue, never waiting
 * on the socket. Whichever call found the connection dead, the next
 * dispatch queues STLXWIN_EVT_DISCONNECTED once.
 * @return Number of events queued, or -1 on any call after the one
 * that queued the disconnect.
 */
int stlxwin_dispatch(stlxwin_conn* conn);

/**
 * @brief Block until an event is queued or the timeout expires.
 * @param timeout_ns Nanoseconds, negative waits forever, 0 polls.
 * @return 1 with events queued, 0 on timeout, -1 once the connection
 * is dead and no event is left.
 */
int stlxwin_wait(stlxwin_conn* conn, int64_t timeout_ns);

/**
 * @brief Pop the next queued event. Never blocks.
 * @return 1 and fills *out, or 0 if the queue is empty.
 */
int stlxwin_next_event(stlxwin_conn* conn, stlxwin_event* out);

/**
 * @brief Create a toplevel window. It becomes visible on first commit.
 * @return Window, or NULL when it cannot be made, as when the connection
 * holds STLXWIN_WINDOWS_MAX windows and popups.
 */
stlxwin_window* stlxwin_window_create(stlxwin_conn* conn,
                                      uint32_t width, uint32_t height,
                                      const char* title, uint32_t flags);

/**
 * @brief Create a popup at (x, y) in parent content coordinates.
 * Popups stack above their parent, are undecorated, and die with it.
 * With STLXWIN_PF_GRAB the popup takes keyboard input and outside
 * input dismisses it, delivering POPUP_DISMISSED to the parent.
 * @return Popup, or NULL when it cannot be made, as for a dead parent or
 * when the connection holds STLXWIN_WINDOWS_MAX windows and popups.
 */
stlxwin_window* stlxwin_popup_create(stlxwin_window* parent,
                                     int32_t x, int32_t y,
                                     uint32_t width, uint32_t height,
                                     uint32_t flags);

/**
 * @brief Destroy a window and, for a parent, its popups, raising each
 * of their GPU buffers' release timelines to its last committed point.
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
 * @brief A toplevel's output and its content's top-left in desktop
 * coordinates, as of the newest STLXWIN_EVT_WINDOW_OUTPUT for it. The
 * display manager places windows itself and first reports a window once
 * its first commit is shown. Each of output, x and y may be NULL.
 * @return 0, or -1 for a popup and before any such event arrived.
 */
int stlxwin_window_get_output(const stlxwin_window* win, uint32_t* output,
                              int32_t* x, int32_t* y);

/**
 * @brief The size and serial of the newest configure, or the created
 * size and serial 0 before any, for a renderer on another thread.
 * Each of width, height and serial may be NULL.
 */
void stlxwin_window_configured(stlxwin_window* win, uint32_t* width,
                               uint32_t* height, uint32_t* serial);

/**
 * @brief Acquire a buffer for the next frame, sized to the current
 * configure. Blocks dispatching until one is free, which paces a
 * render loop to the display manager. Events arriving meanwhile are
 * queued, not lost.
 * @return Buffer, or NULL if the window or connection is dead.
 */
stlxwin_buffer* stlxwin_begin_frame(stlxwin_window* win);

/**
 * @brief begin_frame that never waits for a free buffer.
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
 * @brief Attach a GPU buffer to a window. The display manager gets
 * copies of the handles with only the rights it needs, and disconnects
 * for a picture past the buffer's end.
 * @return The buffer's id, or 0 when the display manager takes no GPU
 * buffers, a field breaks its rule, the connection holds
 * STLXWIN_GPU_BUFFERS_MAX, /dev/gpuN does not open, a handle cannot be
 * narrowed to the rights its field names, or the connection is dead.
 */
uint32_t stlxwin_gpu_attach(stlxwin_window* win,
                            const stlxwin_gpu_buffer_desc* desc);

/**
 * @brief Commit a GPU buffer's picture, acking the newest configure of
 * its size, else the window's last ack. The display manager shows the
 * picture only once the acquire timeline reaches acquire_point, raises
 * the release timeline to latch_point, when not 0, once the picture is
 * no longer pending, and to release_point once it no longer reads the
 * buffer, which must not be committed again before then.
 * @return 0, or -1 for an unknown or detached buffer, a dead
 * connection, or points that do not rise from the last release point
 * through latch_point, when not 0, to release_point.
 */
int stlxwin_gpu_commit(stlxwin_window* win, uint32_t buf_id,
                       uint64_t acquire_point, uint64_t latch_point,
                       uint64_t release_point, const stlxwin_rect* damage,
                       uint32_t n_damage, uint32_t flags);

/**
 * @brief Detach a GPU buffer. The display manager may show it until
 * its last release point is reached, and it counts toward
 * STLXWIN_GPU_BUFFERS_MAX until then.
 * @return 0, or -1 for an unknown or detached buffer or a dead
 * connection.
 */
int stlxwin_gpu_detach(stlxwin_window* win, uint32_t buf_id);

/**
 * @brief Find, without reading the socket, a display manager that is
 * gone. The library reaches every release point it holds once a call
 * finds the connection dead, so a renderer waiting on one must wait in
 * slices of at most 100 ms and call this between them.
 * @return 0, or -1 once the connection is dead.
 */
int stlxwin_gpu_check(stlxwin_conn* conn);

/**
 * @brief Replace the clipboard with len bytes of UTF-8 text, of any
 * size memory holds.
 * @return 0, or -1 when no shared memory object can hold the text or
 * the message cannot be sent, leaving the clipboard as it was.
 */
int stlxwin_clipboard_set(stlxwin_conn* conn, const char* text, size_t len);

/**
 * @brief Fetch the clipboard into a malloc'd buffer the caller frees.
 * @return Length, 0 if empty, -1 on failure.
 */
long stlxwin_clipboard_get(stlxwin_conn* conn, char** out_text);

/**
 * @brief Ask for an output's modes. The connection keeps the latest
 * modes the display manager sends, at once and after each change of
 * the output, and queues STLXWIN_EVT_MODES for each.
 * @return 0, or -1 for an output of STLXWIN_OUTPUTS_MAX or more, when
 * no memory is left to keep the modes, or when the message cannot be
 * sent.
 */
int stlxwin_modes_watch(stlxwin_conn* conn, uint32_t output);

/**
 * @brief Copy a watched output's latest state into info and up to
 * capacity of its modes, the default first, into modes.
 * @return The output's mode count, 0 while it is dark or past the
 * output count, or -1 before its first STLXWIN_EVT_MODES.
 */
long stlxwin_modes_get(stlxwin_conn* conn, uint32_t output,
                       stlxwin_modes_info* info, stlxwin_mode* modes,
                       size_t capacity);

/**
 * @brief Ask the display manager to light an output at one of its
 * listed modes. It answers with one STLXWIN_EVT_MODE_RESULT. For a
 * trial (STLXWIN_SET_MODE_TRIAL) it answers again when the timer sets
 * the previous mode back, but not when the output goes dark or
 * changes mode first.
 * @return 0, or -1 when the message cannot be sent.
 */
int stlxwin_mode_set(stlxwin_conn* conn, uint32_t output,
                     const stlxwin_mode* mode, uint32_t flags);

/**
 * @brief End this connection's trial on an output: keep 1 keeps its
 * mode, 0 sets the previous one back now, and any other keep is
 * answered STLXWIN_MODE_INVALID. The display manager answers with one
 * STLXWIN_EVT_MODE_RESULT.
 * @return 0, or -1 when the message cannot be sent.
 */
int stlxwin_mode_keep(stlxwin_conn* conn, uint32_t output, int keep);

#ifdef __cplusplus
}
#endif

#endif /* STLXWIN_STLXWIN_H */
