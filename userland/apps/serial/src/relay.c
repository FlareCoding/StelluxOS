#define _POSIX_C_SOURCE 200809L

#include "relay.h"

#define ENTER '\r'

/* Writes the line end Enter sends to the board into `out`, returning its length */
static size_t write_line_end(eol_kind eol, uint8_t* out) {
    if (eol == EOL_LF) {
        out[0] = '\n';
        return 1;
    }

    if (eol == EOL_CRLF) {
        out[0] = '\r';
        out[1] = '\n';
        return 2;
    }

    out[0] = ENTER;
    return 1;
}

static bool shows_as_crlf(eol_kind eol, uint8_t byte) {
    return (eol == EOL_LF && byte == '\n') || (eol == EOL_CR && byte == '\r');
}

session_options session_options_default(void) {
    return (session_options){ .echo = false, .eol = EOL_NONE, .xonxoff = false };
}

void relay_init(relay* r, const session_options* options) {
    *r = (relay){ .options = *options };
}

void relay_restart(relay* r) {
    r->is_sending_paused = false;
    r->wants_board_paused = false;
    r->flow_pending = r->is_board_paused ? XON : 0;
}

typed_bytes relay_typed(const relay* r, const uint8_t* in, size_t length, uint8_t* to_port, uint8_t* to_screen) {
    typed_bytes taken = { 0 };
    for (size_t i = 0; i < length; i++) {
        uint8_t byte = in[i];
        if (byte == QUIT_KEY) {
            taken.is_quit = true;
            break;
        }

        if (byte == ENTER) {
            taken.to_port += write_line_end(r->options.eol, to_port + taken.to_port);
            if (r->options.echo) {
                to_screen[taken.to_screen++] = '\r';
                to_screen[taken.to_screen++] = '\n';
            }

            continue;
        }

        to_port[taken.to_port++] = byte;
        if (r->options.echo) {
            to_screen[taken.to_screen++] = byte;
        }
    }

    return taken;
}

size_t relay_received(relay* r, const uint8_t* in, size_t length, uint8_t* to_screen) {
    size_t shown = 0;
    for (size_t i = 0; i < length; i++) {
        uint8_t byte = in[i];
        if (r->options.xonxoff && (byte == XON || byte == XOFF)) {
            r->is_sending_paused = byte == XOFF;
            continue;
        }

        if (shows_as_crlf(r->options.eol, byte)) {
            to_screen[shown++] = '\r';
            to_screen[shown++] = '\n';
            continue;
        }

        to_screen[shown++] = byte;
    }

    return shown;
}

void relay_watch_queue(relay* r, size_t unread) {
    if (!r->options.xonxoff) {
        return;
    }

    uint8_t decided = 0;
    if (!r->wants_board_paused && unread >= HOLD_AT) {
        r->wants_board_paused = true;
        decided = XOFF;
    } else if (r->wants_board_paused && unread <= RESUME_AT) {
        r->wants_board_paused = false;
        decided = XON;
    }

    if (decided != 0) {
        r->flow_pending = r->flow_pending != 0 ? 0 : decided;
    }
}

void relay_flow_sent(relay* r) {
    r->is_board_paused = r->flow_pending == XOFF;
    r->flow_pending = 0;
}
