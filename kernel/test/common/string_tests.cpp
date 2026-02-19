#include "test/framework/test_framework.h"
#include "common/string.h"

STLX_TEST_SUITE(core_string, test::phase::early);

STLX_TEST(core_string, strlen_handles_basic_inputs) {
    STLX_ASSERT_EQ(ctx, string::strlen(""), static_cast<size_t>(0));
    STLX_ASSERT_EQ(ctx, string::strlen("a"), static_cast<size_t>(1));
    STLX_ASSERT_EQ(ctx, string::strlen("hello"), static_cast<size_t>(5));
    STLX_ASSERT_EQ(ctx, string::strlen("stellux 3.0"), static_cast<size_t>(11));
}

STLX_TEST(core_string, memset_fills_expected_bytes) {
    uint8_t buf[32] = {};

    string::memset(buf, 0xA5, sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); i++) {
        STLX_ASSERT_EQ(ctx, buf[i], static_cast<uint8_t>(0xA5));
    }

    string::memset(buf, 0x00, sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); i++) {
        STLX_ASSERT_EQ(ctx, buf[i], static_cast<uint8_t>(0x00));
    }
}

STLX_TEST(core_string, memcpy_copies_and_memcmp_matches) {
    uint8_t src[16] = {
        0x10, 0x11, 0x12, 0x13,
        0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B,
        0x1C, 0x1D, 0x1E, 0x1F
    };
    uint8_t dst[16] = {};

    string::memcpy(dst, src, sizeof(src));
    STLX_ASSERT_EQ(ctx, string::memcmp(dst, src, sizeof(src)), static_cast<int>(0));

    dst[7] = 0x99;
    STLX_ASSERT_NE(ctx, string::memcmp(dst, src, sizeof(src)), static_cast<int>(0));
}

STLX_TEST(core_string, memcpy_with_zero_length_is_noop) {
    uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t dst[8] = {9, 9, 9, 9, 9, 9, 9, 9};

    string::memcpy(dst, src, 0);
    for (size_t i = 0; i < sizeof(dst); i++) {
        STLX_ASSERT_EQ(ctx, dst[i], static_cast<uint8_t>(9));
    }
}

STLX_TEST(core_string, memcmp_lexicographic_ordering) {
    const char* a = "abc";
    const char* b = "abd";
    const char* c = "abc";

    STLX_ASSERT_EQ(ctx, string::memcmp(a, c, 3), static_cast<int>(0));
    STLX_ASSERT_TRUE(ctx, string::memcmp(a, b, 3) < 0);
    STLX_ASSERT_TRUE(ctx, string::memcmp(b, a, 3) > 0);
}
