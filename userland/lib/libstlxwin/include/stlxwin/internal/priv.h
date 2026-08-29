#ifndef STLXWIN_INTERNAL_PRIV_H
#define STLXWIN_INTERNAL_PRIV_H

/*
 * Internal state shared by the library's translation units. Not
 * installed to the sysroot, applications never see these shapes.
 */

#include <stlxwin/proto.h>
#include <stlxwin/stlxwin.h>

#define STLXWIN_EVQ_INITIAL 64u
#define STLXWIN_BUFS_PER_WINDOW 2u

typedef struct stlxwin_buf_slot {
    uint32_t buf_id;
    uint32_t width, height;
    size_t   size;              /* mapped bytes */
    uint32_t* pixels;           /* NULL when the slot is empty */
    int      owned;             /* writable by the app right now */
    stlxwin_buffer view;        /* handed to the app by begin_frame */
} stlxwin_buf_slot;

struct stlxwin_window {
    stlxwin_conn* conn;
    struct stlxwin_window* next;
    uint32_t win_id;
    uint32_t parent_id;         /* 0 for toplevels */

    /* Latest configure, adopted by the next begin_frame */
    uint32_t conf_w, conf_h;
    uint32_t conf_serial;

    stlxwin_buf_slot bufs[STLXWIN_BUFS_PER_WINDOW];
    uint32_t next_buf_id;
    int      want_frame;        /* deliver the next FRAME_DONE as an event */
    int      dead;
};

struct stlxwin_conn {
    int fd;
    int dead;

    /* Partial message assembly for the nonblocking read loop */
    uint8_t  rd_buf[SWP_MAX_MSG_SIZE];
    uint32_t rd_have;

    /* Growable event ring */
    stlxwin_event* evq;
    uint32_t evq_cap;
    uint32_t evq_head, evq_count;

    struct stlxwin_window* windows;
    uint32_t next_win_id;

    uint32_t screen_w, screen_h;

    /* Last CLIPBOARD_DATA payload, owned by the connection */
    char*  clip_text;
    size_t clip_len;
    int    clip_pending;        /* a CLIPBOARD_GET answer is outstanding */
};

/* conn.c */
int stlxwin_send(stlxwin_conn* conn, uint16_t type,
                 const void* payload, uint32_t length);
int stlxwin_evq_push(stlxwin_conn* conn, const stlxwin_event* ev);
struct stlxwin_window* stlxwin_find_window(stlxwin_conn* conn,
                                           uint32_t win_id);

/* window.c */
void stlxwin_window_free(struct stlxwin_window* win);

/* buffer.c */
void stlxwin_buffers_release_slot(struct stlxwin_window* win,
                                  uint32_t buf_id);
void stlxwin_buffers_free_all(struct stlxwin_window* win);

#endif /* STLXWIN_INTERNAL_PRIV_H */
