#include "drivers/serial/port.h"
#include "drivers/serial/settings.h"
#include "clock/clock.h"
#include "fs/fs.h"
#include "fs/node.h"
#include "common/logging.h"
#include "common/string.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "sched/sched.h"
#include "signals/signal.h"
#include "sync/poll.h"
#include "terminal/tty.h"

namespace serial {

// Reads copy out through a buffer of this size on the stack, since a copy to a program can fault and so must run
// outside the state lock
static constexpr size_t READ_CHUNK_BYTES = 256;

// VTIME and TCSBRKP count tenths of a second
static constexpr uint64_t NS_PER_TENTH = 100000000;

static constexpr uint64_t NS_PER_SECOND = 1000000000;

// A drain allows this beyond the time the unsent bytes take at the line's speed, for its chip requests
static constexpr uint64_t DRAIN_DEADLINE_MARGIN_NS = NS_PER_SECOND;

// TCSBRK's break, and TCSBRKP's for an argument of 0
static constexpr uint64_t BREAK_NS = 250000000;

// The longest break TCSBRKP may ask for, 5 s as a chip request's deadline, since the break holds the write lock
static constexpr uint64_t MAX_TCSBRKP_TENTHS = 50;

// The modem lines a port drives, and those it reads from the chip
static constexpr uint32_t MODEM_OUTPUTS = terminal::TIOCM_DTR | terminal::TIOCM_RTS;
static constexpr uint32_t MODEM_INPUTS = terminal::TIOCM_CTS | terminal::TIOCM_CAR | terminal::TIOCM_RNG |
                                         terminal::TIOCM_DSR;

namespace {

// A port's node in /dev/serial, which owns the port. It sits in the privileged heap, since `fs::node::ref_destroy`
// frees every node there
class port_node final : public fs::node {
public:
    explicit port_node(port* owned) : fs::node(fs::node_type::char_device, nullptr, ""), m_port(owned) {}

    ~port_node() override {
        heap::ufree_delete(m_port);
    }

    int32_t open(fs::file*, uint32_t) override {
        return m_port->open();
    }

    int32_t on_close(fs::file*) override {
        m_port->close();
        return fs::OK;
    }

    ssize_t read(fs::file*, void* buf, size_t count, uint32_t flags) override {
        return m_port->read(buf, count, flags, false);
    }

    // A port has no offsets to read at
    ssize_t read_to_user(fs::file*, void* ubuf, size_t count, int64_t offset, uint32_t flags) override {
        if (offset >= 0) {
            return fs::ERR_NOSYS;
        }

        return m_port->read(ubuf, count, flags, true);
    }

    ssize_t write(fs::file*, const void* buf, size_t count, uint32_t flags) override {
        return m_port->write(buf, count, flags, false);
    }

    // A port has no offsets to write at
    ssize_t write_from_user(fs::file*, const void* ubuf, size_t count, int64_t offset, uint32_t flags) override {
        if (offset >= 0) {
            return fs::ERR_NOSYS;
        }

        return m_port->write(ubuf, count, flags, true);
    }

    uint32_t poll(fs::file*, sync::poll_table* pt) override {
        return m_port->poll(pt);
    }

