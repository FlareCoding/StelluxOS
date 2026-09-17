#include "net/tcp/wire.h"
#include "net/checksum.h"
#include "net/byteorder.h"

namespace net {
namespace tcp {

static uint32_t pseudo_header_sum(const ipv4::ipv4_addr& src, const ipv4::ipv4_addr& dst,
                                  size_t len) {
    ipv4::pseudo_header pseudo = {};
    pseudo.src = src;
    pseudo.dst = dst;
    pseudo.proto = ipv4::PROTO_TCP;
    pseudo.length = htons(static_cast<uint16_t>(len));

    return checksum_accumulate(0, &pseudo, sizeof(pseudo));
}

static uint16_t read_network_u16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

static uint32_t read_network_u32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

static bool is_sack_option_len(uint8_t len) {
    if (len < OPT_SACK_BASE_LEN + OPT_SACK_BLOCK_LEN) {
        return false;
    }

    size_t blocks = (len - OPT_SACK_BASE_LEN) / OPT_SACK_BLOCK_LEN;
    return (len - OPT_SACK_BASE_LEN) % OPT_SACK_BLOCK_LEN == 0 && blocks <= MAX_SACK_BLOCKS;
}

static void parse_sack_blocks(const uint8_t* opt, uint8_t len, tcp_options* out) {
    out->sack_count = static_cast<uint8_t>((len - OPT_SACK_BASE_LEN) / OPT_SACK_BLOCK_LEN);

    const uint8_t* block = opt + OPT_SACK_BASE_LEN;
    for (uint8_t i = 0; i < out->sack_count; i++, block += OPT_SACK_BLOCK_LEN) {
        out->sack_blocks[i].start = read_network_u32(block);
        out->sack_blocks[i].end = read_network_u32(block + sizeof(uint32_t));
    }
}

bool is_header_valid(const tcp_header* hdr, size_t len) {
    if (len < HEADER_LEN) {
        return false;
    }

    if (hdr->data_offset() < MIN_DATA_OFFSET) {
        return false;
    }

    return hdr->header_len() <= len;
}

bool parse_options(const tcp_header* hdr, tcp_options* out) {
    *out = tcp_options{};
    bool syn = hdr->flags & FLAG_SYN;

    const uint8_t* opt = reinterpret_cast<const uint8_t*>(hdr) + HEADER_LEN;
    const uint8_t* end = reinterpret_cast<const uint8_t*>(hdr) + hdr->header_len();

    while (opt < end) {
        uint8_t kind = opt[0];
        if (kind == OPT_END) {
            return true;
        }

        if (kind == OPT_NOP) {
            opt++;
            continue;
        }

        if (end - opt < 2) {
            return false;
        }

        uint8_t len = opt[1];
        if (len < 2 || len > end - opt) {
            return false;
        }

        // A known kind with the wrong length is skipped like an unknown one
        switch (kind) {
        case OPT_MSS:
            if (len == OPT_MSS_LEN && syn) {
                out->mss = read_network_u16(opt + 2);
            }
            break;
        case OPT_WINDOW_SCALE:
            if (len == OPT_WINDOW_SCALE_LEN && syn) {
                out->window_scale = opt[2] > MAX_WINDOW_SCALE ? MAX_WINDOW_SCALE : opt[2];
            }
            break;
        case OPT_SACK_PERMITTED:
            if (len == OPT_SACK_PERMITTED_LEN && syn) {
                out->sack_permitted = true;
            }
            break;
        case OPT_SACK:
            if (is_sack_option_len(len)) {
                parse_sack_blocks(opt, len, out);
            }
            break;
        case OPT_TIMESTAMPS:
            if (len == OPT_TIMESTAMPS_LEN) {
                out->has_timestamps = true;
                out->ts_val = read_network_u32(opt + 2);
                out->ts_ecr = read_network_u32(opt + 6);
            }
            break;
        default:
            break;
        }

        opt += len;
    }

    return true;
}

uint16_t compute_checksum(const ipv4::ipv4_addr& src, const ipv4::ipv4_addr& dst,
                          const void* segment, size_t len) {
    uint32_t sum = pseudo_header_sum(src, dst, len);
    sum = checksum_accumulate(sum, segment, len);

    return checksum_finish(sum);
}

bool is_checksum_valid(const ipv4::ipv4_addr& src, const ipv4::ipv4_addr& dst,
                       const void* segment, size_t len) {
    return compute_checksum(src, dst, segment, len) == 0;
}

} // namespace tcp
} // namespace net
