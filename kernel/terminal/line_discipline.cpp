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
        if (ld->line_len > 0) {
            ld->line_len--;
            if (!hold_lock) sync::spin_unlock_irqrestore(ld->lock, irq);
            if (echo && echo->write) {
                static const uint8_t bs_seq[] = {'\b', ' ', '\b'};
                echo->write(echo->ctx, bs_seq, 3);
            }
        } else {
            if (!hold_lock) sync::spin_unlock_irqrestore(ld->lock, irq);
        }
    } else if (c >= 0x20 && c <= 0x7E) {
        if (ld->line_len < LD_LINE_BUF_MAX) {
            ld->line_buf[ld->line_len++] = c;
            if (!hold_lock) sync::spin_unlock_irqrestore(ld->lock, irq);
            if (echo && echo->write) {
                uint8_t byte = static_cast<uint8_t>(c);
                echo->write(echo->ctx, &byte, 1);
            }
        } else {
            if (!hold_lock) sync::spin_unlock_irqrestore(ld->lock, irq);
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

__PRIVILEGED_CODE void ld_input_buf(line_discipline* ld, ring_buffer* sink,
                                     const echo_target* echo,
                                     const signal_target* sig,
                                     const char* buf, size_t len) {
    sync::irq_state irq = sync::spin_lock_irqsave(ld->lock);

    // The raw bulk path only applies with ISIG off, interception needs
    // the per-byte scan
    if (ld->mode == LD_MODE_RAW && !ld->isig) {
        sync::spin_unlock_irqrestore(ld->lock, irq);
        (void)ring_buffer_write(sink, reinterpret_cast<const uint8_t*>(buf), len, true);
        return;
    }

    for (size_t i = 0; i < len; i++) {
        ld_process_byte(ld, sink, echo, sig, buf[i], true, irq);
    }

    sync::spin_unlock_irqrestore(ld->lock, irq);
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
