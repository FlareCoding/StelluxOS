#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "fs/devfs/devfs.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "mm/heap.h"
#include "clock/clock.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"
#include "sched/sched.h"
#include "sync/atomic.h"
#include "sync/poll.h"
#include "helpers.h"

TEST_SUITE(devfs);

static constexpr const char* REMOVAL_DIR_NAME = "devfs_removal";
static constexpr const char* REMOVAL_DIR_PATH = "/dev/devfs_removal";
static constexpr size_t TRANSFER_BYTES = 16;
static constexpr size_t LISTING_BATCH_ENTRIES = 4;

// What a test reads of one removable device, kept in unprivileged memory so the test reads it without elevating
struct removable_device_record {
    sync::atomic<uint32_t> calls; // Reads, writes and ioctls the device answered
    sync::atomic<uint32_t> is_destroyed;
};

// A device that counts its reads, writes and ioctls and marks its destruction in its test's record
class removable_test_device : public fs::node {
public:
    removable_test_device(const char* name, removable_device_record* record)
        : fs::node(fs::node_type::char_device, nullptr, name), m_record(record) {}

    ~removable_test_device() override {
        m_record->is_destroyed.store_release(1);
    }

    ssize_t read(fs::file*, void*, size_t count, uint32_t) override {
        m_record->calls.fetch_add_relaxed(1);
        return static_cast<ssize_t>(count);
    }

    ssize_t write(fs::file*, const void*, size_t count, uint32_t) override {
        m_record->calls.fetch_add_relaxed(1);
        return static_cast<ssize_t>(count);
    }

    int32_t ioctl(fs::file*, uint32_t, uint64_t) override {
        m_record->calls.fetch_add_relaxed(1);
        return fs::OK;
    }

private:
    removable_device_record* m_record;
};

// Adds a removable device to the removal directory, returning it with the reference its creator holds
static removable_test_device* add_removable_device(const char* name, removable_device_record* record) {
    removable_test_device* device = nullptr;
    RUN_ELEVATED({
        fs::node* dir = devfs::ensure_dir(REMOVAL_DIR_NAME);
        device = heap::kalloc_new<removable_test_device>(name, record);
        bool is_added = dir && device && devfs::add_char_device_at(dir, device) == devfs::OK;
        if (device && !is_added) {
            heap::kfree_delete(device);
            device = nullptr;
        }
    });

    return device;
}

// Removes the device from devfs as a driver does when its device is unplugged
static int32_t remove_device(fs::node* device) {
    int32_t rc = devfs::ERR;
    RUN_ELEVATED({
        rc = devfs::remove_char_device(device);
    });

    return rc;
}

// Drops the reference the device's creator holds
static void release_device(fs::node* device) {
    RUN_ELEVATED({
        if (device->release()) {
            fs::node::ref_destroy(device);
        }
    });
}

// True when listing the removal directory shows an entry named `name`
static bool removal_dir_lists(const char* name) {
    fs::file* dir = fs::open(REMOVAL_DIR_PATH, fs::O_RDONLY);
    if (!dir) {
        return false;
    }

    fs::dirent entries[LISTING_BATCH_ENTRIES];
    bool is_listed = false;
    ssize_t count = fs::readdir(dir, entries, LISTING_BATCH_ENTRIES);
    while (count > 0 && !is_listed) {
        for (ssize_t i = 0; i < count; i++) {
            if (string::strcmp(entries[i].name, name) == 0) {
                is_listed = true;
            }
        }

        count = fs::readdir(dir, entries, LISTING_BATCH_ENTRIES);
    }

    fs::close(dir);

    return is_listed;
}

TEST(devfs, append_writes_only_what_fits_and_returns_the_full_length) {
    char buf[4] = {};

    size_t pos = devfs::append_str(buf, sizeof(buf), 0, "hello");
    EXPECT_EQ(pos, 5u);
    EXPECT_EQ(string::memcmp(buf, "hell", 4), 0);

    pos = devfs::append_u64(buf, sizeof(buf), pos, 12345);
    EXPECT_EQ(pos, 10u);
}

static constexpr size_t LARGE_TEXT_LENGTH = 10000;
static constexpr size_t READ_CHUNK_BYTES = 1000;

static char g_large_text[LARGE_TEXT_LENGTH + 1];

static char large_text_letter(size_t index) {
    return static_cast<char>('a' + index % 26);
}

static size_t generate_large_text(char* buf, size_t cap) {
    size_t pos = 0;
    for (size_t i = 0; i < LARGE_TEXT_LENGTH; i++) {
        char letter[2] = { large_text_letter(i), '\0' };
        pos = devfs::append_str(buf, cap, pos, letter);
    }

    return pos;
}

