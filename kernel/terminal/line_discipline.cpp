#include "terminal/line_discipline.h"
#include "terminal/terminal.h"
#include "signals/signal_types.h"
#include "common/ring_buffer.h"
#include "dynpriv/dynpriv.h"

namespace terminal {

// Signal-generating control bytes (termios VINTR/VQUIT/VSUSP defaults)
constexpr char LD_CH_INTR = 0x03; // ^C
constexpr char LD_CH_QUIT = 0x1C; // ^backslash
constexpr char LD_CH_SUSP = 0x1A; // ^Z

constexpr uint8_t LD_CH_BELL = 0x07;
constexpr size_t LD_TAB_WIDTH = 8;
constexpr size_t LD_UTF8_CONTINUATION_MAX = 3;

void ld_init(line_discipline* ld) {
    ld->mode = 0;
    ld->isig = 1;
    ld->line_len = 0;
    ld->prev_char = 0;
    ld->lock = sync::SPINLOCK_INIT;
}

static uint32_t signal_for_char(char c) {
    switch (c) {
        case LD_CH_INTR: return signals::SIGINT;
        case LD_CH_QUIT: return signals::SIGQUIT;
        case LD_CH_SUSP: return signals::SIGTSTP;
        default:         return 0;
    }
}

// Echo "^X" plus CRLF so the next output starts on a fresh line
static void echo_signal_char(const echo_target* echo, char c) {
    if (echo && echo->write) {
        const uint8_t seq[] = {'^', static_cast<uint8_t>(c + 0x40), '\r', '\n'};
        echo->write(echo->ctx, seq, sizeof(seq));
    }
}

// A byte a cooked line keeps: printable ASCII, a tab, or a byte of a UTF-8 character
static bool is_line_byte(char c) {
    uint8_t byte = static_cast<uint8_t>(c);
    return c == '\t' || (byte >= 0x20 && byte != 0x7F);
}

static bool is_utf8_continuation(char c) {
    return (static_cast<uint8_t>(c) & 0xC0) == 0x80;
}

static bool is_utf8_lead(char c) {
    return (static_cast<uint8_t>(c) & 0xC0) == 0xC0;
}

// The column the line's first `len` bytes reach, counted from where the line began
static size_t line_column(const line_discipline* ld, size_t len) {
    size_t column = 0;
    for (size_t i = 0; i < len; i++) {
        if (ld->line_buf[i] == '\t') {
            column += LD_TAB_WIDTH - column % LD_TAB_WIDTH;
        } else if (!is_utf8_continuation(ld->line_buf[i])) {
            column++;
        }
    }

    return column;
}

// Drops the line's last character, every byte of a UTF-8 one, and writes the echo that rubs it
// out: a backspace per column a tab took, or backspace, space, backspace. Returns the echo's length.
static size_t erase_last_char(line_discipline* ld, uint8_t* echo_seq) {
    size_t last = ld->line_len - 1;
    size_t start = last;
    while (start > 0 && last - start < LD_UTF8_CONTINUATION_MAX && is_utf8_continuation(ld->line_buf[start])) {
        start--;
    }

    if (start != last && !is_utf8_lead(ld->line_buf[start])) {
        start = last;
    }

    size_t echo_len = 3;
    if (ld->line_buf[start] == '\t') {
        echo_len = line_column(ld, ld->line_len) - line_column(ld, start);
        for (size_t i = 0; i < echo_len; i++) {
            echo_seq[i] = '\b';
        }
    } else {
        echo_seq[0] = '\b';
        echo_seq[1] = ' ';
        echo_seq[2] = '\b';
    }

    ld->line_len = start;
    return echo_len;
}

static void ld_process_byte(line_discipline* ld, ring_buffer* sink,
                             const echo_target* echo, const signal_target* sig,
                             char c, bool hold_lock, sync::irq_state& irq) {
    uint32_t signum = ld->isig ? signal_for_char(c) : 0;
    if (signum) {
        // The byte becomes a signal instead of input, and the pending
        // line is flushed (Linux ISIG semantics)
        bool cooked = ld->mode != LD_MODE_RAW;
        ld->line_len = 0;
        ld->prev_char = c;
        if (!hold_lock) sync::spin_unlock_irqrestore(ld->lock, irq);

        if (cooked) {
            echo_signal_char(echo, c);
        }
        if (sig && sig->send) {
            sig->send(sig->ctx, signum);
        }
        return;
    }

    if (ld->mode == LD_MODE_RAW) {
        ld->prev_char = c;
        if (!hold_lock) sync::spin_unlock_irqrestore(ld->lock, irq);

        uint8_t byte = static_cast<uint8_t>(c);
        (void)ring_buffer_write(sink, &byte, 1, true);
        return;
    }

    if (ld->prev_char == '\r' && c == '\n') {
        ld->prev_char = c;
        if (!hold_lock) sync::spin_unlock_irqrestore(ld->lock, irq);
        return;
    }

    ld->prev_char = c;

    if (c == '\r' || c == '\n') {
        ld->line_buf[ld->line_len] = '\n';
        size_t len = ld->line_len + 1;
        if (!hold_lock) sync::spin_unlock_irqrestore(ld->lock, irq);

        (void)ring_buffer_write(sink,
                               reinterpret_cast<const uint8_t*>(ld->line_buf),
                               len, true);
        ld->line_len = 0;
        if (echo && echo->write) {
            static const uint8_t crlf[] = {'\r', '\n'};
            echo->write(echo->ctx, crlf, 2);
        }
    } else if (c == 0x7F || c == 0x08) {
        uint8_t erase_seq[LD_TAB_WIDTH];
        size_t erase_len = ld->line_len > 0 ? erase_last_char(ld, erase_seq) : 0;
        if (!hold_lock) sync::spin_unlock_irqrestore(ld->lock, irq);

        if (erase_len > 0 && echo && echo->write) {
            echo->write(echo->ctx, erase_seq, erase_len);
        }
    } else if (is_line_byte(c)) {
        // A full line drops the byte and rings the bell in place of its echo
        bool fits = ld->line_len < LD_LINE_BUF_MAX;
        if (fits) {
            ld->line_buf[ld->line_len++] = c;
        }

        if (!hold_lock) sync::spin_unlock_irqrestore(ld->lock, irq);

        if (echo && echo->write) {
            uint8_t byte = fits ? static_cast<uint8_t>(c) : LD_CH_BELL;
            echo->write(echo->ctx, &byte, 1);
        }
    } else {
        if (!hold_lock) sync::spin_unlock_irqrestore(ld->lock, irq);
    }
}

__PRIVILEGED_CODE void ld_input(line_discipline* ld, ring_buffer* sink,
                                 const echo_target* echo,
                                 const signal_target* sig, char c) {
    sync::irq_state irq = sync::spin_lock_irqsave(ld->lock);
    ld_process_byte(ld, sink, echo, sig, c, false, irq);
}

// The sink room taking c needs: a byte in raw mode, the whole pending line when c ends one in cooked mode
static size_t room_for_byte(const line_discipline* ld, char c) {
    if (ld->isig && signal_for_char(c)) {
        return 0;
    }

    if (ld->mode == LD_MODE_RAW) {
        return 1;
    }

    bool is_line_end = (c == '\r' || c == '\n') && !(ld->prev_char == '\r' && c == '\n');
    return is_line_end ? ld->line_len + 1 : 0;
}

__PRIVILEGED_CODE static bool sink_has_room(ring_buffer* sink, size_t len) {
    return len == 0 || ring_buffer_wait_writable(sink, len, false, true) >= 0;
}

__PRIVILEGED_CODE size_t ld_input_buf(line_discipline* ld, ring_buffer* sink,
                                       const echo_target* echo,
                                       const signal_target* sig,
                                       const char* buf, size_t len) {
    sync::irq_state irq = sync::spin_lock_irqsave(ld->lock);

    // The raw bulk path only applies with ISIG off, interception needs
    // the per-byte scan
    if (ld->mode == LD_MODE_RAW && !ld->isig) {
        sync::spin_unlock_irqrestore(ld->lock, irq);
        ssize_t written = ring_buffer_write(sink, reinterpret_cast<const uint8_t*>(buf), len, true);
        return written > 0 ? static_cast<size_t>(written) : 0;
    }

    size_t taken = 0;
    while (taken < len && sink_has_room(sink, room_for_byte(ld, buf[taken]))) {
        ld_process_byte(ld, sink, echo, sig, buf[taken], true, irq);
        taken++;
    }

    sync::spin_unlock_irqrestore(ld->lock, irq);
    return taken;
}

__PRIVILEGED_CODE size_t ld_room_needed(line_discipline* ld) {
    sync::irq_state irq = sync::spin_lock_irqsave(ld->lock);
    size_t room = ld->mode == LD_MODE_RAW ? 1 : ld->line_len + 1;
    sync::spin_unlock_irqrestore(ld->lock, irq);
    return room;
}

int32_t ld_set_mode(line_discipline* ld, uint32_t cmd) {
    uint32_t new_mode;
    if (cmd == STLX_TCSETS_RAW) {
        new_mode = LD_MODE_RAW;
    } else if (cmd == STLX_TCSETS_COOKED) {
        new_mode = 0;
    } else {
        return ERR;
    }

    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(ld->lock);
        if (ld->mode != new_mode) {
            ld->line_len = 0;
            ld->mode = new_mode;
        }
        // The shortcuts pair ISIG with the mode, raw callers expect
        // control bytes verbatim (cfmakeraw clears ISIG the same way)
        ld->isig = (new_mode == LD_MODE_RAW) ? 0u : 1u;
        sync::spin_unlock_irqrestore(ld->lock, irq);
    });

    return OK;
}

void ld_set_isig(line_discipline* ld, bool on) {
    RUN_ELEVATED({
        sync::irq_state irq = sync::spin_lock_irqsave(ld->lock);
        ld->isig = on ? 1u : 0u;
        sync::spin_unlock_irqrestore(ld->lock, irq);
    });
}

} // namespace terminal
