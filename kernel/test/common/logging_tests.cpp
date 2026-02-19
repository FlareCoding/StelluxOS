#include "test/framework/test_framework.h"
#include "common/logging.h"

namespace {

constexpr size_t CAPTURE_CAPACITY = 4096;
char g_capture[CAPTURE_CAPACITY] = {};
size_t g_capture_len = 0;

void capture_reset() {
    for (size_t i = 0; i < CAPTURE_CAPACITY; i++) {
        g_capture[i] = '\0';
    }
    g_capture_len = 0;
}

void capture_write(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (g_capture_len + 1 >= CAPTURE_CAPACITY) {
            break;
        }
        g_capture[g_capture_len++] = data[i];
    }
    g_capture[g_capture_len] = '\0';
}

bool capture_contains(const char* needle) {
    if (needle == nullptr || needle[0] == '\0') {
        return true;
    }

    for (size_t i = 0; g_capture[i] != '\0'; i++) {
        size_t j = 0;
        while (needle[j] != '\0' && g_capture[i + j] != '\0' && g_capture[i + j] == needle[j]) {
            j++;
        }
        if (needle[j] == '\0') {
            return true;
        }
    }
    return false;
}

const log::backend g_capture_backend = {
    .write = capture_write
};

__PRIVILEGED_CODE void logging_before_each(test::context&) {
    capture_reset();
    log::set_backend(&g_capture_backend);
}

__PRIVILEGED_CODE void logging_after_each(test::context&) {
    log::set_backend(nullptr);
}

} // namespace

STLX_TEST_SUITE_EX(core_logging, test::phase::early, logging_before_each, logging_after_each);

STLX_TEST(core_logging, info_prefix_and_integer_format) {
    log::info("value=%u", 42U);
    STLX_ASSERT_TRUE(ctx, capture_contains("[INFO]  value=42\r\n"));
}

STLX_TEST(core_logging, debug_hex_width_format) {
    log::debug("hex=%08x", 0x1a2b);
    STLX_ASSERT_TRUE(ctx, capture_contains("[DEBUG] hex=00001a2b\r\n"));
}

STLX_TEST(core_logging, string_precision_and_null_string) {
    log::info("s=%.3s null=%s", "abcdef", static_cast<const char*>(nullptr));
    STLX_ASSERT_TRUE(ctx, capture_contains("s=abc null=(null)\r\n"));
}

STLX_TEST(core_logging, percent_escape_is_rendered_once) {
    log::warn("progress=100%%");
    STLX_ASSERT_TRUE(ctx, capture_contains("[WARN]  progress=100%\r\n"));
}

STLX_TEST(core_logging, pointer_format_has_0x_prefix) {
    int local = 0;
    log::info("ptr=%p", &local);
    STLX_ASSERT_TRUE(ctx, capture_contains("[INFO]  ptr=0x"));
}