TEST(devfs, a_text_snapshot_grows_to_hold_the_whole_text) {
    int32_t rc = devfs::ERR;
    RUN_ELEVATED({
        fs::node* dir = devfs::ensure_dir("devfs_test");
        auto* node = heap::kalloc_new<devfs::text_snapshot_node>("large", generate_large_text);
        if (dir && node) {
            rc = devfs::add_char_device_at(dir, node);
        }

        if (rc != devfs::OK && node) {
            heap::kfree_delete(node);
        }
    });
    ASSERT_EQ(rc, devfs::OK);

    fs::file* f = fs::open("/dev/devfs_test/large", fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    size_t length = 0;
    while (length < sizeof(g_large_text)) {
        size_t room = sizeof(g_large_text) - length;
        size_t chunk = room < READ_CHUNK_BYTES ? room : READ_CHUNK_BYTES;
        ssize_t n = fs::read(f, g_large_text + length, chunk);
        if (n <= 0) {
            break;
        }

        length += static_cast<size_t>(n);
    }

    fs::close(f);

    size_t mismatches = 0;
    for (size_t i = 0; i < length; i++) {
        if (g_large_text[i] != large_text_letter(i)) {
            mismatches++;
        }
    }

    EXPECT_EQ(length, LARGE_TEXT_LENGTH);
    EXPECT_EQ(mismatches, 0u);
}

static constexpr const char* LISTED_DEVICE_PATH = "/dev/devfs_removal/listed";

static removable_device_record g_listed_record;

TEST(devfs, lookups_and_listings_stop_finding_a_removed_device) {
    removable_test_device* device = add_removable_device("listed", &g_listed_record);
    ASSERT_NOT_NULL(device);

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat(LISTED_DEVICE_PATH, &attr), fs::OK);
    EXPECT_TRUE(removal_dir_lists("listed"));

    ASSERT_EQ(remove_device(device), devfs::OK);

    int32_t open_error = fs::OK;
    EXPECT_NULL(fs::open(LISTED_DEVICE_PATH, fs::O_RDWR, &open_error));
    EXPECT_EQ(open_error, fs::ERR_NOENT);
    EXPECT_EQ(fs::stat(LISTED_DEVICE_PATH, &attr), fs::ERR_NOENT);
    EXPECT_FALSE(removal_dir_lists("listed"));

    release_device(device);
}

static constexpr const char* REFUSED_DIR_NAME = "devfs_removal_refused";
static constexpr const char* REFUSED_DIR_PATH = "/dev/devfs_removal_refused";

static removable_device_record g_unadded_record;
static removable_device_record g_twice_removed_record;

TEST(devfs, removal_refuses_anything_but_a_char_device_in_devfs) {
    removable_test_device* unadded = nullptr;
    RUN_ELEVATED({
        unadded = heap::kalloc_new<removable_test_device>("unadded", &g_unadded_record);
    });
    ASSERT_NOT_NULL(unadded);
    EXPECT_EQ(remove_device(unadded), devfs::ERR);
    release_device(unadded);

    removable_test_device* device = add_removable_device("twice_removed", &g_twice_removed_record);
    ASSERT_NOT_NULL(device);
    EXPECT_EQ(remove_device(device), devfs::OK);
    EXPECT_EQ(remove_device(device), devfs::ERR);
    release_device(device);

    fs::node* dir = nullptr;
    RUN_ELEVATED({
        dir = devfs::ensure_dir(REFUSED_DIR_NAME);
    });
    ASSERT_NOT_NULL(dir);
    EXPECT_EQ(remove_device(dir), devfs::ERR);

    fs::vattr attr = {};
    EXPECT_EQ(fs::stat(REFUSED_DIR_PATH, &attr), fs::OK);
}

static constexpr const char* CALLED_DEVICE_PATH = "/dev/devfs_removal/called";

static removable_device_record g_called_record;

TEST(devfs, an_open_file_keeps_calling_a_removed_device) {
    removable_test_device* device = add_removable_device("called", &g_called_record);
    ASSERT_NOT_NULL(device);

    fs::file* f = fs::open(CALLED_DEVICE_PATH, fs::O_RDWR);
    ASSERT_NOT_NULL(f);

    ASSERT_EQ(remove_device(device), devfs::OK);
    ASSERT_EQ(g_called_record.is_destroyed.load_acquire(), 0u);

    char bytes[TRANSFER_BYTES] = {};
    EXPECT_EQ(fs::read(f, bytes, sizeof(bytes)), static_cast<ssize_t>(sizeof(bytes)));
    EXPECT_EQ(fs::write(f, bytes, sizeof(bytes)), static_cast<ssize_t>(sizeof(bytes)));
    EXPECT_EQ(fs::ioctl(f, 0, 0), fs::OK);
    EXPECT_EQ(g_called_record.calls.load_acquire(), 3u);

    fs::close(f);
    release_device(device);
}

