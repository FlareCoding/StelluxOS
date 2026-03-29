#include "drivers/input/input.h"
#include "fs/node.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/devfs/devfs.h"
#include "common/ring_buffer.h"
#include "common/logging.h"
#include "mm/heap.h"
#include "dynpriv/dynpriv.h"
#include "sync/poll.h"

namespace input {

namespace {

constexpr size_t KBD_RING_CAPACITY   = 4096;
constexpr size_t MOUSE_RING_CAPACITY = 4096;

__PRIVILEGED_BSS ring_buffer* g_kbd_rb;
__PRIVILEGED_BSS ring_buffer* g_mouse_rb;
__PRIVILEGED_BSS uint32_t     g_kbd_drops;
__PRIVILEGED_BSS uint32_t     g_mouse_drops;

template <typename Event>
class input_device_node : public fs::node {
public:
    input_device_node(fs::instance* fs, const char* name, ring_buffer* rb)
        : fs::node(fs::node_type::char_device, fs, name)
        , m_rb(rb) {
    }

    ssize_t read(fs::file* f, void* buf, size_t count) override {
        constexpr size_t rec_size = sizeof(Event);
        if (count < rec_size || (count % rec_size) != 0) {
            return fs::ERR_INVAL;
        }

        bool nonblock = (f->flags() & fs::O_NONBLOCK) != 0;
        ssize_t rc = ring_buffer_read(m_rb, static_cast<uint8_t*>(buf),
                                      count, nonblock);
        if (rc == RB_ERR_AGAIN) {
            return fs::ERR_AGAIN;
        }
        if (rc < 0) {
            return fs::ERR_IO;
        }
        size_t got = static_cast<size_t>(rc);
        size_t whole = (got / rec_size) * rec_size;
        return static_cast<ssize_t>(whole > 0 ? whole : 0);
    }

    uint32_t poll(fs::file*, sync::poll_table* pt) override {
        sync::poll_entry entry = {};
        return ring_buffer_poll_read(m_rb, pt, &entry);
    }

    int32_t getattr(fs::vattr* attr) override {
        if (!attr) {
            return fs::ERR_INVAL;
        }
        attr->type = fs::node_type::char_device;
        attr->size = 0;
        return fs::OK;
    }

private:
    ring_buffer* m_rb;
};

using kbd_device_node   = input_device_node<kbd_event>;
using mouse_device_node = input_device_node<mouse_event>;

} // anonymous namespace

__PRIVILEGED_CODE int32_t init() {
    g_kbd_rb = ring_buffer_create(KBD_RING_CAPACITY);
    if (!g_kbd_rb) {
        log::error("input: failed to create keyboard ring buffer");
        return ERR;
    }

    g_mouse_rb = ring_buffer_create(MOUSE_RING_CAPACITY);
    if (!g_mouse_rb) {
        log::error("input: failed to create mouse ring buffer");
        ring_buffer_destroy(g_kbd_rb);
        g_kbd_rb = nullptr;
        return ERR;
    }

    fs::node* input_dir = devfs::ensure_dir("input");
    if (!input_dir) {
        log::error("input: failed to create /dev/input directory");
        ring_buffer_destroy(g_mouse_rb);
        ring_buffer_destroy(g_kbd_rb);
        g_mouse_rb = nullptr;
        g_kbd_rb = nullptr;
        return ERR;
    }

    void* kbd_mem = heap::kzalloc(sizeof(kbd_device_node));
    if (!kbd_mem) {
        log::error("input: failed to allocate kbd node");
        ring_buffer_destroy(g_mouse_rb);
        ring_buffer_destroy(g_kbd_rb);
        g_mouse_rb = nullptr;
        g_kbd_rb = nullptr;
        return ERR;
    }
    auto* kbd_node = new (kbd_mem) kbd_device_node(nullptr, "kbd", g_kbd_rb);

    void* mouse_mem = heap::kzalloc(sizeof(mouse_device_node));
    if (!mouse_mem) {
        log::error("input: failed to allocate mouse node");
        kbd_node->~kbd_device_node();
        heap::kfree(kbd_mem);
        ring_buffer_destroy(g_mouse_rb);
        ring_buffer_destroy(g_kbd_rb);
        g_mouse_rb = nullptr;
        g_kbd_rb = nullptr;
        return ERR;
    }
    auto* mouse_node = new (mouse_mem) mouse_device_node(nullptr, "mouse", g_mouse_rb);

    if (devfs::add_char_device_at(input_dir, kbd_node) != devfs::OK) {
        log::error("input: failed to register /dev/input/kbd");
        mouse_node->~mouse_device_node();
        heap::kfree(mouse_mem);
        kbd_node->~kbd_device_node();
        heap::kfree(kbd_mem);
        ring_buffer_destroy(g_mouse_rb);
        ring_buffer_destroy(g_kbd_rb);
        g_mouse_rb = nullptr;
        g_kbd_rb = nullptr;
        return ERR;
    }

    if (devfs::add_char_device_at(input_dir, mouse_node) != devfs::OK) {
        log::error("input: failed to register /dev/input/mouse");
        mouse_node->~mouse_device_node();
        heap::kfree(mouse_mem);
        ring_buffer_destroy(g_mouse_rb);
        ring_buffer_destroy(g_kbd_rb);
        g_mouse_rb = nullptr;
        g_kbd_rb = nullptr;
        return ERR;
    }

    log::info("input: registered /dev/input/kbd and /dev/input/mouse");
    return OK;
}

int32_t push_kbd_event(const kbd_event& evt) {
    int32_t result = 0;
    RUN_ELEVATED({
        if (!g_kbd_rb) {
            result = 0;
        } else {
            ssize_t rc = ring_buffer_write(g_kbd_rb,
                                           reinterpret_cast<const uint8_t*>(&evt),
                                           sizeof(evt), true);
            if (rc < 0 || static_cast<size_t>(rc) < sizeof(evt)) {
                g_kbd_drops++;
                result = 0;
            } else {
                result = 1;
            }
        }
    });
    return result;
}

int32_t push_mouse_event(const mouse_event& evt) {
    int32_t result = 0;
    RUN_ELEVATED({
        if (!g_mouse_rb) {
            result = 0;
        } else {
            ssize_t rc = ring_buffer_write(g_mouse_rb,
                                           reinterpret_cast<const uint8_t*>(&evt),
                                           sizeof(evt), true);
            if (rc < 0 || static_cast<size_t>(rc) < sizeof(evt)) {
                g_mouse_drops++;
                result = 0;
            } else {
                result = 1;
            }
        }
    });
    return result;
}

} // namespace input
