#ifndef STLXWIN_PROTO_H
#define STLXWIN_PROTO_H

/*
 * The Stellux window protocol wire format, shared by the client
 * library and the display manager.
 *
 * One unix stream socket per client carries every message in both
 * directions. Pictures stay outside it, in shared memory objects the
 * client creates and names in ATTACH_BUFFER, or in GPU buffers whose
 * handles ATTACH_GPU_BUFFER sends over the socket, which the DM takes
 * when the GPU_INFO after HELLO_REPLY says so. Each message is an
 * swp_header followed by header.length payload bytes. All structs are
 * little endian, naturally aligned, padding free, and size asserted so
 * a field edit fails the build instead of corrupting the wire.
 */

#include <stdint.h>

#define SWP_VERSION          1u
#define SWP_SOCKET_PATH      "/tmp/stlxdm.sock"
#define SWP_APP_ID_MAX       64u
#define SWP_TITLE_MAX        128u
#define SWP_SHM_NAME_MAX     80u
#define SWP_COMMIT_MAX_RECTS 8u
#define SWP_EVENTS_MAX_BATCH 64u
#define SWP_CLIPBOARD_MAX    4096u
#define SWP_MODES_MAX        32u  /* the most modes an output lists */
#define SWP_OUTPUTS_MAX      16u  /* the most outputs a display drives */
#define SWP_DESKTOP_LIMIT    (1 << 20)  /* every corner of every place lies within this of the origin on both axes */

#define SWP_SHM_BUFFERS_MAX   64u     /* shared memory buffers one connection holds at once */
#define SWP_GPU_HANDLES       3u      /* handles one ATTACH_GPU_BUFFER carries */
#define SWP_GPU_BUFFERS_MAX   16u     /* GPU buffers one connection holds at once, shown detached ones included */
#define SWP_GPU_DIMENSION_MAX 16384u  /* largest width or height of a GPU buffer's picture */

/* Client to DM types are 0x01xx, DM to client are 0x02xx */
enum {
    SWP_MSG_HELLO          = 0x0101,
    SWP_MSG_CREATE_WINDOW  = 0x0102,
    SWP_MSG_DESTROY_WINDOW = 0x0103,
    SWP_MSG_SET_WINDOW     = 0x0104,
    SWP_MSG_ATTACH_BUFFER  = 0x0105,
    SWP_MSG_DETACH_BUFFER  = 0x0106,
    SWP_MSG_COMMIT         = 0x0107,
    SWP_MSG_CLIPBOARD_SET  = 0x0108,
    SWP_MSG_CLIPBOARD_GET  = 0x0109,
    SWP_MSG_CAPTURE        = 0x010A,
    SWP_MSG_GET_MODES      = 0x010B,
    SWP_MSG_SET_MODE       = 0x010C,
    SWP_MSG_KEEP_MODE      = 0x010D,

    SWP_MSG_ATTACH_GPU_BUFFER = 0x010E,
    SWP_MSG_COMMIT_GPU        = 0x010F,

    SWP_MSG_SET_LAYOUT     = 0x0110,

    SWP_MSG_HELLO_REPLY    = 0x0201,
    SWP_MSG_CONFIGURE      = 0x0202,
    SWP_MSG_RELEASE        = 0x0203,
    SWP_MSG_FRAME_DONE     = 0x0204,
    SWP_MSG_EVENT          = 0x0205,
    SWP_MSG_CLIPBOARD_DATA = 0x0206,
    SWP_MSG_MODES          = 0x0207,
    SWP_MSG_MODE_RESULT    = 0x0208,
    SWP_MSG_GPU_INFO       = 0x0209,
    SWP_MSG_OUTPUTS        = 0x020A,
    SWP_MSG_WINDOW_OUTPUT  = 0x020B,
    SWP_MSG_LAYOUT_RESULT  = 0x020C,
};

