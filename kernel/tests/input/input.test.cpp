#define STLX_TEST_TIER TIER_UTIL

#include "stlx_unit_test.h"
#include "drivers/input/input.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/fstypes.h"

TEST_SUITE(input_test);

TEST(input_test, devfs_kbd_exists) {
    fs::node* n = nullptr;
    int32_t rc = fs::lookup("/dev/input/kbd", &n);
    EXPECT_EQ(rc, fs::OK);
    if (n) (void)n->release();
}

TEST(input_test, devfs_mouse_exists) {
    fs::node* n = nullptr;
    int32_t rc = fs::lookup("/dev/input/mouse", &n);
    EXPECT_EQ(rc, fs::OK);
    if (n) (void)n->release();
}

TEST(input_test, push_and_read_kbd) {
    input::kbd_event evt{};
    evt.action = input::KBD_ACTION_DOWN;
    evt.usage = 0x04;
    evt.modifiers = 0;
    EXPECT_EQ(input::push_kbd_event(evt), static_cast<int32_t>(1));

    int32_t open_err = fs::OK;
    fs::file* f = fs::open("/dev/input/kbd", fs::O_RDONLY, &open_err);
    ASSERT_NOT_NULL(f);

    input::kbd_event out{};
    ssize_t n = fs::read(f, &out, sizeof(out));
    EXPECT_EQ(n, static_cast<ssize_t>(sizeof(out)));
    EXPECT_EQ(out.action, input::KBD_ACTION_DOWN);
    EXPECT_EQ(out.usage, static_cast<uint16_t>(0x04));

    fs::close(f);
}

TEST(input_test, push_and_read_mouse) {
    input::mouse_event evt{};
    evt.x_value = 10;
    evt.y_value = -5;
    evt.buttons = 1;
    evt.flags = input::MOUSE_FLAG_RELATIVE;
    EXPECT_EQ(input::push_mouse_event(evt), static_cast<int32_t>(1));

    int32_t open_err = fs::OK;
    fs::file* f = fs::open("/dev/input/mouse", fs::O_RDONLY, &open_err);
    ASSERT_NOT_NULL(f);

    input::mouse_event out{};
    ssize_t n = fs::read(f, &out, sizeof(out));
    EXPECT_EQ(n, static_cast<ssize_t>(sizeof(out)));
    EXPECT_EQ(out.x_value, static_cast<int32_t>(10));
    EXPECT_EQ(out.y_value, static_cast<int32_t>(-5));
    EXPECT_EQ(out.buttons, static_cast<uint16_t>(1));

    fs::close(f);
}

TEST(input_test, nonblock_eagain) {
    int32_t open_err = fs::OK;
    fs::file* f = fs::open("/dev/input/kbd", fs::O_RDONLY | fs::O_NONBLOCK, &open_err);
    ASSERT_NOT_NULL(f);

    input::kbd_event out{};
    ssize_t n = fs::read(f, &out, sizeof(out));
    EXPECT_EQ(n, static_cast<ssize_t>(fs::ERR_AGAIN));

    fs::close(f);
}

TEST(input_test, short_buffer_einval) {
    input::kbd_event evt{};
    evt.action = input::KBD_ACTION_DOWN;
    evt.usage = 0x04;
    input::push_kbd_event(evt);

    int32_t open_err = fs::OK;
    fs::file* f = fs::open("/dev/input/kbd", fs::O_RDONLY | fs::O_NONBLOCK, &open_err);
    ASSERT_NOT_NULL(f);

    uint8_t small[4];
    ssize_t n = fs::read(f, small, sizeof(small));
    EXPECT_EQ(n, static_cast<ssize_t>(fs::ERR_INVAL));

    input::kbd_event drain{};
    fs::read(f, &drain, sizeof(drain));
    fs::close(f);
}

TEST(input_test, overflow_drops) {
    for (int i = 0; i < 1200; i++) {
        input::kbd_event evt{};
        evt.action = input::KBD_ACTION_DOWN;
        evt.usage = static_cast<uint16_t>(i & 0xFF);
        input::push_kbd_event(evt);
    }

    int32_t open_err = fs::OK;
    fs::file* f = fs::open("/dev/input/kbd", fs::O_RDONLY | fs::O_NONBLOCK, &open_err);
    ASSERT_NOT_NULL(f);

    int count = 0;
    input::kbd_event out{};
    while (fs::read(f, &out, sizeof(out)) > 0) {
        count++;
    }
    ASSERT_TRUE(count > 0);
    ASSERT_TRUE(count < 1200);

    fs::close(f);
}
