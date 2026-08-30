#ifndef STLXWIN_PROTO_H
#define STLXWIN_PROTO_H

/*
 * The Stellux window protocol wire format, shared by the client
 * library and the display manager.
 *
 * One unix stream socket per client carries every message in both
 * directions. Pixels travel separately through client-created shared
 * memory objects named in ATTACH_BUFFER. Each message is an swp_header
 * followed by header.length payload bytes. All structs are little
 * endian, naturally aligned, padding free, and size asserted so a
 * field edit fails the build instead of corrupting the wire.
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

    SWP_MSG_HELLO_REPLY    = 0x0201,
    SWP_MSG_CONFIGURE      = 0x0202,
    SWP_MSG_RELEASE        = 0x0203,
    SWP_MSG_FRAME_DONE     = 0x0204,
    SWP_MSG_EVENT          = 0x0205,
    SWP_MSG_CLIPBOARD_DATA = 0x0206,
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

/* ---- client to DM ---- */

typedef struct {
    uint32_t proto_version;
    char     app_id[SWP_APP_ID_MAX];
} swp_hello;
_Static_assert(sizeof(swp_hello) == 68, "wire layout");

enum {
    SWP_WF_RESIZABLE  = 1u << 0,
    SWP_WF_BORDERLESS = 1u << 1,
};

enum {
    SWP_PF_GRAB = 1u << 0,  /* outside input dismisses, keyboard follows */
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

/* ---- DM to client ---- */

enum {
    SWP_FMT_XRGB8888 = 1,   /* the only format, negotiation reserved */
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

/* Sent once per presentation of the window, never per commit, so a
 * superseded commit produces no frame event. */
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

/* Read buffer bound for either side of the socket */
#define SWP_MAX_MSG_SIZE \
    (sizeof(swp_header) + sizeof(swp_clipboard_set) + SWP_CLIPBOARD_MAX)

#endif /* STLXWIN_PROTO_H */
