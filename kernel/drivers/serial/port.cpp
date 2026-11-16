#include "drivers/serial/port.h"
#include "drivers/serial/settings.h"
#include "clock/clock.h"
#include "fs/fs.h"
#include "fs/node.h"
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

static constexpr uint64_t NS_PER_VTIME = 100000000;

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

    if (!p->copy_identity(id)) {
        heap::ufree_delete(p);
        return nullptr;
    }

    p->m_queue = static_cast<uint8_t*>(heap::ualloc(RECEIVE_QUEUE_BYTES));
    if (!p->m_queue) {
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
    m_read_wq.init();
}

port::~port() {
    heap::ufree(m_queue);
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
        rc = driver_result_to_fs(m_driver->set_line(m_line));
    }

    // The device can leave during the first open's chip request, and then the open fails rather than count a file
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

__PRIVILEGED_CODE void port::close() {
    sync::mutex_lock(m_control_lock);
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    m_open_files--;
    bool is_last = m_open_files == 0;
    if (is_last) {
        m_queue_head = 0;
        m_queued = 0;
    }

    sync::spin_unlock_irqrestore(m_state_lock, irq);

    if (is_last) {
        m_is_exclusive = false;
    }

    sync::mutex_unlock(m_control_lock);
}

__PRIVILEGED_CODE int32_t port::request(uint32_t cmd, uint64_t arg) {
    if (cmd == terminal::FIONREAD) {
        return copy_queued_count_out(arg);
    }

    if (is_gone()) {
        return fs::ERR_IO;
    }

    switch (cmd) {
        case terminal::TCGETS:     return copy_settings_out(arg, sizeof(terminal::termios));
        case terminal::TCGETS2:    return copy_settings_out(arg, sizeof(terminal::termios2));
        case terminal::TCSETS:
        case terminal::TCSETSW:    return apply_settings(arg, sizeof(terminal::termios), false);
        case terminal::TCSETSF:    return apply_settings(arg, sizeof(terminal::termios), true);
        case terminal::TCSETS2:
        case terminal::TCSETSW2:   return apply_settings(arg, sizeof(terminal::termios2), false);
        case terminal::TCSETSF2:   return apply_settings(arg, sizeof(terminal::termios2), true);
        case terminal::TCFLSH:     return flush(arg);
        case terminal::TIOCGWINSZ: return copy_window_size_out(arg);
        case terminal::TIOCSWINSZ: return take_window_size(arg);
        case terminal::TIOCEXCL:   return set_exclusive(true);
        case terminal::TIOCNXCL:   return set_exclusive(false);
        case terminal::TCXONC:     return fs::ERR_INVAL;
        default:                   return fs::ERR_NOSYS;
    }
}

__PRIVILEGED_CODE void port::release_driver() {
    sync::mutex_lock(m_control_lock);
    m_driver = nullptr;
    sync::mutex_unlock(m_control_lock);
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

// Stores the settings only once the chip takes them, so TCGETS returns what the last applied TCSETS wrote
__PRIVILEGED_CODE int32_t port::apply_settings(uint64_t arg, size_t length, bool drops_queued) {
    terminal::termios2 written = {};
    const void* source = reinterpret_cast<const void*>(arg);
    if (mm::uaccess::copy_from_user(&written, source, length) != mm::uaccess::OK) {
        return fs::ERR_INVAL;
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

    if (rc == fs::OK) {
        m_settings = settings;
        m_line = line;
        take_read_settings(settings);
    }

    sync::mutex_unlock(m_control_lock);

    if (rc == fs::OK && drops_queued) {
        drop_queued();
    }

    return rc;
}

__PRIVILEGED_CODE int32_t port::flush(uint64_t which) {
    switch (which) {
        case terminal::TCIFLUSH:
        case terminal::TCIOFLUSH:
            drop_queued();
            return fs::OK;
        case terminal::TCOFLUSH:
            return fs::OK;
        default:
            return fs::ERR_INVAL;
    }
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

__PRIVILEGED_CODE uint32_t port::poll(sync::poll_table* pt) {
    if (pt) {
        sync::poll_subscribe(*pt, m_read_wq);
    }

    // A failed stream only makes reads fail, so the port reports readable rather than an error, until an unplug
    // turns those reads into end of file
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    bool is_readable = m_queued >= min_readable_bytes_locked() || (m_has_failed && !m_is_gone);
    bool is_gone = m_is_gone;
    sync::spin_unlock_irqrestore(m_state_lock, irq);

    return (is_readable ? sync::POLL_IN : 0) | (is_gone ? sync::POLL_HUP : 0);
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
    sync::spin_unlock_irqrestore(m_state_lock, irq);
}

__PRIVILEGED_CODE void port::take_read_settings(const terminal::termios2& settings) {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    m_read_min = settings.c_cc[terminal::VMIN];
    m_read_timeout_ns = settings.c_cc[terminal::VTIME] * NS_PER_VTIME;
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
