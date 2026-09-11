#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "stub_interface.h"
#include "net/interface.h"
#include "net/net.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/node.h"
#include "sync/poll.h"
#include "common/string.h"

TEST_SUITE(netevents);

using namespace net;

static bool readable(fs::file* f) {
    return (f->get_node()->poll(f, nullptr) & sync::POLL_IN) != 0;
}

static uint64_t parse_generation(const char* text, ssize_t len) {
    uint64_t value = 0;
    for (ssize_t i = 0; i < len && text[i] != '\n'; i++) {
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }

    return value;
}

// --- reports_each_generation_once ---
// Proves: a fresh watcher reads the current generation, then nothing until a
// change, and poll says readable exactly when a read would return something.

TEST(netevents, reports_each_generation_once) {
    fs::file* f = fs::open("/dev/net/events", fs::O_RDONLY);
    ASSERT_NOT_NULL(f);
    EXPECT_TRUE(readable(f));

    char text[32];
    ssize_t len = fs::read(f, text, sizeof(text));
    ASSERT_TRUE(len > 1);
    EXPECT_EQ(text[len - 1], '\n');
    EXPECT_EQ(parse_generation(text, len), status_generation());
    EXPECT_FALSE(readable(f));
    EXPECT_EQ(fs::read(f, text, sizeof(text)), static_cast<ssize_t>(0));

    stub_interface link(false);
    link.set_link_up(true);
    EXPECT_TRUE(readable(f));

    len = fs::read(f, text, sizeof(text));
    ASSERT_TRUE(len > 1);
    EXPECT_EQ(parse_generation(text, len), status_generation());
    EXPECT_FALSE(readable(f));

    EXPECT_EQ(fs::read(f, text, 1), static_cast<ssize_t>(0));
    link.set_link_up(false);
    EXPECT_EQ(fs::read(f, text, 1), fs::ERR_INVAL);

    fs::close(f);
}
