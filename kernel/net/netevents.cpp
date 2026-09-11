#include "net/netevents.h"
#include "net/interface.h"
#include "fs/node.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/devfs/devfs.h"
#include "mm/heap.h"
#include "sync/poll.h"
#include "common/string.h"
#include "common/logging.h"

namespace net {
namespace netevents {

// Room for the longest generation in decimal and its line break
constexpr size_t LINE_CAP = 21;

// What one open file has reported so far
struct watch {
    uint64_t seen;
};

class events_node : public fs::node {
public:
    events_node() : fs::node(fs::node_type::char_device, nullptr, "events") {}

    int32_t open(fs::file* f, uint32_t flags) override;
    int32_t on_close(fs::file* f) override;
    ssize_t read(fs::file* f, void* buf, size_t count) override;
    uint32_t poll(fs::file* f, sync::poll_table* pt) override;
};

static watch* watch_of(fs::file* f) {
    return f ? static_cast<watch*>(f->private_data()) : nullptr;
}

// The cursor belongs to the file, which lives in unprivileged memory
int32_t events_node::open(fs::file* f, uint32_t) {
    auto* w = static_cast<watch*>(heap::uzalloc(sizeof(watch)));
    if (!w) {
        return fs::ERR_NOMEM;
    }

    f->set_private_data(w);
    return fs::OK;
}

int32_t events_node::on_close(fs::file* f) {
    watch* w = watch_of(f);
    if (w) {
        f->set_private_data(nullptr);
        heap::ufree(w);
    }

    return fs::OK;
}

ssize_t events_node::read(fs::file* f, void* buf, size_t count) {
    watch* w = watch_of(f);
    if (!w || !buf) {
        return fs::ERR_BADF;
    }

    uint64_t generation = status_generation();
    if (w->seen == generation) {
        return 0;
    }

    char line[LINE_CAP];
    size_t len = string::format_u64(line, sizeof(line) - 1, generation);
    line[len++] = '\n';
    if (count < len) {
        return fs::ERR_INVAL;
    }

    string::memcpy(buf, line, len);
    w->seen = generation;
    return static_cast<ssize_t>(len);
}

uint32_t events_node::poll(fs::file* f, sync::poll_table* pt) {
    if (pt) {
        watch_status(*pt);
    }

    watch* w = watch_of(f);
    return w && w->seen != status_generation() ? sync::POLL_IN : 0;
}

__PRIVILEGED_CODE int32_t init() {
    fs::node* dir = devfs::ensure_dir("net");
    if (!dir) {
        log::error("netevents: failed to create /dev/net");
        return ERR_NO_MEMORY;
    }

    void* mem = heap::kzalloc(sizeof(events_node));
    if (!mem) {
        log::error("netevents: failed to allocate /dev/net/events");
        return ERR_NO_MEMORY;
    }

    auto* node = new (mem) events_node();
    if (devfs::add_char_device_at(dir, node) != devfs::OK) {
        log::error("netevents: failed to register /dev/net/events");
        node->~events_node();
        heap::kfree(mem);
        return ERR_INVALID;
    }

    return OK;
}

} // namespace netevents
} // namespace net
