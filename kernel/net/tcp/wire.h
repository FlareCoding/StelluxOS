#ifndef STELLUX_NET_TCP_WIRE_H
#define STELLUX_NET_TCP_WIRE_H

#include "net/ipv4.h"

namespace net {
namespace tcp {

constexpr size_t  HEADER_LEN      = 20;
constexpr size_t  MAX_HEADER_LEN  = 60;
constexpr size_t  MAX_OPTIONS_LEN = MAX_HEADER_LEN - HEADER_LEN;
constexpr uint8_t MIN_DATA_OFFSET = 5;
constexpr uint8_t MAX_DATA_OFFSET = 15;

constexpr uint8_t FLAG_FIN = 0x01;
constexpr uint8_t FLAG_SYN = 0x02;
constexpr uint8_t FLAG_RST = 0x04;
constexpr uint8_t FLAG_PSH = 0x08;
constexpr uint8_t FLAG_ACK = 0x10;
constexpr uint8_t FLAG_URG = 0x20;
constexpr uint8_t FLAG_ECE = 0x40;
constexpr uint8_t FLAG_CWR = 0x80;

// Option kinds (RFC 9293 3.2) and the wire length of each fixed-size option
constexpr uint8_t OPT_END            = 0;
constexpr uint8_t OPT_NOP            = 1;
constexpr uint8_t OPT_MSS            = 2;
constexpr uint8_t OPT_WINDOW_SCALE   = 3;
constexpr uint8_t OPT_SACK_PERMITTED = 4;
constexpr uint8_t OPT_SACK           = 5;
constexpr uint8_t OPT_TIMESTAMPS     = 8;

constexpr uint8_t OPT_MSS_LEN            = 4;
constexpr uint8_t OPT_WINDOW_SCALE_LEN   = 3;
constexpr uint8_t OPT_SACK_PERMITTED_LEN = 2;
constexpr uint8_t OPT_SACK_BASE_LEN      = 2;
constexpr uint8_t OPT_SACK_BLOCK_LEN     = 8;
constexpr uint8_t OPT_TIMESTAMPS_LEN     = 10;

constexpr uint8_t  MAX_SACK_BLOCKS                 = 4;
constexpr uint8_t  MAX_SACK_BLOCKS_WITH_TIMESTAMPS = 3;
constexpr uint8_t  MAX_WINDOW_SCALE                = 14; // RFC 7323 2.3
constexpr uint8_t  WINDOW_SCALE_NONE               = 0xFF;
constexpr uint16_t MSS_NONE                        = 0;

/**
 * TCP header (RFC 9293 3.1). Multi-byte fields are in network byte order, the
 * data offset counts 32-bit words including the options, and ECE and CWR are
 * the two high flag bits (RFC 3168). The reserved bits are ignored on input.
 * https://www.rfc-editor.org/rfc/rfc9293
 */
struct tcp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off_rsvd;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;

    inline uint8_t data_offset() const { return data_off_rsvd >> 4; }
    inline size_t header_len() const { return data_offset() * sizeof(uint32_t); }

    inline void set_data_offset(uint8_t words) {
        data_off_rsvd = static_cast<uint8_t>(words << 4);
    }
} __attribute__((packed));
static_assert(sizeof(tcp_header) == HEADER_LEN);

/**
 * One SACK block in host order: the bytes from `start` up to but not
 * including `end` have been received (RFC 2018 3).
 */
struct sack_block {
    uint32_t start;
    uint32_t end;
};

/**
 * The options of one segment, parsed once on input or describing what to send
 * on output. An absent option reads as `MSS_NONE`, `WINDOW_SCALE_NONE`, false,
 * or a `sack_count` of zero.
 */
struct tcp_options {
    uint16_t   mss            = MSS_NONE;
    uint8_t    window_scale   = WINDOW_SCALE_NONE;
    bool       sack_permitted = false;
    bool       has_timestamps = false;
    uint32_t   ts_val         = 0;
    uint32_t   ts_ecr         = 0;
    uint8_t    sack_count     = 0;
    sack_block sack_blocks[MAX_SACK_BLOCKS] = {};
};

/**
 * @brief True when the `len` bytes at `hdr` hold a complete header whose data
 * offset is at least MIN_DATA_OFFSET words and fits within `len`.
 */
bool is_header_valid(const tcp_header* hdr, size_t len);

/**
 * @brief Reads the options between the fixed header and the data offset into
 * `out`. MSS and window scale count only on a SYN, a scale above
 * MAX_WINDOW_SCALE is clamped, and unknown kinds are skipped by their length.
 * @return False when an option is truncated or shorter than two bytes, in
 *         which case the segment is malformed and must be dropped.
 */
bool parse_options(const tcp_header* hdr, tcp_options* out);

/**
 * @brief Writes the options `opts` describes at `dst`: the SYN options MSS,
 * SACK-permitted, timestamps, and window scale first, then the SACK blocks,
 * padded with NOPs to a multiple of four bytes.
 * @return Bytes written, at most MAX_OPTIONS_LEN.
 */
size_t build_options(uint8_t* dst, const tcp_options& opts);

/**
 * @brief The checksum over the pseudo-header for `src` and `dst` and the `len`
 * bytes of `segment`, whose own checksum field must be zero (RFC 9293 3.1).
 * @return The checksum in host order, stored with `htons`.
 */
uint16_t compute_checksum(const ipv4::ipv4_addr& src, const ipv4::ipv4_addr& dst,
                          const void* segment, size_t len);

/**
 * @brief True when the received `segment` of `len` bytes, checksum field
 * included, sums correctly for `src` and `dst`.
 */
bool is_checksum_valid(const ipv4::ipv4_addr& src, const ipv4::ipv4_addr& dst,
                       const void* segment, size_t len);

} // namespace tcp
} // namespace net

#endif // STELLUX_NET_TCP_WIRE_H
