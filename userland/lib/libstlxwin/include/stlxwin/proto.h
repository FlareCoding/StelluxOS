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
 * when the GPU_INFO after HELLO_REPLY says so. The clipboard's content
 * stays outside it too, in shared memory objects whose handles
 * CLIPBOARD_SET, CLIPBOARD_OFFER and CLIPBOARD_DATA send, and so does
 * a drag's, whose handles START_DRAG and DROP_DATA send. Each
 * message is an swp_header followed by header.length payload bytes.
 * All structs are little endian, naturally aligned, padding free, and
 * size asserted so a field edit fails the build instead of corrupting
 * the wire.
 */

#include <stdint.h>

/* C++ spells the assertion static_assert, the only spelling GCC's C++ accepts, so the header builds as either */
#ifdef __cplusplus
#define SWP_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define SWP_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

#define SWP_VERSION          1u
#define SWP_SOCKET_PATH      "/tmp/stlxdm.sock"
#define SWP_APP_ID_MAX       64u
#define SWP_TITLE_MAX        128u
#define SWP_SHM_NAME_MAX     80u
#define SWP_COMMIT_MAX_RECTS 8u
#define SWP_EVENTS_MAX_BATCH 64u
#define SWP_MODES_MAX        32u  /* the most modes an output lists */
#define SWP_OUTPUTS_MAX      16u  /* the most outputs a display drives */
#define SWP_DESKTOP_LIMIT    (1 << 20)  /* every corner of every place lies within this of the origin on both axes */

#define SWP_SHM_BUFFERS_MAX   64u     /* shared memory buffers one connection holds at once */
#define SWP_GPU_HANDLES       3u      /* handles one ATTACH_GPU_BUFFER carries */
#define SWP_GPU_BUFFERS_MAX   16u     /* GPU buffers one connection holds at once, shown detached ones included */
#define SWP_GPU_DIMENSION_MAX 16384u  /* largest width or height of a GPU buffer's picture */
#define SWP_HANDLES_MAX       5u      /* the most handles one message carries, which both ends read */

#define SWP_MIME_MAX         64u   /* a MIME type's bytes with its NUL */
#define SWP_OFFER_TYPES_MAX  4u    /* representations one offer carries */
#define SWP_DRAG_PICTURE_MAX 512u  /* largest width or height of a drag's picture */
#define SWP_DROP_WORDS_MAX   64u   /* a drop target's words for the drag's chip with their NUL */

/* The type CLIPBOARD_SET and an untyped CLIPBOARD_GET carry */
#define SWP_MIME_TEXT "text/plain;charset=utf-8"

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

    SWP_MSG_ACTIVATE_WINDOW = 0x0111,

    SWP_MSG_CLIPBOARD_OFFER = 0x0112,
    SWP_MSG_CLIPBOARD_WATCH = 0x0113,

    SWP_MSG_START_DRAG  = 0x0114,
    SWP_MSG_CANCEL_DRAG = 0x0115,
    SWP_MSG_DROP_STATUS = 0x0116,
    SWP_MSG_DROP_GET    = 0x0117,
    SWP_MSG_DROP_FINISH = 0x0118,

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

    SWP_MSG_CLIPBOARD_TYPES = 0x020D,

    SWP_MSG_DRAG_STATE  = 0x020E,
    SWP_MSG_DROP_ENTER  = 0x020F,
    SWP_MSG_DROP_MOTION = 0x0210,
    SWP_MSG_DROP_LEAVE  = 0x0211,
    SWP_MSG_DROP        = 0x0212,
    SWP_MSG_DROP_DATA   = 0x0213,
};

typedef struct {
    uint16_t type;      /* SWP_MSG_* */
    uint16_t flags;     /* per-message flag bits, zero unless specified */
    uint32_t length;    /* payload bytes following this header */
} swp_header;
SWP_STATIC_ASSERT(sizeof(swp_header) == 8, "wire layout");

typedef struct {
    int32_t x, y;
    int32_t w, h;
} swp_rect;
SWP_STATIC_ASSERT(sizeof(swp_rect) == 16, "wire layout");

/* A picture size and refresh rate an output can light at */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint64_t refresh_millihertz; /* 0 when unknown */
} swp_mode;
SWP_STATIC_ASSERT(sizeof(swp_mode) == 16, "wire layout");

/* ---- client to DM ---- */