static constexpr const char* FREED_DEVICE_PATH = "/dev/devfs_removal/freed";

static removable_device_record g_freed_record;

TEST(devfs, a_removed_device_is_freed_when_its_last_file_closes) {
    removable_test_device* device = add_removable_device("freed", &g_freed_record);
    ASSERT_NOT_NULL(device);

    fs::file* f = fs::open(FREED_DEVICE_PATH, fs::O_RDWR);
    ASSERT_NOT_NULL(f);

    ASSERT_EQ(remove_device(device), devfs::OK);
    release_device(device);
    ASSERT_EQ(g_freed_record.is_destroyed.load_acquire(), 0u);

    fs::close(f);
    EXPECT_EQ(g_freed_record.is_destroyed.load_acquire(), 1u);
}

static removable_device_record g_kept_record;

TEST(devfs, a_removed_device_lives_until_its_creator_releases_it) {
    removable_test_device* device = add_removable_device("kept", &g_kept_record);
    ASSERT_NOT_NULL(device);

    ASSERT_EQ(remove_device(device), devfs::OK);
    ASSERT_EQ(g_kept_record.is_destroyed.load_acquire(), 0u);

    release_device(device);
    EXPECT_EQ(g_kept_record.is_destroyed.load_acquire(), 1u);
}

static constexpr const char* WATCHED_DIR_NAME = "devfs_watched";
static constexpr const char* WATCHED_PATH = "/dev/devfs_watched/text";
static constexpr const char WATCHED_TEXT[] = "watched\n";
static constexpr ssize_t WATCHED_TEXT_LENGTH = sizeof(WATCHED_TEXT) - 1;

static devfs::generation_counter g_watched_generation;
static devfs::text_snapshot_node* g_watched_node;

// Set by a test so that the generator's next run counts a change
static bool g_changes_while_generating;

static size_t generate_watched_text(char* buf, size_t cap) {
    if (g_changes_while_generating) {
        g_changes_while_generating = false;
        (void)g_watched_generation.advance();
    }

    return devfs::append_str(buf, cap, 0, WATCHED_TEXT);
}

// Adds the watched node on first use and opens a file on it
static fs::file* open_watched_node() {
    if (!g_watched_node) {
        g_watched_generation.init();
        RUN_ELEVATED({
            fs::node* dir = devfs::ensure_dir(WATCHED_DIR_NAME);
            auto* node = heap::kalloc_new<devfs::text_snapshot_node>(
                "text", generate_watched_text, &g_watched_generation
            );
            if (dir && node && devfs::add_char_device_at(dir, node) == devfs::OK) {
                g_watched_node = node;
            } else if (node) {
                heap::kfree_delete(node);
            }
        });
    }

    return g_watched_node ? fs::open(WATCHED_PATH, fs::O_RDONLY) : nullptr;
}

static uint32_t poll_events(fs::file* f) {
    uint32_t events = 0;
    RUN_ELEVATED(events = f->get_node()->poll(f, nullptr));
    return events;
}

static void count_change() {
    RUN_ELEVATED((void)g_watched_generation.advance());
}

TEST(devfs, a_watched_snapshot_is_readable_until_it_holds_the_latest_change) {
    fs::file* f = open_watched_node();
    ASSERT_NOT_NULL(f);

    char text[sizeof(WATCHED_TEXT)] = {};
    EXPECT_EQ(poll_events(f), sync::POLL_IN);
    ASSERT_EQ(fs::read(f, text, sizeof(text)), WATCHED_TEXT_LENGTH);
    EXPECT_EQ(poll_events(f), 0u);

    count_change();
    EXPECT_EQ(poll_events(f), sync::POLL_IN);

    // Reading on past the snapshot's end takes no new one
    EXPECT_EQ(fs::read(f, text, sizeof(text)), 0);
    EXPECT_EQ(poll_events(f), sync::POLL_IN);

    ASSERT_EQ(fs::seek(f, 0, fs::SEEK_SET), 0);
    ASSERT_EQ(fs::read(f, text, sizeof(text)), WATCHED_TEXT_LENGTH);
    EXPECT_EQ(poll_events(f), 0u);

    fs::close(f);
}