typedef struct {
    uint16_t type;      /* SWP_MSG_* */
    uint16_t flags;     /* per-message flag bits, zero unless specified */
    uint32_t length;    /* payload bytes following this header */
} swp_header;
_Static_assert(sizeof(swp_header) == 8, "wire layout");

typedef struct {
    int32_t x, y;
    int32_t w, h;
} swp_rect;
_Static_assert(sizeof(swp_rect) == 16, "wire layout");

/* A picture size and refresh rate an output can light at */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint64_t refresh_millihertz; /* 0 when unknown */
} swp_mode;
_Static_assert(sizeof(swp_mode) == 16, "wire layout");

/* ---- client to DM ---- */

typedef struct {
    uint32_t proto_version;
    char     app_id[SWP_APP_ID_MAX];
} swp_hello;
_Static_assert(sizeof(swp_hello) == 68, "wire layout");

/* SWP_WF_GLASS lays the DM's glass material under a borderless window, as the DM does under every decorated window's
 * frame, and premultiplied pixels show it through their alpha. The DM disconnects a client asking for premultiplied
 * pixels without SWP_WF_GLASS, even on a decorated window. */
enum {
    SWP_WF_RESIZABLE     = 1u << 0,
    SWP_WF_BORDERLESS    = 1u << 1,
    SWP_WF_GLASS         = 1u << 2,
    SWP_WF_PREMULTIPLIED = 1u << 3,  /* shared memory buffers hold premultiplied ARGB8888, not XRGB8888 */
};

enum {
    SWP_PF_GRAB    = 1u << 0,  /* outside input dismisses, keyboard follows */
    SWP_PF_ROUNDED = 1u << 1,  /* glass rounded by STLXTHEME_MENU_RADIUS to match a panel the client draws */
};

typedef struct {
    uint32_t win_id;        /* client chosen, unique per connection */
    uint32_t width, height; /* content pixels */
    uint32_t flags;         /* SWP_WF_* */
    uint32_t parent_id;     /* 0 for a toplevel, else the popup parent */
    int32_t  rel_x, rel_y;  /* popup position in parent content space */
    uint32_t popup_flags;   /* SWP_PF_* */
    char     title[SWP_TITLE_MAX];
} swp_create_window;
_Static_assert(sizeof(swp_create_window) == 160, "wire layout");

typedef struct {
    uint32_t win_id;
} swp_destroy_window;
_Static_assert(sizeof(swp_destroy_window) == 4, "wire layout");

enum {
    SWP_FIELD_TITLE      = 1,  /* title[] carries the value */
    SWP_FIELD_MIN_SIZE   = 2,  /* a is width, b is height */
    SWP_FIELD_MAX_SIZE   = 3,  /* a is width, b is height */
    SWP_FIELD_FULLSCREEN = 4,  /* a is 0 or 1 */
    SWP_FIELD_CURSOR     = 5,  /* a is an swp_cursor value */
};

typedef enum {
    SWP_CURSOR_ARROW = 0,
    SWP_CURSOR_IBEAM,
    SWP_CURSOR_HAND,
    SWP_CURSOR_RESIZE_H,
    SWP_CURSOR_RESIZE_V,
    SWP_CURSOR_NONE,
    SWP_CURSOR_RESIZE_NWSE,
    SWP_CURSOR_RESIZE_NESW,
} swp_cursor;

/* One field per message. These are rare and tiny, so a fixed layout
 * beats a variable one. */
typedef struct {
    uint32_t win_id;
    uint32_t field;         /* SWP_FIELD_* */
    uint32_t a, b;
    char     title[SWP_TITLE_MAX];
} swp_set_window;
_Static_assert(sizeof(swp_set_window) == 144, "wire layout");

typedef struct {
    uint32_t win_id;
    uint32_t buf_id;        /* client chosen, unique per window */
    uint32_t width, height; /* stride is always width * 4 */
    char     shm_name[SWP_SHM_NAME_MAX];
} swp_attach_buffer;
_Static_assert(sizeof(swp_attach_buffer) == 96, "wire layout");