typedef struct {
    uint32_t proto_version;
    char     app_id[SWP_APP_ID_MAX];
} swp_hello;
SWP_STATIC_ASSERT(sizeof(swp_hello) == 68, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_create_window) == 160, "wire layout");

typedef struct {
    uint32_t win_id;
} swp_destroy_window;
SWP_STATIC_ASSERT(sizeof(swp_destroy_window) == 4, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_set_window) == 144, "wire layout");

typedef struct {
    uint32_t win_id;
    uint32_t buf_id;        /* client chosen, unique per window */
    uint32_t width, height; /* stride is always width * 4 */
    char     shm_name[SWP_SHM_NAME_MAX];
} swp_attach_buffer;
SWP_STATIC_ASSERT(sizeof(swp_attach_buffer) == 96, "wire layout");

/* A GPU buffer detached while shown stays the window's picture, which
 * the CPU compositor shows as the content color when the buffer is not
 * mappable, and keeps its buf_id, counting toward SWP_GPU_BUFFERS_MAX,
 * until the next picture latches */
typedef struct {
    uint32_t win_id;
    uint32_t buf_id;
} swp_detach_buffer;
SWP_STATIC_ASSERT(sizeof(swp_detach_buffer) == 8, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_commit) == 152, "wire layout");

/* Replaces the clipboard with an item of one SWP_MIME_TEXT representation. A nonzero len must go in one sendmsg whose
 * SCM_RIGHTS data holds exactly one handle, of a shared memory object holding the text from its start. */
typedef struct {
    uint64_t len;           /* UTF-8 bytes, 0 empties the clipboard and sends no handle */
} swp_clipboard_set;
SWP_STATIC_ASSERT(sizeof(swp_clipboard_set) == 8, "wire layout");

enum {
    SWP_CLIPBOARD_PRIVATE = 1u << 0,  /* the copy stays out of the clipboard's history, as a password must */
};

/* One representation: printable ASCII MIME type, NUL-padded, and its bytes from its object's start. Flags are 0,
 * reserved for representations a source renders later. */
typedef struct {
    char     mime[SWP_MIME_MAX];
    uint64_t len;
    uint32_t flags;
    uint32_t reserved;
} swp_offer_type;
SWP_STATIC_ASSERT(sizeof(swp_offer_type) == 80, "wire layout");

/* One item in up to SWP_OFFER_TYPES_MAX representations, as CLIPBOARD_OFFER carries it to replace the clipboard,
 * where a count of 0 empties it. Its handles follow in order, one per representation with a nonzero len. */
typedef struct {
    uint32_t       count;
    uint32_t       flags;  /* the clipboard's SWP_CLIPBOARD_* */
    swp_offer_type types[SWP_OFFER_TYPES_MAX];
} swp_offer;
SWP_STATIC_ASSERT(sizeof(swp_offer) == 328, "wire layout");

/* CLIPBOARD_GET's typed form, which asks for one representation. Without a payload it asks for SWP_MIME_TEXT. */
typedef struct {
    char mime[SWP_MIME_MAX];  /* NUL-terminated */
} swp_clipboard_get;
SWP_STATIC_ASSERT(sizeof(swp_clipboard_get) == 64, "wire layout");

typedef struct {
    uint32_t buf_id;        /* attached buffer the screen is blitted into */
} swp_capture;
SWP_STATIC_ASSERT(sizeof(swp_capture) == 4, "wire layout");

/* Answered by the output's MODES, and for an output below output_count
 * by MODES again after each change until the client disconnects */
typedef struct {
    uint32_t output;        /* the display's index of the output */
} swp_get_modes;
SWP_STATIC_ASSERT(sizeof(swp_get_modes) == 4, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_set_mode) == 24, "wire layout");

typedef struct {
    uint32_t output;
    uint32_t keep;          /* 1 keeps the trial's mode, 0 sets the previous one back now */
} swp_keep_mode;
SWP_STATIC_ASSERT(sizeof(swp_keep_mode) == 8, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_attach_gpu_buffer) == 32, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_commit_gpu) == 176, "wire layout");

typedef struct {
    uint32_t output;
    int32_t  x, y;          /* the place's top-left */
} swp_layout_entry;
SWP_STATIC_ASSERT(sizeof(swp_layout_entry) == 12, "wire layout");