    int32_t ioctl(fs::file*, uint32_t cmd, uint64_t arg) override {
        return m_port->request(cmd, arg);
    }

private:
    port* m_port;
};

} // namespace

static size_t min_size(size_t a, size_t b) {
    return a < b ? a : b;
}

static uint64_t min_ns(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

static bool asks_to_hang_up(const terminal::termios2& settings) {
    return (settings.c_cflag & terminal::CBAUD) == terminal::B0;
}

// The time a character's start, data, parity and stop bits take at the line's speed
static uint64_t character_ns(const line_settings& line) {
    uint64_t bits = 1 + line.data_bits + (line.parity_mode == parity::none ? 0 : 1) + line.stop_bits;
    return bits * NS_PER_SECOND / line.baud;
}

static uint64_t tcsbrkp_break_ns(uint64_t arg) {
    return arg == 0 ? BREAK_NS : arg * NS_PER_TENTH;
}

static int32_t driver_result_to_fs(int32_t rc) {
    switch (rc) {
        case OK:              return fs::OK;
        case ERR_UNSUPPORTED: return fs::ERR_INVAL;
        default:              return fs::ERR_IO;
    }
}

/**
 * Copies to a program's buffer or to a kernel buffer, false when the program's buffer faults.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool copy_out(uint8_t* destination, const uint8_t* source, size_t len, bool is_user_buffer) {
    if (!is_user_buffer) {
        string::memcpy(destination, source, len);
        return true;
    }

    return mm::uaccess::copy_to_user(destination, source, len) == mm::uaccess::OK;
}

/**
 * Copies from a program's buffer or from a kernel buffer, false when the program's buffer faults.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool copy_in(uint8_t* destination, const uint8_t* source, size_t len, bool is_user_buffer) {
    if (!is_user_buffer) {
        string::memcpy(destination, source, len);
        return true;
    }

    return mm::uaccess::copy_from_user(destination, source, len) == mm::uaccess::OK;
}

__PRIVILEGED_CODE port* port::create(port_driver* driver, const identity& id) {
    auto* p = heap::ualloc_new<port>();
    if (!p) {
        return nullptr;
    }

    p->m_driver = driver;
    p->m_settings = default_termios();
    if (translate_termios(p->m_settings, &p->m_line) != OK) {
        heap::ufree_delete(p);
        return nullptr;
    }

    p->take_read_settings(p->m_settings);
    p->take_character_time(p->m_line);

    if (!p->copy_identity(id)) {
        heap::ufree_delete(p);
        return nullptr;
    }

    p->m_queue = static_cast<uint8_t*>(heap::ualloc(RECEIVE_QUEUE_BYTES));
    p->m_staging = static_cast<uint8_t*>(heap::ualloc(SEND_BYTES));
    if (!p->m_queue || !p->m_staging) {
        heap::ufree_delete(p);
        return nullptr;
    }

    p->m_node = heap::kalloc_new<port_node>(p);
    if (!p->m_node) {
        heap::ufree_delete(p);
        return nullptr;
    }

    return p;
}

port::port() {
    m_control_lock.init();
    m_write_lock.init();
    m_read_wq.init();
    m_send_wq.init();
}

port::~port() {
    heap::ufree(m_queue);
    heap::ufree(m_staging);
    heap::ufree(m_strings);
}

__PRIVILEGED_CODE bool port::copy_identity(const identity& id) {
    m_identity = id;
    const char** fields[] = {
        &m_identity.chip, &m_identity.serial_number, &m_identity.location, &m_identity.bus, &m_identity.driver,
        &m_identity.manufacturer, &m_identity.product,
    };

    size_t strings_bytes = 0;
    for (const char** field : fields) {
        strings_bytes += string::strlen(*field) + 1;
    }

    m_strings = static_cast<char*>(heap::ualloc(strings_bytes));
    if (!m_strings) {
        return false;
    }

    char* copy = m_strings;
    for (const char** field : fields) {
        size_t text_bytes = string::strlen(*field) + 1;
        string::memcpy(copy, *field, text_bytes);
        *field = copy;
        copy += text_bytes;
    }

    return true;
}

// Opens and closes count files under the control lock, so no other open or close runs during a first open's chip
// request
__PRIVILEGED_CODE int32_t port::open() {
    sync::mutex_lock(m_control_lock);
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    bool is_first = m_open_files == 0;
    bool was_gone = m_is_gone;
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    int32_t rc = fs::OK;
    if (was_gone || !m_driver) {
        rc = fs::ERR_IO;
    } else if (m_is_exclusive) {
        rc = fs::ERR_BUSY;
    } else if (is_first) {
        // The chip still breaks when the last close could not end its break
        rc = m_is_breaking ? change_break(false) : fs::OK;
        if (rc == fs::OK) {
            rc = driver_result_to_fs(m_driver->set_line(m_line));
        }

        if (rc == fs::OK && !asks_to_hang_up(m_settings)) {
            rc = change_modem_outputs(MODEM_OUTPUTS);
        }
    }

    // The device can leave during the first open's chip requests, and then the open fails rather than count a file
    if (rc == fs::OK) {
        irq = sync::spin_lock_irqsave(m_state_lock);
        if (m_is_gone) {
            rc = fs::ERR_IO;
        } else {
            m_open_files++;
        }

        sync::spin_unlock_irqrestore(m_state_lock, irq);
    }

    sync::mutex_unlock(m_control_lock);
    return rc;
}

// A close that is not the last leaves the count at once, so one of two files closing together is the last and drains.
// The last stays counted through its drain, so a file that opens meanwhile is not a first open
__PRIVILEGED_CODE void port::close() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    bool may_be_last = m_open_files == 1;
    if (!may_be_last) {
        m_open_files--;
    }

    sync::spin_unlock_irqrestore(m_state_lock, irq);

    if (!may_be_last) {
        return;
    }

    // A chip holds what it has to send while it breaks, so the break ends before the drain
    sync::mutex_lock(m_control_lock);
    if (m_is_breaking) {
        (void)change_break(false);
    }

    sync::mutex_unlock(m_control_lock);

    sync::mutex_lock(m_write_lock);
    (void)drain(false);
    release_write_lock();

    sync::mutex_lock(m_control_lock);
    irq = sync::spin_lock_irqsave(m_state_lock);
    bool is_last = m_open_files == 1;
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    if (is_last) {
        cancel_started_send();
        if (m_settings.c_cflag & terminal::HUPCL) {
            (void)change_modem_outputs(0);
        }

        m_is_exclusive = false;
    }

    irq = sync::spin_lock_irqsave(m_state_lock);
    m_open_files--;
    if (is_last) {
        m_queue_head = 0;
        m_queued = 0;
    }

    sync::spin_unlock_irqrestore(m_state_lock, irq);
    sync::mutex_unlock(m_control_lock);
}

__PRIVILEGED_CODE int32_t port::request(uint32_t cmd, uint64_t arg) {
    if (cmd == terminal::FIONREAD) {
        return copy_queued_count_out(arg);
    }

    if (is_gone()) {
        return fs::ERR_IO;
    }

    if (cmd == terminal::TCSBRKP && arg > MAX_TCSBRKP_TENTHS) {
        return fs::ERR_INVAL;
    }

    switch (cmd) {
        case terminal::TCGETS:     return copy_settings_out(arg, sizeof(terminal::termios));
        case terminal::TCGETS2:    return copy_settings_out(arg, sizeof(terminal::termios2));
        case terminal::TCSETS:     return apply_settings(arg, sizeof(terminal::termios), false, false);
        case terminal::TCSETSW:    return apply_settings(arg, sizeof(terminal::termios), true, false);
        case terminal::TCSETSF:    return apply_settings(arg, sizeof(terminal::termios), true, true);
        case terminal::TCSETS2:    return apply_settings(arg, sizeof(terminal::termios2), false, false);
        case terminal::TCSETSW2:   return apply_settings(arg, sizeof(terminal::termios2), true, false);
        case terminal::TCSETSF2:   return apply_settings(arg, sizeof(terminal::termios2), true, true);
        case terminal::TCSBRK:     return drain_then_break(arg == 0 ? BREAK_NS : 0);
        case terminal::TCSBRKP:    return drain_then_break(tcsbrkp_break_ns(arg));
        case terminal::TIOCSBRK:   return set_break_request(true);
        case terminal::TIOCCBRK:   return set_break_request(false);
        case terminal::TIOCMGET:   return copy_modem_lines_out(arg);
        case terminal::TIOCMSET:   return take_modem_outputs(cmd, arg);
        case terminal::TIOCMBIS:   return take_modem_outputs(cmd, arg);
        case terminal::TIOCMBIC:   return take_modem_outputs(cmd, arg);
        case terminal::TCFLSH:     return flush(arg);
        case terminal::TIOCOUTQ:   return copy_send_length_out(arg);
        case terminal::TIOCGWINSZ: return copy_window_size_out(arg);
        case terminal::TIOCSWINSZ: return take_window_size(arg);
        case terminal::TIOCEXCL:   return set_exclusive(true);
        case terminal::TIOCNXCL:   return set_exclusive(false);
        case terminal::TCXONC:     return fs::ERR_INVAL;
        default:                   return fs::ERR_NOSYS;
    }
}

__PRIVILEGED_CODE void port::release_driver() {
    sync::mutex_lock(m_write_lock);
    sync::mutex_lock(m_control_lock);
    m_driver = nullptr;
    sync::mutex_unlock(m_control_lock);
    release_write_lock();
}

__PRIVILEGED_CODE bool port::is_gone() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    bool was_gone = m_is_gone;
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    return was_gone;
}

__PRIVILEGED_CODE void port::drop_queued() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    m_queue_head = 0;
    m_queued = 0;
    sync::spin_unlock_irqrestore(m_state_lock, irq);
}

// TCGETS asks for the first bytes of the stored termios2, which have the termios layout, and TCGETS2 for all of it
__PRIVILEGED_CODE int32_t port::copy_settings_out(uint64_t arg, size_t length) {
    sync::mutex_lock(m_control_lock);
    terminal::termios2 settings = m_settings;
    sync::mutex_unlock(m_control_lock);

    void* destination = reinterpret_cast<void*>(arg);
    return mm::uaccess::copy_to_user(destination, &settings, length) == mm::uaccess::OK ? fs::OK : fs::ERR_INVAL;
}

// Stores the settings only once the chip takes them, so TCGETS returns what the last applied TCSETS wrote. TCSETSW
// and TCSETSF hold the write lock from their drain to the change, so no send starts in between
__PRIVILEGED_CODE int32_t port::apply_settings(uint64_t arg, size_t length, bool drains_first, bool drops_queued) {
    terminal::termios2 written = {};
    const void* source = reinterpret_cast<const void*>(arg);
    if (mm::uaccess::copy_from_user(&written, source, length) != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    if (drains_first) {
        sync::mutex_lock(m_write_lock);
        int32_t rc = drain(true);
        if (rc != fs::OK) {
            release_write_lock();
            return rc;
        }
    }

    sync::mutex_lock(m_control_lock);
    terminal::termios2 settings = m_settings;
    string::memcpy(&settings, &written, length);

    line_settings line = {};
    int32_t rc = translate_termios(settings, &line) == OK ? fs::OK : fs::ERR_INVAL;

    // A chip is never asked for B0's speed of 0, so it keeps the speed it has
    if (rc == fs::OK && line.baud == 0) {
        line.baud = m_line.baud;
    }

    if (rc == fs::OK) {
        rc = m_driver ? driver_result_to_fs(m_driver->set_line(line)) : fs::ERR_IO;
    }

    if (rc == fs::OK && asks_to_hang_up(settings) != asks_to_hang_up(m_settings)) {
        rc = change_modem_outputs(asks_to_hang_up(settings) ? 0 : MODEM_OUTPUTS);
    }

    if (rc == fs::OK) {
        m_settings = settings;
        m_line = line;
        take_read_settings(settings);
        take_character_time(line);
    }

    sync::mutex_unlock(m_control_lock);

    if (drains_first) {
        release_write_lock();
    }

    if (rc == fs::OK && drops_queued) {
        drop_queued();
    }

    return rc;
}

__PRIVILEGED_CODE int32_t port::flush(uint64_t which) {
    if (which > terminal::TCIOFLUSH) {
        return fs::ERR_INVAL;
    }

    if (which != terminal::TCOFLUSH) {
        drop_queued();
    }

    if (which != terminal::TCIFLUSH) {
        sync::mutex_lock(m_control_lock);
        cancel_started_send();
        sync::mutex_unlock(m_control_lock);
    }

    return fs::OK;
}

// FIONREAD answers with an int
__PRIVILEGED_CODE int32_t port::copy_queued_count_out(uint64_t arg) {
    int32_t queued = static_cast<int32_t>(queued_bytes());

    void* destination = reinterpret_cast<void*>(arg);
    return mm::uaccess::copy_to_user(destination, &queued, sizeof(queued)) == mm::uaccess::OK ? fs::OK : fs::ERR_INVAL;
}

// A port stores the size a program sets, 0 by 0 until then, so isatty() is true for it
__PRIVILEGED_CODE int32_t port::copy_window_size_out(uint64_t arg) {
    sync::mutex_lock(m_control_lock);
    terminal::winsize size = m_window_size;
    sync::mutex_unlock(m_control_lock);

    void* destination = reinterpret_cast<void*>(arg);
    return mm::uaccess::copy_to_user(destination, &size, sizeof(size)) == mm::uaccess::OK ? fs::OK : fs::ERR_INVAL;
}

__PRIVILEGED_CODE int32_t port::take_window_size(uint64_t arg) {
    terminal::winsize size = {};
    const void* source = reinterpret_cast<const void*>(arg);
    if (mm::uaccess::copy_from_user(&size, source, sizeof(size)) != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    sync::mutex_lock(m_control_lock);
    m_window_size = size;
    sync::mutex_unlock(m_control_lock);

    return fs::OK;
}

__PRIVILEGED_CODE int32_t port::set_exclusive(bool is_exclusive) {
    sync::mutex_lock(m_control_lock);
    m_is_exclusive = is_exclusive;
    sync::mutex_unlock(m_control_lock);

    return fs::OK;
}

__PRIVILEGED_CODE ssize_t port::read(void* out, size_t count, uint32_t flags, bool is_user_buffer) {
    bool may_wait = (flags & fs::O_NONBLOCK) == 0;
    uint64_t started_ns = clock::now_ns();
    uint8_t chunk[READ_CHUNK_BYTES];

    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    while (may_wait && !is_read_ready_locked(count) && !signals::interrupt_pending(sched::current())) {
        uint64_t deadline_ns = read_deadline_locked(started_ns);
        if (deadline_ns == 0) {
            irq = sync::wait(m_read_wq, m_state_lock, irq);
        } else if (clock::now_ns() < deadline_ns) {
            irq = sync::wait_until(m_read_wq, m_state_lock, irq, deadline_ns);
        } else {
            break;
        }
    }

    ssize_t empty_result = empty_read_result_locked(may_wait);
    size_t taken = pop_locked(chunk, min_size(count, READ_CHUNK_BYTES));
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    if (taken == 0) {
        return empty_result;
    }

    size_t done = 0;
    while (taken > 0) {
        if (!copy_out(static_cast<uint8_t*>(out) + done, chunk, taken, is_user_buffer)) {
            return done > 0 ? static_cast<ssize_t>(done) : fs::ERR_FAULT;
        }

        done += taken;
        irq = sync::spin_lock_irqsave(m_state_lock);
        taken = pop_locked(chunk, min_size(count - done, READ_CHUNK_BYTES));
        sync::spin_unlock_irqrestore(m_state_lock, irq);
    }

    return static_cast<ssize_t>(done);
}

__PRIVILEGED_CODE ssize_t port::write(const void* data, size_t count, uint32_t flags, bool is_user_buffer) {
    bool may_wait = (flags & fs::O_NONBLOCK) == 0;
    const auto* bytes = static_cast<const uint8_t*>(data);

    size_t written = 0;
    while (written < count) {
        size_t length = min_size(count - written, SEND_BYTES);
        ssize_t copied = start_send(bytes + written, length, may_wait, is_user_buffer);
        while (copied == fs::ERR_AGAIN && may_wait) {
            int32_t rc = wait_for_send_end(0);
            copied = rc == fs::OK ? start_send(bytes + written, length, true, is_user_buffer) : rc;
        }

        if (copied < 0) {
            return written > 0 ? static_cast<ssize_t>(written) : copied;
        }

        written += static_cast<size_t>(copied);
    }

    return static_cast<ssize_t>(written);
}

// Holds the write lock only from the copy into the staging buffer to the send's start, so no call waits behind a
// writer while the chip takes the bytes
__PRIVILEGED_CODE ssize_t port::start_send(const uint8_t* data, size_t length, bool may_wait, bool is_user_buffer) {
    if (may_wait) {
        sync::mutex_lock(m_write_lock);
    } else if (!sync::mutex_trylock(m_write_lock)) {
        return fs::ERR_AGAIN;
    }

    ssize_t result = static_cast<ssize_t>(length);
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    bool logs_failure = m_has_send_failed && !m_is_send_failure_logged;
    if (m_is_gone || m_has_send_failed || !m_driver) {
        result = fs::ERR_IO;
    } else if (m_send_length > 0 || m_is_cancelling) {
        result = fs::ERR_AGAIN;
    }

    m_is_send_failure_logged = m_is_send_failure_logged || logs_failure;
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    if (logs_failure) {
        log::warn("serial: a send to /dev/serial/%s failed, so its writes fail until it is unplugged", name());
    }

    if (result > 0 && !copy_in(m_staging, data, length, is_user_buffer)) {
        result = fs::ERR_FAULT;
    }

    // Marked in flight before the call, since the send can end before the call returns
    if (result > 0) {
        irq = sync::spin_lock_irqsave(m_state_lock);
        m_send_length = length;
        sync::spin_unlock_irqrestore(m_state_lock, irq);

        bool is_started = m_driver->send(m_staging, length) == OK;

        irq = sync::spin_lock_irqsave(m_state_lock);
        if (is_started) {
            m_is_send_started = m_send_length > 0;
        } else {
            m_send_length = 0;
            result = fs::ERR_IO;
        }

        sync::spin_unlock_irqrestore(m_state_lock, irq);
    }

    release_write_lock();
    return result;
}

// Waits for the send in flight to end, and for a cancel to finish, taking only the state lock. A signal ends the wait,
// and so does `deadline_ns` unless it is 0
__PRIVILEGED_CODE int32_t port::wait_for_send_end(uint64_t deadline_ns) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    while ((m_send_length > 0 || m_is_cancelling) && !m_is_gone && !m_has_send_failed &&
           !signals::interrupt_pending(sched::current())) {
        if (deadline_ns == 0) {
            irq = sync::wait(m_send_wq, m_state_lock, irq);
        } else if (clock::now_ns() < deadline_ns) {
            irq = sync::wait_until(m_send_wq, m_state_lock, irq, deadline_ns);
        } else {
            break;
        }
    }

    int32_t rc = fs::OK;
    if (m_is_gone || m_has_send_failed) {
        rc = fs::ERR_IO;
    } else if (m_send_length > 0 || m_is_cancelling) {
        rc = signals::interrupt_pending(sched::current()) ? fs::ERR_INTR : fs::ERR_IO;
    }

    sync::spin_unlock_irqrestore(m_state_lock, irq);
    return rc;
}

// Counts the bytes the chip took and may still hold as well as the send's, so a chip still sending them is waited for
__PRIVILEGED_CODE uint64_t port::drain_deadline() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    uint64_t now_ns = clock::now_ns();
    uint64_t held_ns = m_chip_empty_ns > now_ns ? m_chip_empty_ns - now_ns : 0;
    uint64_t deadline_ns = now_ns + m_send_length * m_character_ns + held_ns + DRAIN_DEADLINE_MARGIN_NS;
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    return deadline_ns;
}

// The caller must hold the write lock. Waits for the send, then while the chip holds bytes, up to the deadline, and
// with `waits_for_last_character` one character more for the byte a chip's count leaves out, if it may be leaving
__PRIVILEGED_CODE int32_t port::drain(bool waits_for_last_character) {
    uint64_t deadline_ns = drain_deadline();
    int32_t rc = wait_for_send_end(deadline_ns);
    bool has_held_bytes = false;
    ssize_t pending = 1;
    while (rc == fs::OK && pending > 0) {
        pending = chip_tx_pending();
        if (pending == TX_UNKNOWN) {
            rc = sleep_until(min_ns(chip_empty_ns(TX_UNKNOWN), deadline_ns));
            pending = 0;
        } else if (pending < 0 || (pending > 0 && clock::now_ns() >= deadline_ns)) {
            rc = fs::ERR_IO;
        } else if (pending > 0) {
            has_held_bytes = true;
            rc = sleep_until(min_ns(chip_empty_ns(pending), deadline_ns));
        }
    }

    bool may_be_sending = has_held_bytes || is_chip_sending();
    if (rc == fs::OK && waits_for_last_character && may_be_sending) {
        rc = sleep_until(min_ns(chip_empty_ns(1), deadline_ns));
    }

    return rc;
}

__PRIVILEGED_CODE ssize_t port::chip_tx_pending() {
    sync::mutex_lock(m_control_lock);
    ssize_t pending = m_driver ? m_driver->tx_pending() : ERR_IO;
    sync::mutex_unlock(m_control_lock);

    return pending;
}

// When the chip should have sent what it holds, or for a chip that cannot tell, every byte it took
__PRIVILEGED_CODE uint64_t port::chip_empty_ns(ssize_t pending) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    uint64_t empty_ns = pending == TX_UNKNOWN ? m_chip_empty_ns
                                              : clock::now_ns() + static_cast<uint64_t>(pending) * m_character_ns;
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    return empty_ns;
}

// True until the bytes the chip took have left it at the line's speed
__PRIVILEGED_CODE bool port::is_chip_sending() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    bool is_sending = m_chip_empty_ns > clock::now_ns();
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    return is_sending;
}

// Sleeps until `wake_ns`, or until a signal or an unplug, taking only the state lock, so control calls go on meanwhile
__PRIVILEGED_CODE int32_t port::sleep_until(uint64_t wake_ns) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    while (clock::now_ns() < wake_ns && !m_is_gone && !signals::interrupt_pending(sched::current())) {
        irq = sync::wait_until(m_send_wq, m_state_lock, irq, wake_ns);
    }

    int32_t rc = fs::OK;
    if (m_is_gone) {
        rc = fs::ERR_IO;
    } else if (clock::now_ns() < wake_ns) {
        rc = fs::ERR_INTR;
    }

    sync::spin_unlock_irqrestore(m_state_lock, irq);
    return rc;
}

// Holds the write lock through the drain and the break, so no send starts before the break ends. A `break_ns` of 0
// only drains
__PRIVILEGED_CODE int32_t port::drain_then_break(uint64_t break_ns) {
    sync::mutex_lock(m_write_lock);
    int32_t rc = drain(true);
    if (rc == fs::OK && break_ns > 0) {
        rc = hold_break(break_ns);
    }

    release_write_lock();
    return rc;
}

// Takes the control lock for each chip request rather than across the break, so modem lines and settings can change
// during it. A signal ends the break early, and the request ending it still goes out
__PRIVILEGED_CODE int32_t port::hold_break(uint64_t break_ns) {
    int32_t rc = set_break_request(true);
    if (rc != fs::OK) {
        return rc;
    }

    int32_t slept = sleep_until(clock::now_ns() + break_ns);
    rc = set_break_request(false);
    return slept != fs::OK ? slept : rc;
}

__PRIVILEGED_CODE int32_t port::set_break_request(bool on) {
    sync::mutex_lock(m_control_lock);
    int32_t rc = change_break(on);
    sync::mutex_unlock(m_control_lock);

    return rc;
}

// TIOCMGET gives an int of TIOCM bits: DTR and RTS as last set, and the chip's inputs
__PRIVILEGED_CODE int32_t port::copy_modem_lines_out(uint64_t arg) {
    sync::mutex_lock(m_control_lock);
    uint32_t inputs = 0;
    int32_t rc = m_driver ? driver_result_to_fs(m_driver->get_modem_inputs(&inputs)) : fs::ERR_IO;
    int32_t lines = static_cast<int32_t>(m_modem_outputs | (inputs & MODEM_INPUTS));
    sync::mutex_unlock(m_control_lock);

    if (rc != fs::OK) {
        return rc;
    }

    void* destination = reinterpret_cast<void*>(arg);
    return mm::uaccess::copy_to_user(destination, &lines, sizeof(lines)) == mm::uaccess::OK ? fs::OK : fs::ERR_INVAL;
}

// TIOCMSET, TIOCMBIS and TIOCMBIC take an int of TIOCM bits, of which the port changes DTR and RTS in one request
__PRIVILEGED_CODE int32_t port::take_modem_outputs(uint32_t cmd, uint64_t arg) {
    int32_t asked = 0;
    const void* source = reinterpret_cast<const void*>(arg);
    if (mm::uaccess::copy_from_user(&asked, source, sizeof(asked)) != mm::uaccess::OK) {
        return fs::ERR_INVAL;
    }

    sync::mutex_lock(m_control_lock);
    uint32_t lines = static_cast<uint32_t>(asked);
    if (cmd == terminal::TIOCMBIS) {
        lines = m_modem_outputs | lines;
    } else if (cmd == terminal::TIOCMBIC) {
        lines = m_modem_outputs & ~lines;
    }

    int32_t rc = change_modem_outputs(lines & MODEM_OUTPUTS);
    sync::mutex_unlock(m_control_lock);

    return rc;
}

// The caller must hold the control lock
__PRIVILEGED_CODE int32_t port::change_modem_outputs(uint32_t lines) {
    int32_t rc = m_driver ? driver_result_to_fs(m_driver->set_modem_outputs(lines)) : fs::ERR_IO;
    if (rc == fs::OK) {
        m_modem_outputs = lines;
    }

    return rc;
}

// The caller must hold the control lock
__PRIVILEGED_CODE int32_t port::change_break(bool on) {
    int32_t rc = m_driver ? driver_result_to_fs(m_driver->set_break(on)) : fs::ERR_IO;
    if (rc == fs::OK) {
        m_is_breaking = on;
    }

    return rc;
}

// Releasing the write lock wakes pollers, since a free lock can make the port writable
__PRIVILEGED_CODE void port::release_write_lock() {
    sync::mutex_unlock(m_write_lock);
    sync::wake_all(m_send_wq);
}

// The caller must hold the control lock. No send starts while the cancel runs, and a send whose driver call has not
// returned is waited for, since that call never waits for the chip, so the cancel reaches every send in flight
__PRIVILEGED_CODE void port::cancel_started_send() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    m_is_cancelling = m_send_length > 0 && m_driver;
    while (m_is_cancelling && m_send_length > 0 && !m_is_send_started) {
        irq = sync::wait_uninterruptible(m_send_wq, m_state_lock, irq);
    }

    bool is_cancelling = m_is_cancelling;
    bool is_send_started = m_is_send_started;
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    if (!is_cancelling) {
        return;
    }

    if (is_send_started) {
        m_driver->cancel_send();
    }

    irq = sync::spin_lock_irqsave(m_state_lock);
    m_is_cancelling = false;
    sync::wake_all(m_send_wq);
    sync::spin_unlock_irqrestore(m_state_lock, irq);
}

// TIOCOUTQ gives an int
__PRIVILEGED_CODE int32_t port::copy_send_length_out(uint64_t arg) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    int32_t length = static_cast<int32_t>(m_send_length);
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    void* destination = reinterpret_cast<void*>(arg);
    return mm::uaccess::copy_to_user(destination, &length, sizeof(length)) == mm::uaccess::OK ? fs::OK : fs::ERR_INVAL;
}

__PRIVILEGED_CODE uint32_t port::poll(sync::poll_table* pt) {
    if (pt) {
        sync::poll_subscribe(*pt, m_read_wq);
        sync::poll_subscribe(*pt, m_send_wq);
    }

    // A failed stream only makes reads fail, so the port reports readable rather than an error, until an unplug
    // turns those reads into end of file
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    bool is_readable = m_queued >= min_readable_bytes_locked() || (m_has_failed && !m_is_gone);
    bool is_gone = m_is_gone;

    // A failed send or an unplug only makes writes fail, so the port reports writable rather than an error
    bool can_start_send = m_send_length == 0 && !m_is_cancelling && !sync::mutex_is_locked(m_write_lock);
    bool is_writable = can_start_send || m_is_gone || m_has_send_failed;
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    return (is_readable ? sync::POLL_IN : 0) | (is_writable ? sync::POLL_OUT : 0) | (is_gone ? sync::POLL_HUP : 0);
}

__PRIVILEGED_CODE size_t port::queued_bytes() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    size_t queued = m_queued;
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    return queued;
}

__PRIVILEGED_CODE void port::queue_received(const uint8_t* data, size_t len) {
    m_received_bytes.fetch_add_relaxed(len);

    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    if (m_open_files > 0 && !m_is_gone && m_is_receiving) {
        push_locked(data, len);
        m_last_receive_ns = clock::now_ns();
        sync::wake_all(m_read_wq);
    }

    sync::spin_unlock_irqrestore(m_state_lock, irq);
}

__PRIVILEGED_CODE void port::mark_failed() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    m_has_failed = true;
    sync::wake_all(m_read_wq);
    sync::spin_unlock_irqrestore(m_state_lock, irq);
}

__PRIVILEGED_CODE void port::mark_gone() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    m_is_gone = true;
    sync::wake_all(m_read_wq);
    sync::wake_all(m_send_wq);
    sync::spin_unlock_irqrestore(m_state_lock, irq);
}

__PRIVILEGED_CODE void port::end_send(ssize_t taken_or_error) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    if (taken_or_error < 0) {
        m_has_send_failed = true;
    } else {
        // The chip sends what it took at the line's speed, after what it already held
        uint64_t now_ns = clock::now_ns();
        uint64_t from_ns = m_chip_empty_ns > now_ns ? m_chip_empty_ns : now_ns;
        m_chip_empty_ns = from_ns + static_cast<uint64_t>(taken_or_error) * m_character_ns;
    }

    m_send_length = 0;
    m_is_send_started = false;
    sync::wake_all(m_send_wq);
    sync::spin_unlock_irqrestore(m_state_lock, irq);
}

__PRIVILEGED_CODE void port::take_character_time(const line_settings& line) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    m_character_ns = character_ns(line);
    sync::spin_unlock_irqrestore(m_state_lock, irq);
}

__PRIVILEGED_CODE void port::take_read_settings(const terminal::termios2& settings) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    m_read_min = settings.c_cc[terminal::VMIN];
    m_read_timeout_ns = settings.c_cc[terminal::VTIME] * NS_PER_TENTH;
    m_is_receiving = (settings.c_cflag & terminal::CREAD) != 0;
    sync::wake_all(m_read_wq);
    sync::spin_unlock_irqrestore(m_state_lock, irq);
}

bool port::is_read_ready_locked(size_t count) const {
    if (m_is_gone || m_has_failed) {
        return true;
    }

    // With VMIN and VTIME both 0 a read returns what is queued at once, possibly nothing
    if (m_read_min == 0) {
        return m_queued > 0 || m_read_timeout_ns == 0;
    }

    return m_queued >= min_size(m_read_min, count);
}

size_t port::min_readable_bytes_locked() const {
    return (m_read_timeout_ns == 0 && m_read_min > 1) ? m_read_min : 1;
}

uint64_t port::read_deadline_locked(uint64_t started_ns) const {
    if (m_read_timeout_ns == 0) {
        return 0;
    }

    if (m_read_min == 0) {
        return started_ns + m_read_timeout_ns;
    }

    return m_queued > 0 ? m_last_receive_ns + m_read_timeout_ns : 0;
}

// A blocking read ends with nothing queued on a signal, or with VMIN 0 once VTIME passes
ssize_t port::empty_read_result_locked(bool may_wait) const {
    if (m_is_gone) {
        return 0;
    }

    if (m_has_failed) {
        return fs::ERR_IO;
    }

    if (!may_wait) {
        return fs::ERR_AGAIN;
    }

    return signals::interrupt_pending(sched::current()) ? fs::ERR_INTR : 0;
}

// Copies as many of `len` bytes as fit after the queued ones, dropping and counting the rest
void port::push_locked(const uint8_t* data, size_t len) {
    size_t taken = min_size(len, RECEIVE_QUEUE_BYTES - m_queued);
    size_t tail = (m_queue_head + m_queued) % RECEIVE_QUEUE_BYTES;
    size_t before_wrap = min_size(taken, RECEIVE_QUEUE_BYTES - tail);

    string::memcpy(m_queue + tail, data, before_wrap);
    string::memcpy(m_queue, data + before_wrap, taken - before_wrap);
    m_queued += taken;
    m_dropped_bytes.fetch_add_release(len - taken);
}

// Moves up to `len` of the oldest queued bytes into `out`
size_t port::pop_locked(uint8_t* out, size_t len) {
    size_t taken = min_size(len, m_queued);
    size_t before_wrap = min_size(taken, RECEIVE_QUEUE_BYTES - m_queue_head);

    string::memcpy(out, m_queue + m_queue_head, before_wrap);
    string::memcpy(out + before_wrap, m_queue, taken - before_wrap);
    m_queue_head = (m_queue_head + taken) % RECEIVE_QUEUE_BYTES;
    m_queued -= taken;

    return taken;
}

} // namespace serial
