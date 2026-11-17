/* relay.c on the build machine: Ctrl-], local echo, the board's line end both ways, and XON and XOFF */
#define _POSIX_C_SOURCE 200809L

#include "relay.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_REPORTS 8
#define BYTES_MAX   64

static uint64_t g_checks;
static uint64_t g_failures;

static void check(bool holds, const char* what) {
    g_checks++;
    if (holds) {
        return;
    }

    g_failures++;
    if (g_failures <= MAX_REPORTS) {
        printf("  FAIL: %s\n", what);
    }
}

static relay relay_with(bool echo, eol_kind eol, bool xonxoff) {
    session_options options = { .echo = echo, .eol = eol, .xonxoff = xonxoff };
    relay r;
    relay_init(&r, &options);

    return r;
}

static bool bytes_are(const uint8_t* bytes, size_t length, const char* expected) {
    return length == strlen(expected) && memcmp(bytes, expected, length) == 0;
}

/* Whether typing `in` sends `to_port` to the board, echoes `to_screen`, and quits as `is_quit` says */
static bool typing_gives(const relay* r, const char* in, const char* to_port, const char* to_screen, bool is_quit) {
    uint8_t port[BYTES_MAX];
    uint8_t screen[BYTES_MAX];
    typed_bytes taken = relay_typed(r, (const uint8_t*)in, strlen(in), port, screen);

    return bytes_are(port, taken.to_port, to_port) && bytes_are(screen, taken.to_screen, to_screen) &&
           taken.is_quit == is_quit;
}

static bool receiving_shows(relay* r, const char* in, const char* shown) {
    uint8_t screen[BYTES_MAX];
    size_t length = relay_received(r, (const uint8_t*)in, strlen(in), screen);

    return bytes_are(screen, length, shown);
}

static void check_typing(void) {
    session_options defaults = session_options_default();
    check(!defaults.echo && defaults.eol == EOL_NONE && !defaults.xonxoff, "by default nothing is changed");

    relay plain = relay_with(false, EOL_NONE, false);
    check(typing_gives(&plain, "ab\rc", "ab\rc", "", false), "with no options typed bytes pass unchanged and unechoed");
    check(typing_gives(&plain, "\x1d", "", "", true), "Ctrl-] alone quits");
    check(typing_gives(&plain, "ab\x1d" "cd", "ab", "", true), "Ctrl-] quits and the bytes after it are not taken");
    check(typing_gives(&plain, "abc\x1d", "abc", "", true), "Ctrl-] last quits after the bytes before it");

    relay echoing = relay_with(true, EOL_NONE, false);
    check(typing_gives(&echoing, "hi\r", "hi\r", "hi\r\n", false), "echo shows each byte, and Enter as CR LF");

    relay lf = relay_with(false, EOL_LF, false);
    relay crlf = relay_with(true, EOL_CRLF, false);
    relay cr = relay_with(false, EOL_CR, false);

    check(typing_gives(&lf, "x\r", "x\n", "", false), "with --eol lf, Enter sends LF");
    check(typing_gives(&crlf, "x\r", "x\r\n", "x\r\n", false), "with --eol crlf, Enter sends CR LF and echoes once");
    check(typing_gives(&cr, "x\r", "x\r", "", false), "with --eol cr, Enter sends CR");
    check(typing_gives(&lf, "a\n", "a\n", "", false), "a typed LF passes as it is");
}

static void check_receiving(void) {
    relay plain = relay_with(false, EOL_NONE, false);
    relay lf = relay_with(false, EOL_LF, false);
    relay cr = relay_with(false, EOL_CR, false);
    relay crlf = relay_with(false, EOL_CRLF, false);

    check(receiving_shows(&plain, "a\nb\r", "a\nb\r"), "with no --eol the board's bytes show unchanged");
    check(receiving_shows(&lf, "a\nb", "a\r\nb"), "with --eol lf a received LF shows as CR LF");
    check(receiving_shows(&cr, "a\rb", "a\r\nb"), "with --eol cr a received CR shows as CR LF");
    check(receiving_shows(&crlf, "a\r\nb", "a\r\nb"), "with --eol crlf a received CR LF shows as it is");

    relay flowing = relay_with(false, EOL_NONE, true);
    check(receiving_shows(&flowing, "a\x13" "b", "ab") && flowing.is_sending_paused,
          "a received XOFF is taken out and pauses sending");
    check(receiving_shows(&flowing, "\x11", "") && !flowing.is_sending_paused,
          "a received XON is taken out and resumes sending");

    check(receiving_shows(&plain, "a\x13" "b", "a\x13" "b") && !plain.is_sending_paused,
          "without --xonxoff XOFF is a byte");
}

static void check_flow_decisions(void) {
    relay r = relay_with(false, EOL_NONE, true);
    relay_watch_queue(&r, HOLD_AT - 1);
    check(r.flow_pending == 0 && !r.wants_board_paused, "below HOLD_AT nothing is decided");

    relay_watch_queue(&r, HOLD_AT);
    check(r.flow_pending == XOFF && r.wants_board_paused, "at HOLD_AT the tool decides on XOFF");

    relay_flow_sent(&r);
    relay_watch_queue(&r, HOLD_AT * 2);
    check(r.flow_pending == 0 && r.wants_board_paused, "XOFF goes once per crossing");

    relay_watch_queue(&r, RESUME_AT + 1);
    check(r.flow_pending == 0 && r.wants_board_paused, "above RESUME_AT the board stays paused");

    relay_watch_queue(&r, RESUME_AT);
    check(r.flow_pending == XON && !r.wants_board_paused, "at RESUME_AT the tool decides on XON");

    relay pending = relay_with(false, EOL_NONE, true);
    relay_watch_queue(&pending, HOLD_AT);
    relay_watch_queue(&pending, 0);
    check(pending.flow_pending == 0 && !pending.wants_board_paused,
          "an XON decided before its XOFF went out cancels both");

    relay off = relay_with(false, EOL_NONE, false);
    relay_watch_queue(&off, HOLD_AT * 2);
    check(off.flow_pending == 0 && !off.wants_board_paused, "without --xonxoff nothing is decided");

    session_options options = { .xonxoff = true };
    relay_init(&r, &options);
    check(!r.is_sending_paused && !r.wants_board_paused && !r.is_board_paused && r.flow_pending == 0,
          "a relay starts with nothing paused or pending");
}

static void check_returns(void) {
    relay r = relay_with(false, EOL_NONE, true);
    relay_watch_queue(&r, HOLD_AT);
    relay_flow_sent(&r);
    check(r.is_board_paused && r.flow_pending == 0, "an XOFF that went out leaves the board paused");

    relay_restart(&r);
    check(r.flow_pending == XON && !r.wants_board_paused, "a board that comes back paused is sent an XON");

    relay_flow_sent(&r);
    check(!r.is_board_paused && r.flow_pending == 0, "the XON that went out resumes the board");

    relay_restart(&r);
    check(r.flow_pending == 0, "a board that comes back unpaused is sent nothing");

    check(receiving_shows(&r, "\x13", "") && r.is_sending_paused, "the board pauses sending");
    relay_restart(&r);
    check(!r.is_sending_paused, "a board that comes back takes typed bytes again");
}

int main(void) {
    check_typing();
    check_receiving();
    check_flow_decisions();
    check_returns();

    if (g_failures != 0) {
        printf("relay: %llu of %llu checks failed\n", (unsigned long long)g_failures, (unsigned long long)g_checks);
        return 1;
    }

    printf("relay: %llu checks passed\n", (unsigned long long)g_checks);
    return 0;
}