/* Places each output OUTPUTS lists at a top-left, at its mode's size,
 * and names the main output. Answered by one LAYOUT_RESULT. */
typedef struct {
    uint32_t         generation;               /* the generation of the OUTPUTS this was built from */
    uint32_t         main;                     /* the main output's index */
    uint32_t         count;                    /* entries, one per output OUTPUTS lists */
    swp_layout_entry entries[SWP_OUTPUTS_MAX]; /* `count` used */
} swp_set_layout;
SWP_STATIC_ASSERT(sizeof(swp_set_layout) == 204, "wire layout");

/* Asks the DM to raise and focus a toplevel, or bring it back when minimized. The DM grants it only soon after the
 * user's last key or button press, with no overlay or grabbing popup holding the input, and sends no answer. */
typedef struct {
    uint32_t win_id;
} swp_activate_window;
SWP_STATIC_ASSERT(sizeof(swp_activate_window) == 4, "wire layout");

/* What a drop does with the data, which a drag's source allows and its target accepts */
enum {
    SWP_DND_COPY = 1u << 0,
    SWP_DND_MOVE = 1u << 1,
    SWP_DND_LINK = 1u << 2,
};

/* Starts a drag from the press `serial` names while it lasts on the window holding the grab, answered by DRAG_STATE.
 * One sendmsg carries it with the picture's handle first, when it has one, then the offer's as CLIPBOARD_OFFER's. */
typedef struct {
    uint32_t  win_id;
    uint32_t  serial;                /* the press's, from its BUTTON_DOWN, which names the drag from then on */
    uint32_t  actions;               /* SWP_DND_* the source allows, at least one */
    uint32_t  item_count;            /* the items dragged, at least 1 */

    /* The picture, premultiplied ARGB8888 in rows of picture_w * 4 bytes from its object's start */
    uint32_t  picture_w, picture_h;  /* each 1 to SWP_DRAG_PICTURE_MAX, or both 0 for no picture */
    int32_t   hot_x, hot_y;          /* the picture's pixel under the pointer */

    swp_offer offer;                 /* flags 0, and no representation for a drag only its own client takes */
} swp_start_drag;
SWP_STATIC_ASSERT(sizeof(swp_start_drag) == 360, "wire layout");

/* Ends the drag `drag` names as Esc does, when this client started it, and is ignored otherwise */
typedef struct {
    uint32_t drag;
} swp_cancel_drag;
SWP_STATIC_ASSERT(sizeof(swp_cancel_drag) == 4, "wire layout");

/* A window's answer to each DROP_ENTER and DROP_MOTION. No actions refuses, the words show only while the DM chose
 * words_action, and an answer for a drag no longer over the window is ignored. */
typedef struct {
    uint32_t win_id;
    uint32_t drag;
    uint32_t actions;        /* SWP_DND_* accepted where the pointer is, 0 refusing */
    uint32_t preferred;      /* one of them, 0 when refusing */
    uint32_t words_action;   /* the SWP_DND_* value the words describe, 0 for a refusal's reason */
    char     words[SWP_DROP_WORDS_MAX];  /* UTF-8, NUL-terminated, empty for none */
} swp_drop_status;
SWP_STATIC_ASSERT(sizeof(swp_drop_status) == 84, "wire layout");

/* Asks for one representation of the drag dropped on win_id. Only the window that took the drop may ask, from its
 * DROP until its DROP_FINISH, and any other ask is answered with nothing. Answered by one DROP_DATA. */
typedef struct {
    uint32_t win_id;
    uint32_t drag;
    char     mime[SWP_MIME_MAX];  /* NUL-terminated */
} swp_drop_get;
SWP_STATIC_ASSERT(sizeof(swp_drop_get) == 72, "wire layout");

enum {
    SWP_DROP_FOCUS = 1u << 0,  /* raise and focus the window, as a drop into a text field asks */
};

/* Ends the drop on win_id with the action done, after which the DM tells the source and closes its objects. An action
 * the DM did not choose counts as none. */
typedef struct {
    uint32_t win_id;
    uint32_t drag;
    uint32_t action;        /* SWP_DND_* done, 0 for none */
    uint32_t flags;         /* SWP_DROP_* */
} swp_drop_finish;
SWP_STATIC_ASSERT(sizeof(swp_drop_finish) == 16, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_hello_reply) == 16, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_configure) == 20, "wire layout");