TEST(devfs, a_change_while_the_text_is_generated_leaves_the_snapshot_stale) {
    fs::file* f = open_watched_node();
    ASSERT_NOT_NULL(f);

    char text[sizeof(WATCHED_TEXT)] = {};
    g_changes_while_generating = true;
    ASSERT_EQ(fs::read(f, text, sizeof(text)), WATCHED_TEXT_LENGTH);
    EXPECT_EQ(poll_events(f), sync::POLL_IN);

    ASSERT_EQ(fs::seek(f, 0, fs::SEEK_SET), 0);
    ASSERT_EQ(fs::read(f, text, sizeof(text)), WATCHED_TEXT_LENGTH);
    EXPECT_EQ(poll_events(f), 0u);

    fs::close(f);
}

static constexpr uint64_t POLL_TIMEOUT_NS = 10000000000ULL;

static fs::file* g_poller_file;
static sync::atomic<uint32_t> g_poller_events;
static sync::atomic<uint32_t> g_poller_is_woken;
static sync::atomic<uint32_t> g_poller_done;

// Polls the watched file on a task of its own, since the test runner's task cannot sleep
static void poll_watched_node(void*) {
    RUN_ELEVATED({
        sync::poll_table pt;
        pt.init(sched::current());
        g_poller_events.store_relaxed(g_poller_file->get_node()->poll(g_poller_file, &pt));
        g_poller_is_woken.store_relaxed(sync::poll_wait(pt, POLL_TIMEOUT_NS) ? 1 : 0);
        sync::poll_cleanup(pt);
    });

    g_poller_done.store_release(1);
    sched::exit(0);
}

TEST(devfs, a_change_wakes_a_poller) {
    fs::file* f = open_watched_node();
    ASSERT_NOT_NULL(f);

    char text[sizeof(WATCHED_TEXT)] = {};
    ASSERT_EQ(fs::read(f, text, sizeof(text)), WATCHED_TEXT_LENGTH);

    g_poller_file = f;
    g_poller_done.store_relaxed(0);
    sched::task* poller = test_helpers::start_pinned_task(poll_watched_node, nullptr, "devfs_poller");
    ASSERT_NOT_NULL(poller);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(poller));
    EXPECT_EQ(g_poller_events.load_acquire(), 0u);

    count_change();
    ASSERT_TRUE(test_helpers::spin_wait(g_poller_done));
    EXPECT_EQ(g_poller_is_woken.load_acquire(), 1u);

    test_helpers::unpin(poller);
    fs::close(f);
}

TEST(devfs, a_snapshot_node_seeks_only_back_to_its_start) {
    fs::file* f = open_watched_node();
    ASSERT_NOT_NULL(f);

    char letter = 0;
    ASSERT_EQ(fs::read(f, &letter, 1), 1);
    EXPECT_EQ(fs::seek(f, 0, fs::SEEK_CUR), 1);

    // Refused seeks leave the offset where it was
    EXPECT_EQ(fs::seek(f, 1, fs::SEEK_CUR), static_cast<int64_t>(fs::ERR_INVAL));
    EXPECT_EQ(fs::seek(f, 1, fs::SEEK_SET), static_cast<int64_t>(fs::ERR_INVAL));
    EXPECT_EQ(fs::seek(f, 0, fs::SEEK_END), static_cast<int64_t>(fs::ERR_INVAL));
    EXPECT_EQ(fs::seek(f, 0, fs::SEEK_CUR), 1);

    EXPECT_EQ(fs::seek(f, 0, fs::SEEK_SET), 0);
    ASSERT_EQ(fs::read(f, &letter, 1), 1);
    EXPECT_EQ(letter, WATCHED_TEXT[0]);

    fs::close(f);
}

static constexpr const char* HELD_PATH = "/dev/devfs_watched/held";
static constexpr uint64_t HOLD_TIMEOUT_NS = 10000000000ULL;

// A node whose generator counts its runs at once and runs until the test lets it finish, used on three tasks
static devfs::text_snapshot_node* g_held_node;
static fs::file* g_shared_file;
static sync::atomic<uint32_t> g_generators_running;
static sync::atomic<uint32_t> g_most_generators_running;
static sync::atomic<uint32_t> g_may_finish_generating;
static sync::atomic<uint32_t> g_shared_file_calls_done;

static size_t generate_held_text(char* buf, size_t cap) {
    uint32_t running = g_generators_running.fetch_add_acq_rel(1) + 1;
    if (running > g_most_generators_running.load_relaxed()) {
        g_most_generators_running.store_release(running);
    }

    uint64_t deadline = clock::now_ns() + HOLD_TIMEOUT_NS;
    while (!g_may_finish_generating.load_acquire() && clock::now_ns() < deadline) {
        test_helpers::brief_delay();
    }

    g_generators_running.fetch_sub_relaxed(1);
    return devfs::append_str(buf, cap, 0, WATCHED_TEXT);
}

