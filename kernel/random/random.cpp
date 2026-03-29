#include "random/random.h"
#include "hw/rng.h"
#include "fs/node.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/devfs/devfs.h"
#include "mm/heap.h"
#include "common/logging.h"

namespace random {

namespace {

class urandom_node : public fs::node {
public:
    urandom_node(fs::instance* fs, const char* name)
        : fs::node(fs::node_type::char_device, fs, name) {}

    ssize_t read(fs::file*, void* buf, size_t count) override {
        int32_t rc = random::fill(buf, count);
        if (rc != OK) {
            return fs::ERR_IO;
        }
        return static_cast<ssize_t>(count);
    }

    int32_t getattr(fs::vattr* attr) override {
        if (!attr) return fs::ERR_INVAL;
        attr->type = fs::node_type::char_device;
        attr->size = 0;
        return fs::OK;
    }
};

} // anonymous namespace

int32_t fill(void* buf, size_t len) {
    if (!hw::rng::available()) {
        return ERR_NOSRC;
    }
    if (!hw::rng::fill(buf, len)) {
        return ERR_NOSRC;
    }
    return OK;
}

__PRIVILEGED_CODE int32_t init() {
    if (!hw::rng::available()) {
        log::warn("random: no hardware RNG detected");
        return ERR_NOSRC;
    }

    log::info("random: hardware RNG available");

    void* urandom_mem = heap::kzalloc(sizeof(urandom_node));
    if (!urandom_mem) {
        log::error("random: failed to allocate urandom node");
        return ERR_NOSRC;
    }
    auto* unode = new (urandom_mem) urandom_node(nullptr, "urandom");

    if (devfs::add_char_device("urandom", unode) != devfs::OK) {
        log::error("random: failed to register /dev/urandom");
        unode->~urandom_node();
        heap::kfree(urandom_mem);
        return ERR_NOSRC;
    }

    void* random_mem = heap::kzalloc(sizeof(urandom_node));
    if (!random_mem) {
        log::warn("random: failed to allocate random node, /dev/random unavailable");
        return OK;
    }
    auto* rnode = new (random_mem) urandom_node(nullptr, "random");

    if (devfs::add_char_device("random", rnode) != devfs::OK) {
        log::warn("random: failed to register /dev/random");
        rnode->~urandom_node();
        heap::kfree(random_mem);
    }

    return OK;
}

} // namespace random
