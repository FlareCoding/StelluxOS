#include "trace/trace.h"
#include "fs/fs.h"
#include "fs/node.h"
#include "fs/file.h"
#include "fs/devfs/devfs.h"
#include "mm/heap.h"

namespace trace {

class ktrace_node : public fs::node {
public:
    ktrace_node(fs::instance* fs, const char* name)
        : fs::node(fs::node_type::char_device, fs, name) {}

    // O_RDONLY opens a dump session (freeze + snapshot), other modes are control-only
    int32_t open(fs::file* f, uint32_t flags) override {
        if ((flags & fs::ACCESS_MODE_MASK) != fs::O_RDONLY) {
            return fs::OK;
        }
        if (begin_dump() != OK) {
            return fs::ERR_BUSY;
        }
        f->set_offset(0);
        return fs::OK;
    }

    int32_t on_close(fs::file* f) override {
        if ((f->flags() & fs::ACCESS_MODE_MASK) == fs::O_RDONLY) {
            end_dump();
        }
        return fs::OK;
    }

    ssize_t read(fs::file* f, void* buf, size_t count) override {
        uint64_t off = static_cast<uint64_t>(f->offset());
        size_t n = dump_read(off, buf, count);
        f->set_offset(static_cast<int64_t>(off + n));
        return static_cast<ssize_t>(n);
    }

    int32_t ioctl(fs::file*, uint32_t cmd, uint64_t arg) override {
        switch (cmd) {
        case KTRACE_IOCTL_SET_CATEGORIES:
            set_enabled_categories(static_cast<category>(arg));
            return fs::OK;
        case KTRACE_IOCTL_RESET:
            reset_buffers();
            return fs::OK;
        default:
            return fs::ERR_INVAL;
        }
    }

    int32_t getattr(fs::vattr* attr) override {
        if (!attr) return fs::ERR_INVAL;
        attr->type = fs::node_type::char_device;
        attr->size = dump_size();
        return fs::OK;
    }
};

__PRIVILEGED_CODE int32_t register_device() {
    void* mem = heap::kzalloc(sizeof(ktrace_node));
    if (!mem) {
        return ERR_NO_MEM;
    }
    auto* knode = new (mem) ktrace_node(nullptr, "ktrace");
    if (devfs::add_char_device("ktrace", knode) != devfs::OK) {
        knode->~ktrace_node();
        heap::kfree(mem);
        return ERR_IO;
    }
    return OK;
}

} // namespace trace