/* The buffer is no longer read by the DM and may be reused or freed */
typedef struct {
    uint32_t win_id;
    uint32_t buf_id;
} swp_release;
SWP_STATIC_ASSERT(sizeof(swp_release) == 8, "wire layout");

/* Sent once per presentation of the window on its toplevel's output,
 * the one holding the largest part of the toplevel and its decorations,
 * or on another while the DM cannot draw there. Never sent per commit,
 * so a superseded commit produces no frame event. */
typedef struct {
    uint32_t win_id;
    uint32_t reserved;      /* keeps time_ns aligned, must be 0 */
    uint64_t time_ns;       /* CLOCK_MONOTONIC presentation time */
} swp_frame_done;
SWP_STATIC_ASSERT(sizeof(swp_frame_done) == 16, "wire layout");

enum {
    SWP_EV_KEY_DOWN = 1,
    SWP_EV_KEY_UP,
    SWP_EV_KEY_REPEAT,
    SWP_EV_MOTION,
    SWP_EV_BUTTON_DOWN,     /* `ch` holds the press's serial, new and nonzero for every press routed to a window */
    SWP_EV_BUTTON_UP,
    SWP_EV_SCROLL,
    SWP_EV_ENTER,
    SWP_EV_LEAVE,
    SWP_EV_FOCUS_IN,
    SWP_EV_FOCUS_OUT,
    SWP_EV_CLOSE,
    SWP_EV_POPUP_DISMISSED,
    SWP_EV_PASTE,           /* paste the clipboard as the toolkit's own paste key would */
};

/* One fixed shape for every event kind keeps parsing a loop. Key
 * events carry the DM-translated UTF-32 codepoint alongside the raw
 * HID usage, so clients never own keymap tables. */
typedef struct {
    uint16_t kind;          /* SWP_EV_* */
    uint8_t  modifiers;     /* key and pointer records, the modifiers held */
    uint8_t  button;        /* button records */
    uint16_t usage;         /* key records, HID usage */
    int16_t  scroll;        /* scroll records, signed detents */
    uint32_t ch;            /* key records, UTF-32 codepoint or 0, and BUTTON_DOWN's press serial */
    int32_t  x, y;          /* pointer records, window content local */
} swp_event_rec;
SWP_STATIC_ASSERT(sizeof(swp_event_rec) == 20, "wire layout");

/* EVENT payload: this prefix, then count swp_event_rec records */
typedef struct {
    uint32_t win_id;
    uint32_t count;
} swp_event_prefix;
SWP_STATIC_ASSERT(sizeof(swp_event_prefix) == 8, "wire layout");

/* The answer to a CLIPBOARD_GET. A nonzero len comes with one SCM_RIGHTS handle of the asked representation's shared
 * memory object, which every client reading the clipboard shares, so none may write it. */
typedef struct {
    uint64_t len;           /* bytes from the object's start, 0 while the clipboard holds no such representation */
} swp_clipboard_data;
SWP_STATIC_ASSERT(sizeof(swp_clipboard_data) == 8, "wire layout");

/* The clipboard item's types, sent after a CLIPBOARD_WATCH, which has no payload, and again after every change of the
 * clipboard until the client disconnects */
typedef struct {
    uint32_t count;                                    /* representations, 0 while the clipboard is empty */
    char     types[SWP_OFFER_TYPES_MAX][SWP_MIME_MAX]; /* in the offer's order, `count` used, the rest zero */
} swp_clipboard_types;
SWP_STATIC_ASSERT(sizeof(swp_clipboard_types) == 260, "wire layout");

enum {
    SWP_DRAG_STARTED   = 1,  /* the DM took the drag, and its picture follows the pointer */
    SWP_DRAG_REFUSED   = 2,  /* the press had ended, another drag ran or an overlay held the input, so no drag */
    SWP_DRAG_FINISHED  = 3,  /* a target finished the drop with `action`, 0 when it did nothing */
    SWP_DRAG_CANCELLED = 4,  /* Esc, a release where nothing accepted, or the source's CANCEL_DRAG */
};

/* The course of a drag, sent to the window that started it: STARTED or REFUSED, then FINISHED or CANCELLED */
typedef struct {
    uint32_t win_id;
    uint32_t drag;          /* the serial START_DRAG named */
    uint32_t state;         /* SWP_DRAG_* */
    uint32_t action;        /* FINISHED's SWP_DND_* action, else 0 */
} swp_drag_state;
SWP_STATIC_ASSERT(sizeof(swp_drag_state) == 16, "wire layout");

