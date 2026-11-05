#ifndef STLXWIN_INTERNAL_PRIV_H
#define STLXWIN_INTERNAL_PRIV_H

/*
 * Internal state shared by the library's translation units. Not
 * installed to the sysroot, applications never see these shapes.
 *
 * A connection's lock is held by every send and around what the GPU
 * calls and stlxwin_window_configured share with the dispatching
 * thread: the GPU records and descriptor, gpu_info, dead, and each
 * window's configure, unacked configures, acks, next_buf_id and
 * want_frame. A function named _locked must run with it held.
 */

#include <stlxwin/proto.h>
#include <stlxwin/stlxwin.h>

#include <pthread.h>
#include <stdatomic.h>

#define STLXWIN_EVQ_INITIAL 64u
#define STLXWIN_BUFS_PER_WINDOW 2u
#define STLXWIN_UNACKED_CONFS_MAX 8u  /* as many sent CONFIGUREs as the DM remembers */

typedef struct stlxwin_buf_slot {
    uint32_t buf_id;
    uint32_t width, height;
    size_t   size;              /* mapped bytes */
    uint32_t* pixels;           /* NULL when the slot is empty */
    int      owned;             /* writable by the app right now */
    stlxwin_buffer view;        /* handed to the app by begin_frame */
} stlxwin_buf_slot;

/* A CONFIGURE newer than the window's last ack */
typedef struct {
    uint32_t serial;
    uint32_t width, height;
} stlxwin_unacked_conf;

/* An attached GPU buffer, kept after a detach until its last release
 * point is reached, since the DM may show it until then */
typedef struct {
    struct stlxwin_window* win; /* NULL when the record is free */
    uint32_t buf_id;
    uint32_t width, height;
    int      release;           /* the library's own copy of the release timeline, read and write */
    uint64_t release_point;     /* the last committed, 0 before the first commit */
    int      detached;
} stlxwin_gpu_record;

struct stlxwin_window {
    stlxwin_conn* conn;
    struct stlxwin_window* next;
    uint32_t win_id;
    uint32_t parent_id;         /* 0 for toplevels */

    /* Latest configure, adopted by the next begin_frame. Only the
     * dispatching thread writes it, so its reads need no lock */
    uint32_t conf_w, conf_h;
    uint32_t conf_serial;

    stlxwin_unacked_conf unacked_confs[STLXWIN_UNACKED_CONFS_MAX];
    uint32_t unacked_conf_count;
    uint32_t acked_serial;      /* the newest serial a commit of this window acked */

    stlxwin_buf_slot bufs[STLXWIN_BUFS_PER_WINDOW];
    uint32_t next_buf_id;
    int      want_frame;        /* deliver the next FRAME_DONE as an event */
    int      dead;
};

struct stlxwin_conn {
    int fd;
    atomic_int dead;            /* set under lock as every release point is reached, read anywhere */
    int disconnect_queued;      /* STLXWIN_EVT_DISCONNECTED went into the event queue */

    pthread_mutex_t lock;

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

    swp_gpu_info gpu_info;      /* the newest GPU_INFO, zero from a DM that sends none */
    int gpu_fd;                 /* /dev/gpuN, opened by the first GPU attach, -1 before */
    stlxwin_gpu_record gpu_records[SWP_GPU_BUFFERS_MAX];

    /* Last CLIPBOARD_DATA payload, owned by the connection */
    char*  clip_text;
    size_t clip_len;
    int    clip_pending;        /* a CLIPBOARD_GET answer is outstanding */

    /* Last MODES of each output, allocated at the first watch */
    swp_modes* modes;
    uint32_t   modes_kept;      /* one bit per output whose MODES arrived */
};

/* conn.c */
int stlxwin_send(stlxwin_conn* conn, uint16_t type,
                 const void* payload, uint32_t length);
int stlxwin_send_locked(stlxwin_conn* conn, uint16_t type,
                        const void* payload, uint32_t length);
int stlxwin_send_handles_locked(stlxwin_conn* conn, uint16_t type,
                                const void* payload, uint32_t length,
                                const int handles[SWP_GPU_HANDLES]);
void stlxwin_mark_dead(stlxwin_conn* conn);
void stlxwin_mark_dead_locked(stlxwin_conn* conn);
int stlxwin_evq_push(stlxwin_conn* conn, const stlxwin_event* ev);
struct stlxwin_window* stlxwin_find_window(stlxwin_conn* conn,
                                           uint32_t win_id);

/* window.c */
void stlxwin_window_free(struct stlxwin_window* win);
void stlxwin_window_keep_configure_locked(struct stlxwin_window* win,
                                          const swp_configure* conf);
void stlxwin_window_record_ack_locked(struct stlxwin_window* win,
                                      uint32_t serial);

/* buffer.c */
void stlxwin_buffers_release_slot(struct stlxwin_window* win,
                                  uint32_t buf_id);
void stlxwin_buffers_free_all(struct stlxwin_window* win);
uint32_t stlxwin_wire_damage(swp_rect* wire, const stlxwin_rect* damage,
                             uint32_t n_damage);

/* gpu.c */
void stlxwin_gpu_release_records_locked(stlxwin_conn* conn,
                                        const struct stlxwin_window* win);

/* modes.c */
stlxwin_mode stlxwin_mode_from_swp(const swp_mode* mode);

#endif /* STLXWIN_INTERNAL_PRIV_H */