static fs::file* open_held_node() {
    if (!g_held_node) {
        RUN_ELEVATED({
            fs::node* dir = devfs::ensure_dir(WATCHED_DIR_NAME);
            auto* node = heap::kalloc_new<devfs::text_snapshot_node>("held", generate_held_text);
            if (dir && node && devfs::add_char_device_at(dir, node) == devfs::OK) {
                g_held_node = node;
            } else if (node) {
                heap::kfree_delete(node);
            }
        });
    }

    return g_held_node ? fs::open(HELD_PATH, fs::O_RDONLY) : nullptr;
}

static void read_shared_file(void*) {
    char text[sizeof(WATCHED_TEXT)] = {};
    (void)fs::read(g_shared_file, text, sizeof(text));
    g_shared_file_calls_done.fetch_add_release(1);
    sched::exit(0);
}

static void rewind_shared_file(void*) {
    (void)fs::seek(g_shared_file, 0, fs::SEEK_SET);
    g_shared_file_calls_done.fetch_add_release(1);
    sched::exit(0);
}

// True once `t` sleeps, or once a second generator runs beside the first, as reads without the lock let happen
static bool sleeps_or_generates_beside_the_first(sched::task* t) {
    uint64_t deadline = clock::now_ns() + test_helpers::SPIN_TIMEOUT_NS;
    while (clock::now_ns() < deadline) {
        if (t->state.load_acquire() == sched::TASK_STATE_BLOCKED || g_most_generators_running.load_acquire() > 1) {
            return true;
        }

        test_helpers::brief_delay();
    }

    return false;
}

// Threads and forked processes share one open file, so its reads and seeks wait while one of its reads refills it
TEST(devfs, a_snapshot_being_taken_holds_off_other_reads_and_seeks_of_its_file) {
    fs::file* f = open_held_node();
    ASSERT_NOT_NULL(f);

    g_shared_file = f;
    g_generators_running.store_relaxed(0);
    g_most_generators_running.store_relaxed(0);
    g_may_finish_generating.store_relaxed(0);
    g_shared_file_calls_done.store_relaxed(0);

    sched::task* first = test_helpers::start_pinned_task_on_another_cpu(read_shared_file, nullptr, "devfs_reader", 0);
    ASSERT_NOT_NULL(first);
    ASSERT_TRUE(test_helpers::spin_wait_ge(g_generators_running, 1));

    sched::task* second = test_helpers::start_pinned_task_on_another_cpu(read_shared_file, nullptr, "devfs_reader", 0);
    ASSERT_NOT_NULL(second);
    EXPECT_TRUE(sleeps_or_generates_beside_the_first(second));

    sched::task* rewinder = test_helpers::start_pinned_task_on_another_cpu(
        rewind_shared_file, nullptr, "devfs_rewinder", 0
    );
    ASSERT_NOT_NULL(rewinder);
    EXPECT_TRUE(test_helpers::blocks_before_deadline(rewinder));

    g_may_finish_generating.store_release(1);
    EXPECT_TRUE(test_helpers::spin_wait_ge(g_shared_file_calls_done, 3));
    EXPECT_EQ(g_most_generators_running.load_acquire(), 1u);

    test_helpers::unpin(first);
    test_helpers::unpin(second);
    test_helpers::unpin(rewinder);
    fs::close(f);
}

// Room for the uptime's digits and its line break
static constexpr size_t UPTIME_TEXT_BYTES = 32;

TEST(devfs, a_snapshot_node_without_a_generation_is_always_readable) {
    fs::file* f = fs::open("/dev/sysinfo/uptime", fs::O_RDONLY);
    ASSERT_NOT_NULL(f);

    char text[UPTIME_TEXT_BYTES] = {};
    EXPECT_GE(fs::read(f, text, sizeof(text)), 1);
    EXPECT_EQ(poll_events(f), sync::POLL_IN | sync::POLL_OUT);

    fs::close(f);
}

// Takes the watched and held nodes out of devfs, leaving their directory, which devfs never frees
static int32_t remove_snapshot_nodes() {
    if (g_watched_node) {
        (void)remove_device(g_watched_node);
        release_device(g_watched_node);
        g_watched_node = nullptr;
    }

    if (g_held_node) {
        (void)remove_device(g_held_node);
        release_device(g_held_node);
        g_held_node = nullptr;
    }

    return 0;
}

AFTER_ALL(devfs, remove_snapshot_nodes);
