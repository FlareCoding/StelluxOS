#ifndef SERIAL_RELAY_H
#define SERIAL_RELAY_H

/* What the monitor does to the bytes it relays. Ctrl-] ends the session, and local echo, the board's line end, and XON
 * and XOFF apply here, since the port refuses the termios flags that would do them */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QUIT_KEY  0x1D        // Ctrl-]
#define XON       0x11
#define XOFF      0x13
#define HOLD_AT   (16 * 1024) // unread bytes, of the port's 64 KiB receive queue, at which the tool sends XOFF
#define RESUME_AT (4 * 1024)  // unread bytes at which it sends XON again

typedef enum {
    EOL_NONE,
    EOL_CR,
    EOL_LF,
    EOL_CRLF,
} eol_kind;

typedef struct {
    bool     echo;
    eol_kind eol;
    bool     xonxoff;
} session_options;

typedef struct {
    session_options options;
    bool            is_sending_paused;  // the board sent XOFF and no XON since, so typed bytes wait
    bool            wants_board_paused; // the tool decided on XOFF and no XON since
    bool            is_board_paused;    // the tool's last flow byte to reach the board was XOFF
    uint8_t         flow_pending;       // the XON or XOFF to write ahead of typed bytes, cleared by relay_flow_sent
} relay;

typedef struct {
    size_t to_port;   // bytes for the board
    size_t to_screen; // bytes to echo
    bool   is_quit;   // Ctrl-] came, and the bytes after it were not taken
} typed_bytes;

/**
 * @brief Echo off, no line end mapping, XON and XOFF off: the bytes pass both ways unchanged.
 */
session_options session_options_default(void);

/**
 * @brief Sets `r` to `options` with nothing paused and no XON or XOFF pending.
 */
void relay_init(relay* r, const session_options* options);

/**
 * @brief Starts `r` over for a board that came back: nothing paused by the board, and an XON pending when the tool's
 * last flow byte to reach the board was XOFF, which a board with its own power still obeys.
 */
void relay_restart(relay* r);

/**
 * @brief Takes typed bytes up to Ctrl-]. Enter, a carriage return from a raw terminal, goes to the board as LF with
 * EOL_LF, as CR LF with EOL_CRLF and as CR otherwise. With echo on, Enter shows as CR LF and other bytes as typed.
 * Each output holds at most twice `length` bytes.
 */
typed_bytes relay_typed(const relay* r, const uint8_t* in, size_t length, uint8_t* to_port, uint8_t* to_screen);

/**
 * @brief Takes the board's bytes for the screen. With XON and XOFF on, those two bytes are taken out, XOFF setting
 * is_sending_paused and XON clearing it, and the board's line end shows as CR LF.
 * @return The bytes written to `to_screen`, at most twice `length`.
 */
size_t relay_received(relay* r, const uint8_t* in, size_t length, uint8_t* to_screen);

/**
 * @brief Decides on XOFF once `unread` reaches HOLD_AT and on XON once it falls to RESUME_AT, each once per crossing,
 * leaving the byte in flow_pending. A decision that finds the other byte still pending clears it instead, since the
 * board never saw that byte. Does nothing with XON and XOFF off.
 */
void relay_watch_queue(relay* r, size_t unread);

/**
 * @brief Records that flow_pending reached the board.
 */
void relay_flow_sent(relay* r);

#endif /* SERIAL_RELAY_H */
