#include "drivers/serial/port.h"
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
        if (cmd != terminal::FIONREAD) {
            return fs::ERR_NOSYS;
        }

        int32_t queued = static_cast<int32_t>(m_port->queued_bytes());
        void* out = reinterpret_cast<void*>(arg);
        return mm::uaccess::copy_to_user(out, &queued, sizeof(queued)) == mm::uaccess::OK ? fs::OK : fs::ERR_INVAL;
    }

private:
    port* m_port;
};

} // namespace

static size_t min_size(size_t a, size_t b) {
    return a < b ? a : b;
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

__PRIVILEGED_CODE port* port::create(const identity& id) {
    auto* p = heap::ualloc_new<port>();
    if (!p) {
        return nullptr;
    }

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

__PRIVILEGED_CODE int32_t port::open() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    bool is_gone = m_is_gone;
    if (!is_gone) {
        m_open_files++;
    }

    sync::spin_unlock_irqrestore(m_state_lock, irq);

    return is_gone ? fs::ERR_IO : fs::OK;
}

__PRIVILEGED_CODE void port::close() {
    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    m_open_files--;
    if (m_open_files == 0) {
        m_queue_head = 0;
        m_queued = 0;
    }

    sync::spin_unlock_irqrestore(m_state_lock, irq);
}

__PRIVILEGED_CODE ssize_t port::read(void* out, size_t count, uint32_t flags, bool is_user_buffer) {
    bool may_wait = (flags & fs::O_NONBLOCK) == 0;
    uint8_t chunk[READ_CHUNK_BYTES];

    sync::irq_state irq = sync::spin_lock_irqsave(m_state_lock);
    while (!has_read_result_locked() && may_wait && !signals::interrupt_pending(sched::current())) {
        irq = sync::wait(m_read_wq, m_state_lock, irq);
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
    bool is_readable = m_queued > 0 || (m_has_failed && !m_is_gone);
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
    if (m_open_files > 0 && !m_is_gone) {
        push_locked(data, len);
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

bool port::has_read_result_locked() const {
    return m_queued > 0 || m_is_gone || m_has_failed;
}

ssize_t port::empty_read_result_locked(bool may_wait) const {
    if (m_is_gone) {
        return 0;
    }

    if (m_has_failed) {
        return fs::ERR_IO;
    }

    return may_wait ? fs::ERR_INTR : fs::ERR_AGAIN;
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