/* A GPU buffer detached while shown stays the window's picture, which
 * the CPU compositor shows as the content color when the buffer is not
 * mappable, and keeps its buf_id, counting toward SWP_GPU_BUFFERS_MAX,
 * until the next picture latches */
typedef struct {
    uint32_t win_id;
    uint32_t buf_id;
} swp_detach_buffer;
_Static_assert(sizeof(swp_detach_buffer) == 8, "wire layout");

/* The atomic frame submission: content, damage, and resize ack in one
 * message. Zero damage rects means the whole buffer changed. */
typedef struct {
    uint32_t win_id;
    uint32_t buf_id;
    uint32_t ack_serial;    /* latest CONFIGURE serial seen, else 0 */
    uint32_t commit_flags;  /* reserved, must be 0 */
    uint32_t n_damage;      /* 0 to SWP_COMMIT_MAX_RECTS */
    uint32_t reserved;      /* keeps damage[] aligned, must be 0 */
    swp_rect damage[SWP_COMMIT_MAX_RECTS];
} swp_commit;
_Static_assert(sizeof(swp_commit) == 152, "wire layout");

typedef struct {
    uint32_t len;           /* followed by len UTF-8 bytes */
} swp_clipboard_set;
_Static_assert(sizeof(swp_clipboard_set) == 4, "wire layout");

typedef struct {
    uint32_t buf_id;        /* attached buffer the screen is blitted into */
} swp_capture;
_Static_assert(sizeof(swp_capture) == 4, "wire layout");

/* Answered by the output's MODES, and for an output below output_count
 * by MODES again after each change until the client disconnects */
typedef struct {
    uint32_t output;        /* the display's index of the output */
} swp_get_modes;
_Static_assert(sizeof(swp_get_modes) == 4, "wire layout");

enum {
    SWP_SET_MODE_TRIAL = 1u << 0,  /* set back after 15 seconds unless kept */
};

/* SET_MODE and KEEP_MODE are each answered by one MODE_RESULT. The DM
 * asks the display for the mode on behalf of any client. */
typedef struct {
    uint32_t output;
    uint32_t flags;         /* SWP_SET_MODE_* */
    swp_mode mode;          /* one the output's MODES lists */
} swp_set_mode;
_Static_assert(sizeof(swp_set_mode) == 24, "wire layout");

typedef struct {
    uint32_t output;
    uint32_t keep;          /* 1 keeps the trial's mode, 0 sets the previous one back now */
} swp_keep_mode;
_Static_assert(sizeof(swp_keep_mode) == 8, "wire layout");

/* Attaches a GPU buffer. The message must go in one sendmsg whose
 * SCM_RIGHTS data holds exactly SWP_GPU_HANDLES handles: the buffer and
 * its acquire timeline with read rights alone, then its release
 * timeline with write rights alone. */
typedef struct {
    uint32_t win_id;
    uint32_t buf_id;        /* unique among the window's buffers of both kinds */
    uint32_t width, height; /* the picture's pixels, each 1 to SWP_GPU_DIMENSION_MAX */
    uint32_t pitch;         /* bytes from row to row, a multiple of 4, at least width * 4, times height under 4 GiB */
    uint32_t format;        /* SWP_FMT_XRGB8888 */
    uint64_t offset;        /* bytes from the buffer's start to the picture, a multiple of 4 */
} swp_attach_gpu_buffer;
_Static_assert(sizeof(swp_attach_gpu_buffer) == 32, "wire layout");

/* A COMMIT of a GPU buffer's picture. The DM latches it only once the
 * acquire timeline reaches acquire_point, releasing it unshown if the
 * point comes with GPU_TIMELINE_FAILED, and signals the release
 * timeline instead of sending RELEASE. A buffer's release points must
 * rise, a nonzero latch_point must lie between the buffer's last
 * release point and release_point, and it must not be committed again
 * while shown or pending. */