enum {
    SWP_DROP_OWN = 1u << 0,  /* the drag comes from this connection */
};

/* A drag came over win_id's content. The window answers it and each DROP_MOTION with a DROP_STATUS, and refuses
 * until it does. */
typedef struct {
    uint32_t win_id;
    uint32_t drag;
    int32_t  x, y;          /* window content local */
    uint32_t actions;       /* SWP_DND_* the source allows */
    uint32_t item_count;
    uint32_t flags;         /* SWP_DROP_OWN */
    uint32_t modifiers;     /* held, as key records carry them */
    uint32_t count;         /* representations */
    char     types[SWP_OFFER_TYPES_MAX][SWP_MIME_MAX];  /* in the offer's order, `count` used, the rest zero */
} swp_drop_enter;
SWP_STATIC_ASSERT(sizeof(swp_drop_enter) == 292, "wire layout");

/* The drag over win_id moved or its modifiers changed, sent at most once per DM wakeup */
typedef struct {
    uint32_t win_id;
    uint32_t drag;
    int32_t  x, y;
    uint32_t modifiers;
} swp_drop_motion;
SWP_STATIC_ASSERT(sizeof(swp_drop_motion) == 20, "wire layout");

/* The drag left win_id's content, or ended there without a drop on it */
typedef struct {
    uint32_t win_id;
    uint32_t drag;
} swp_drop_leave;
SWP_STATIC_ASSERT(sizeof(swp_drop_leave) == 8, "wire layout");

/* The drag was dropped on win_id, whose last DROP_STATUS accepted it. The window asks for the representations it
 * reads with DROP_GET, then sends DROP_FINISH, and after 5 seconds without one counts as having done nothing. */
typedef struct {
    uint32_t win_id;
    uint32_t drag;
    int32_t  x, y;
    uint32_t action;        /* the SWP_DND_* action the DM chose */
    uint32_t modifiers;
} swp_drop;
SWP_STATIC_ASSERT(sizeof(swp_drop) == 24, "wire layout");

/* The answer to a DROP_GET, a nonzero len with one SCM_RIGHTS handle of a shared memory object no reader may write,
 * and 0 when the drop holds no such representation or is not the asking window's to read */
typedef struct {
    uint64_t len;           /* bytes from the object's start */
} swp_drop_data;
SWP_STATIC_ASSERT(sizeof(swp_drop_data) == 8, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_modes) == 544, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_mode_result) == 32, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_gpu_info) == 8, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_desktop_output) == 80, "wire layout");

/* The desktop's layout, sent after HELLO_REPLY and at the end of each
 * DM wakeup to every client holding an older generation */
typedef struct {
    uint32_t           generation;               /* raised by one at every change of what OUTPUTS carries, from 1 */
    uint32_t           count;                    /* members, the outputs holding a place, at least 1 */
    swp_desktop_output outputs[SWP_OUTPUTS_MAX]; /* ascending output index, `count` used, the rest zero */
} swp_outputs;
SWP_STATIC_ASSERT(sizeof(swp_outputs) == 1288, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_window_output) == 16, "wire layout");

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
SWP_STATIC_ASSERT(sizeof(swp_layout_result) == 8, "wire layout");

/* Read buffer bound for either side of the socket, which every message fits */
#define SWP_MAX_MSG_SIZE 4096u
SWP_STATIC_ASSERT(sizeof(swp_header) + sizeof(swp_outputs) <= SWP_MAX_MSG_SIZE, "an OUTPUTS fits a read");
SWP_STATIC_ASSERT(sizeof(swp_header) + sizeof(swp_event_prefix) +
                      SWP_EVENTS_MAX_BATCH * sizeof(swp_event_rec) <= SWP_MAX_MSG_SIZE,
                  "a full EVENT fits a read");
SWP_STATIC_ASSERT(SWP_HANDLES_MAX >= SWP_GPU_HANDLES && SWP_HANDLES_MAX >= 1 + SWP_OFFER_TYPES_MAX,
                  "an attach's handles, and a drag's picture with its offer's, fit one message");

#endif /* STLXWIN_PROTO_H */
