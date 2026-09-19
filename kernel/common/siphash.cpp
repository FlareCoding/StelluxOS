#include "common/siphash.h"

namespace hash {

constexpr uint64_t INIT_V0 = 0x736f6d6570736575ULL;
constexpr uint64_t INIT_V1 = 0x646f72616e646f6dULL;
constexpr uint64_t INIT_V2 = 0x6c7967656e657261ULL;
constexpr uint64_t INIT_V3 = 0x7465646279746573ULL;
constexpr uint64_t FINAL_MARK = 0xff;

constexpr unsigned COMPRESSION_ROUNDS  = 2;
constexpr unsigned FINALIZATION_ROUNDS = 4;

struct sip_state {
    uint64_t v0;
    uint64_t v1;
    uint64_t v2;
    uint64_t v3;
};

static uint64_t rotl(uint64_t value, unsigned bits) {
    return (value << bits) | (value >> (64 - bits));
}

static uint64_t read_le64(const uint8_t* p, size_t len) {
    uint64_t word = 0;
    for (size_t i = 0; i < len; i++) {
        word |= uint64_t{p[i]} << (8 * i);
    }

    return word;
}

static void sip_round(sip_state& s) {
    s.v0 += s.v1;
    s.v1 = rotl(s.v1, 13);
    s.v1 ^= s.v0;
    s.v0 = rotl(s.v0, 32);
    s.v2 += s.v3;
    s.v3 = rotl(s.v3, 16);
    s.v3 ^= s.v2;
    s.v0 += s.v3;
    s.v3 = rotl(s.v3, 21);
    s.v3 ^= s.v0;
    s.v2 += s.v1;
    s.v1 = rotl(s.v1, 17);
    s.v1 ^= s.v2;
    s.v2 = rotl(s.v2, 32);
}

static void absorb(sip_state& s, uint64_t word) {
    s.v3 ^= word;
    for (unsigned i = 0; i < COMPRESSION_ROUNDS; i++) {
        sip_round(s);
    }

    s.v0 ^= word;
}

uint64_t siphash(const void* data, size_t len, const siphash_key& key) {
    sip_state s = {INIT_V0 ^ key.k0, INIT_V1 ^ key.k1, INIT_V2 ^ key.k0, INIT_V3 ^ key.k1};

    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + (len & ~size_t{7});
    for (; p < end; p += sizeof(uint64_t)) {
        absorb(s, read_le64(p, sizeof(uint64_t)));
    }

    // The last word carries the remaining bytes with the length in its top byte
    uint64_t last = uint64_t{static_cast<uint8_t>(len)} << 56;
    absorb(s, last | read_le64(p, len & 7));

    s.v2 ^= FINAL_MARK;
    for (unsigned i = 0; i < FINALIZATION_ROUNDS; i++) {
        sip_round(s);
    }

    return s.v0 ^ s.v1 ^ s.v2 ^ s.v3;
}

} // namespace hash