typedef struct {
    uint32_t win_id;
    uint32_t buf_id;
    uint32_t ack_serial;    /* the newest CONFIGURE of the picture's size, else the window's last ack */
    uint32_t commit_flags;  /* reserved, must be 0 */
    uint32_t n_damage;      /* 0 to SWP_COMMIT_MAX_RECTS */
    uint32_t reserved;      /* keeps the points aligned, must be 0 */
    uint64_t acquire_point; /* reached once the picture is drawn */
    uint64_t latch_point;   /* 0, or reached by the DM on the release timeline once the picture is no longer pending */
    uint64_t release_point; /* reached by the DM once it no longer reads the picture */
    swp_rect damage[SWP_COMMIT_MAX_RECTS];
} swp_commit_gpu;
_Static_assert(sizeof(swp_commit_gpu) == 176, "wire layout");

typedef struct {
    uint32_t output;
    int32_t  x, y;          /* the place's top-left */
} swp_layout_entry;
_Static_assert(sizeof(swp_layout_entry) == 12, "wire layout");

/* Places each output OUTPUTS lists at a top-left, at its mode's size,
 * and names the main output. Answered by one LAYOUT_RESULT. */
typedef struct {
    uint32_t         generation;               /* the generation of the OUTPUTS this was built from */
    uint32_t         main;                     /* the main output's index */
    uint32_t         count;                    /* entries, one per output OUTPUTS lists */
    swp_layout_entry entries[SWP_OUTPUTS_MAX]; /* `count` used */
} swp_set_layout;
_Static_assert(sizeof(swp_set_layout) == 204, "wire layout");

/* ---- DM to client ---- */

enum {
    SWP_FMT_XRGB8888 = 1,   /* the only format value, SWP_WF_PREMULTIPLIED marks premultiplied shared memory buffers */
};

/* HELLO_REPLY's header flags */
enum {
    SWP_HELLO_GPU_INFO = 1u << 0,  /* a GPU_INFO follows the reply */
    SWP_HELLO_OUTPUTS  = 1u << 1,  /* an OUTPUTS follows the reply, after any GPU_INFO */
};

typedef struct {
    uint32_t proto_version;
    uint32_t format;        /* SWP_FMT_* */
    uint32_t screen_w, screen_h;
} swp_hello_reply;
_Static_assert(sizeof(swp_hello_reply) == 16, "wire layout");

enum {
    SWP_STATE_FOCUSED    = 1u << 0,
    SWP_STATE_FULLSCREEN = 1u << 1,
};

/* A size or state change. Obliges the client to answer with a commit
 * acking this serial, even if it had nothing to draw. */
typedef struct {
    uint32_t win_id;
    uint32_t width, height;
    uint32_t serial;
    uint32_t state;         /* SWP_STATE_* */
} swp_configure;
_Static_assert(sizeof(swp_configure) == 20, "wire layout");

/* The buffer is no longer read by the DM and may be reused or freed */
typedef struct {
    uint32_t win_id;
    uint32_t buf_id;
} swp_release;
_Static_assert(sizeof(swp_release) == 8, "wire layout");

/* Sent once per presentation of the window on its toplevel's output,
 * the one holding the largest part of the toplevel and its decorations,
 * or on another while the DM cannot draw there. Never sent per commit,
 * so a superseded commit produces no frame event. */
typedef struct {
    uint32_t win_id;
    uint32_t reserved;      /* keeps time_ns aligned, must be 0 */
    uint64_t time_ns;       /* CLOCK_MONOTONIC presentation time */
} swp_frame_done;
_Static_assert(sizeof(swp_frame_done) == 16, "wire layout");

enum {
    SWP_EV_KEY_DOWN = 1,
    SWP_EV_KEY_UP,
    SWP_EV_KEY_REPEAT,
    SWP_EV_MOTION,
    SWP_EV_BUTTON_DOWN,
    SWP_EV_BUTTON_UP,
    SWP_EV_SCROLL,
    SWP_EV_ENTER,
    SWP_EV_LEAVE,
    SWP_EV_FOCUS_IN,
    SWP_EV_FOCUS_OUT,
    SWP_EV_CLOSE,
    SWP_EV_POPUP_DISMISSED,
};

