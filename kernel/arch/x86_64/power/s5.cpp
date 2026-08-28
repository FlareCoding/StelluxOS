#include "power/s5.h"
#include "common/string.h"

namespace power {

// AML opcodes involved in the Name(_S5_, Package(...)) pattern
constexpr uint8_t AML_NAME_OP      = 0x08;
constexpr uint8_t AML_ROOT_CHAR    = 0x5C;
constexpr uint8_t AML_PACKAGE_OP   = 0x12;
constexpr uint8_t AML_ZERO_OP      = 0x00;
constexpr uint8_t AML_ONE_OP       = 0x01;
constexpr uint8_t AML_BYTE_PREFIX  = 0x0A;
constexpr uint8_t AML_WORD_PREFIX  = 0x0B;
constexpr uint8_t AML_DWORD_PREFIX = 0x0C;
constexpr uint8_t AML_ONES_OP      = 0xFF;

// SLP_TYP is a 3-bit field in the PM1 control register
constexpr uint8_t SLP_TYP_VALUE_MASK = 0x07;

/**
 * Decode an AML PkgLength. The two top bits of the lead byte give the
 * count of extra bytes, and the encoding splits the value across them.
 * Returns the bytes consumed, or 0 on malformed input.
 */
static size_t skip_pkg_length(const uint8_t* p, size_t remaining) {
    if (remaining == 0) {
        return 0;
    }

    size_t extra_bytes = p[0] >> 6;
    if (remaining < 1 + extra_bytes) {
        return 0;
    }

    return 1 + extra_bytes;
}

/**
 * Decode one integer package element (ZeroOp, OneOp, OnesOp, or a
 * Byte/Word/DWord constant). Returns the bytes consumed, or 0 on
 * anything that is not an integer constant.
 */
static size_t decode_integer(const uint8_t* p, size_t remaining,
                             uint64_t* out) {
    if (remaining == 0) {
        return 0;
    }

    switch (p[0]) {
    case AML_ZERO_OP:
        *out = 0;
        return 1;
    case AML_ONE_OP:
        *out = 1;
        return 1;
    case AML_ONES_OP:
        *out = 0xFF;
        return 1;
    case AML_BYTE_PREFIX:
        if (remaining < 2) return 0;
        *out = p[1];
        return 2;
    case AML_WORD_PREFIX:
        if (remaining < 3) return 0;
        *out = static_cast<uint64_t>(p[1]) | (static_cast<uint64_t>(p[2]) << 8);
        return 3;
    case AML_DWORD_PREFIX:
        if (remaining < 5) return 0;
        *out = static_cast<uint64_t>(p[1])
             | (static_cast<uint64_t>(p[2]) << 8)
             | (static_cast<uint64_t>(p[3]) << 16)
             | (static_cast<uint64_t>(p[4]) << 24);
        return 5;
    default:
        return 0;
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE bool find_s5_sleep_types(const uint8_t* aml, size_t len,
                                           uint8_t* typ_a, uint8_t* typ_b) {
    for (size_t i = 0; i + 5 <= len; i++) {
        if (string::memcmp(aml + i, "_S5_", 4) != 0) {
            continue;
        }

        if (aml[i + 4] != AML_PACKAGE_OP) {
            continue;
        }

        // The name must come from a NameOp, optionally through the
        // root prefix ("\_S5_"), or the match is unrelated bytes.
        bool named = (i >= 1 && aml[i - 1] == AML_NAME_OP)
                  || (i >= 2 && aml[i - 1] == AML_ROOT_CHAR
                             && aml[i - 2] == AML_NAME_OP);
        if (!named) {
            continue;
        }

        const uint8_t* p = aml + i + 5;
        size_t remaining = len - (i + 5);

        size_t consumed = skip_pkg_length(p, remaining);
        if (consumed == 0) {
            continue;
        }
        p += consumed;
        remaining -= consumed;

        // NumElements, the package must hold at least SLP_TYPa and SLP_TYPb
        if (remaining == 0 || p[0] < 2) {
            continue;
        }
        p += 1;
        remaining -= 1;

        uint64_t value_a = 0;
        consumed = decode_integer(p, remaining, &value_a);
        if (consumed == 0) {
            continue;
        }
        p += consumed;
        remaining -= consumed;

        uint64_t value_b = 0;
        consumed = decode_integer(p, remaining, &value_b);
        if (consumed == 0) {
            continue;
        }

        *typ_a = static_cast<uint8_t>(value_a) & SLP_TYP_VALUE_MASK;
        *typ_b = static_cast<uint8_t>(value_b) & SLP_TYP_VALUE_MASK;
        return true;
    }

    return false;
}

} // namespace power
