#define _POSIX_C_SOURCE 200809L
#include <stlxwin/internal/priv.h>

#include <stdlib.h>

stlxwin_mode stlxwin_mode_from_swp(const swp_mode* mode) {
    stlxwin_mode out = { mode->width, mode->height, mode->refresh_millihertz };
    return out;
}

int stlxwin_modes_watch(stlxwin_conn* conn, uint32_t output) {
    if (!conn || output >= SWP_OUTPUTS_MAX) {
        return -1;
    }

    if (!conn->modes) {
        conn->modes = calloc(SWP_OUTPUTS_MAX, sizeof(*conn->modes));
        if (!conn->modes) {
            return -1;
        }
    }

    swp_get_modes msg = { output };
    return stlxwin_send(conn, SWP_MSG_GET_MODES, &msg, sizeof(msg));
}

long stlxwin_modes_get(stlxwin_conn* conn, uint32_t output,
                       stlxwin_modes_info* info, stlxwin_mode* modes,
                       size_t capacity) {
    if (!conn || output >= SWP_OUTPUTS_MAX || (conn->modes_kept & (1u << output)) == 0) {
        return -1;
    }

    const swp_modes* kept = &conn->modes[output];
    if (info) {
        info->output_count = kept->output_count;
        info->flags = kept->flags;
        info->current = stlxwin_mode_from_swp(&kept->current);
    }

    size_t copied = kept->count < capacity ? kept->count : capacity;
    for (size_t i = 0; i < copied; i++) {
        modes[i] = stlxwin_mode_from_swp(&kept->modes[i]);
    }

    return (long)kept->count;
}

int stlxwin_mode_set(stlxwin_conn* conn, uint32_t output,
                     const stlxwin_mode* mode, uint32_t flags) {
    if (!conn || !mode) {
        return -1;
    }

    swp_set_mode msg = {
        .output = output,
        .flags  = flags,
        .mode   = { mode->width, mode->height, mode->refresh_millihertz },
    };

    return stlxwin_send(conn, SWP_MSG_SET_MODE, &msg, sizeof(msg));
}

int stlxwin_mode_keep(stlxwin_conn* conn, uint32_t output, int keep) {
    if (!conn) {
        return -1;
    }

    swp_keep_mode msg = { output, (uint32_t)keep };
    return stlxwin_send(conn, SWP_MSG_KEEP_MODE, &msg, sizeof(msg));
}