/* One fixed shape for every event kind keeps parsing a loop. Key
 * events carry the DM-translated UTF-32 codepoint alongside the raw
 * HID usage, so clients never own keymap tables. */
typedef struct {
    uint16_t kind;          /* SWP_EV_* */
    uint8_t  modifiers;     /* key records */
    uint8_t  button;        /* button records */
    uint16_t usage;         /* key records, HID usage */
    int16_t  scroll;        /* scroll records, signed detents */
    uint32_t ch;            /* key records, UTF-32 codepoint or 0 */
    int32_t  x, y;          /* pointer records, window content local */
} swp_event_rec;
_Static_assert(sizeof(swp_event_rec) == 20, "wire layout");

/* EVENT payload: this prefix, then count swp_event_rec records */
typedef struct {
    uint32_t win_id;
    uint32_t count;
} swp_event_prefix;
_Static_assert(sizeof(swp_event_prefix) == 8, "wire layout");

typedef struct {
    uint32_t len;           /* followed by len UTF-8 bytes */
} swp_clipboard_data;
_Static_assert(sizeof(swp_clipboard_data) == 4, "wire layout");

enum {
    SWP_OUTPUT_LIT     = 1u << 0,  /* the output shows a picture */
    SWP_OUTPUT_PRIMARY = 1u << 1,  /* gfxfb's primary output */
};

/* An output's state and modes, the default first. An output of
 * output_count or more gets output_count alone. */
typedef struct {
    uint32_t output;
    uint32_t output_count;
    uint32_t flags;         /* SWP_OUTPUT_* */
    uint32_t count;         /* entries of modes in use, 0 while the output is dark */
    swp_mode current;       /* zero while the output is dark */
    swp_mode modes[SWP_MODES_MAX];
} swp_modes;
_Static_assert(sizeof(swp_modes) == 544, "wire layout");

/* What the DM did. A trial's timer answers SET_BACK or DEFAULT
 * unasked, and a trial that an output event ends gets no answer. */
enum {
    SWP_MODE_SET        = 1,  /* asked without a trial */
    SWP_MODE_TRIAL      = 2,  /* asked as a trial, its timer running when the result is 0 */
    SWP_MODE_KEPT       = 3,  /* the trial's mode kept, nothing asked */
    SWP_MODE_SET_BACK   = 4,  /* the trial's previous mode asked for again */
    SWP_MODE_DEFAULT    = 5,  /* a set-back failed, so the output's default was asked for */
    SWP_MODE_NO_BUFFERS = 6,  /* no DM buffers at the new size, so the previous mode was asked for */
    SWP_MODE_BUSY       = 7,  /* refused, nothing asked: a trial is running */
    SWP_MODE_NO_TRIAL   = 8,  /* refused, nothing asked: no trial of this client's on the output */
    SWP_MODE_INVALID    = 9,  /* refused, nothing asked: an unknown flag or keep value */
};

typedef struct {
    uint32_t output;
    int32_t  result;        /* GFXFB_SET_MODE's last result, 0 or a negative errno, 0 when nothing was asked */
    uint32_t outcome;       /* SWP_MODE_* */
    uint32_t reserved;      /* zero */
    swp_mode mode;          /* the mode the output is lit at, zero while it is dark */
} swp_mode_result;
_Static_assert(sizeof(swp_mode_result) == 32, "wire layout");

enum {
    SWP_GPU_ACCEPTS   = 1u << 0,  /* the DM takes GPU buffers from /dev/gpuN, N being the device field */
    SWP_GPU_CPU_READS = 1u << 1,  /* buffers must be mappable, and system memory reads fastest */
    SWP_GPU_GPU_READS = 1u << 2,  /* the GPU reads buffers, mappable or not, and video memory reads fastest */
};

