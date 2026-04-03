#include "random/random.h"
#include "hw/rng.h"
#include "fs/node.h"
#include "fs/file.h"
#include "fs/fs.h"
#include "fs/devfs/devfs.h"
#include "mm/heap.h"
#include "common/logging.h"
#include "clock/clock.h"

namespace random {

namespace {

// xoshiro256** software PRNG - used as fallback when no hardware RNG exists.

uint64_t g_sw_state[4];
bool g_use_sw_prng = false;

inline uint64_t rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

uint64_t xoshiro256ss() {
    uint64_t result = rotl(g_sw_state[1] * 5, 7) * 9;
    uint64_t t = g_sw_state[1] << 17;
    g_sw_state[2] ^= g_sw_state[0];
    g_sw_state[3] ^= g_sw_state[1];
    g_sw_state[1] ^= g_sw_state[2];
    g_sw_state[0] ^= g_sw_state[3];
    g_sw_state[2] ^= t;
    g_sw_state[3] = rotl(g_sw_state[3], 45);
    return result;
}

// splitmix64 to expand a single seed into the full xoshiro state
uint64_t splitmix64(uint64_t* state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

void sw_prng_init() {
    uint64_t seed = clock::now_ns();
    seed ^= reinterpret_cast<uintptr_t>(&seed);
    if (seed == 0) seed = 0xdeadbeefcafe1234ULL;
    g_sw_state[0] = splitmix64(&seed);
    g_sw_state[1] = splitmix64(&seed);
    g_sw_state[2] = splitmix64(&seed);
    g_sw_state[3] = splitmix64(&seed);
    g_use_sw_prng = true;
}

void sw_prng_fill(void* buf, size_t len) {
    auto* dst = static_cast<uint8_t*>(buf);
    size_t offset = 0;
    while (offset < len) {
        uint64_t val = xoshiro256ss();
        size_t remaining = len - offset;
        size_t chunk = remaining < sizeof(val) ? remaining : sizeof(val);
        auto* src = reinterpret_cast<const uint8_t*>(&val);
        for (size_t i = 0; i < chunk; i++) {
            dst[offset + i] = src[i];
        }
        offset += chunk;
    }
}

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
    if (hw::rng::available()) {
        if (hw::rng::fill(buf, len)) {
            return OK;
        }
    }
    if (g_use_sw_prng) {
        sw_prng_fill(buf, len);
        return OK;
    }
    return ERR_NOSRC;
}

__PRIVILEGED_CODE int32_t init() {
    if (hw::rng::available()) {
        log::info("random: hardware RNG available");
    } else {
        log::warn("random: no hardware RNG, using software PRNG fallback");
        sw_prng_init();
    }

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