/* Sent after every HELLO_REPLY and again whenever it changes. With
 * SWP_GPU_ACCEPTS exactly one read flag is set. A reader must accept a
 * longer payload and use its first sizeof(swp_gpu_info) bytes, so fields
 * can be appended. */
typedef struct {
    uint32_t flags;         /* SWP_GPU_* */
    uint32_t device;        /* N of /dev/gpuN, 0 without SWP_GPU_ACCEPTS */
} swp_gpu_info;
_Static_assert(sizeof(swp_gpu_info) == 8, "wire layout");

enum {
    SWP_DESKTOP_MAIN = 1u << 0,  /* the main output, carrying the bar and the dock, its top-left at (0, 0) */
};

/* An output's place, in desktop coordinates, which count monitor pixels
 * right and down from the main output's top-left */
typedef struct {
    uint32_t output;             /* gfxfb's output index */
    uint32_t flags;              /* SWP_DESKTOP_* */
    int32_t  x, y;               /* the place's top-left */
    uint32_t width, height;      /* the mode's size, the last one while the output relights */
    uint64_t refresh_millihertz; /* the mode's refresh, 0 when unknown */
    swp_rect work;               /* the work area: the main output's place less the bar and the dock, else the place */
    char     monitor[32];        /* "maker name", NUL-terminated, empty when the output reports no identity */
} swp_desktop_output;
_Static_assert(sizeof(swp_desktop_output) == 80, "wire layout");

/* The desktop's layout, sent after HELLO_REPLY and at the end of each
 * DM wakeup to every client holding an older generation */
typedef struct {
    uint32_t           generation;               /* raised by one at every change of what OUTPUTS carries, from 1 */
    uint32_t           count;                    /* members, the outputs holding a place, at least 1 */
    swp_desktop_output outputs[SWP_OUTPUTS_MAX]; /* ascending output index, `count` used, the rest zero */
} swp_outputs;
_Static_assert(sizeof(swp_outputs) == 1288, "wire layout");

/* A toplevel's output and position, sent after its first FRAME_DONE and
 * at the end of each DM wakeup that finds either changed since the last
 * one sent, after any OUTPUTS. During a drag or resize of the window, a
 * change of position alone is sent only once the drag or resize ends.
 * Never sent for popups. */
typedef struct {
    uint32_t win_id;
    uint32_t output;        /* the member holding the largest part of the window and its decorations */
    int32_t  x, y;          /* the content's top-left in desktop coordinates, inside the decorations */
} swp_window_output;
_Static_assert(sizeof(swp_window_output) == 16, "wire layout");

/* What the DM did with a SET_LAYOUT: a refused request changes nothing
 * and gets the first refusal below that applies */
enum {
    SWP_LAYOUT_APPLIED = 1,  /* applied and stored */
    SWP_LAYOUT_UNSAVED = 2,  /* applied but not stored, so kept for its monitors until the DM exits */
    SWP_LAYOUT_STALE   = 3,  /* the generation is not the current one: nothing changed */
    SWP_LAYOUT_INVALID = 4,  /* entries not exactly the members with one as main, or a corner past the limit */
    SWP_LAYOUT_OVERLAP = 5,  /* two places overlap */
    SWP_LAYOUT_APART   = 6,  /* an output does not connect to the main output */
};

typedef struct {
    uint32_t outcome;       /* SWP_LAYOUT_* */
    uint32_t generation;    /* the current one as it is sent: the last OUTPUTS' */
} swp_layout_result;
_Static_assert(sizeof(swp_layout_result) == 8, "wire layout");

/* Read buffer bound for either side of the socket */
#define SWP_MAX_MSG_SIZE \
    (sizeof(swp_header) + sizeof(swp_clipboard_set) + SWP_CLIPBOARD_MAX)

#endif /* STLXWIN_PROTO_H */
