#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "drivers/graphics/gfxfb.h"
#include "drivers/graphics/gfxfb_node.h"
#include "dynpriv/dynpriv.h"
#include "fs/devfs/devfs.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/fstypes.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/paging.h"
#include "mm/vma.h"
#include "mm/vmm.h"

using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(gfxfb);

static constexpr uint64_t SURFACE_WIDTH = 64;
static constexpr uint64_t SURFACE_HEIGHT = 32;
static constexpr uint16_t SURFACE_BPP = 32;
static constexpr uint64_t SURFACE_PITCH = SURFACE_WIDTH * (SURFACE_BPP / 8);
static constexpr uint64_t SURFACE_SIZE = SURFACE_PITCH * SURFACE_HEIGHT;
static constexpr size_t SURFACE_PAGES = SURFACE_SIZE / pmm::PAGE_SIZE;
static constexpr uint8_t SURFACE_RED_SHIFT = 16;
static constexpr uint8_t SURFACE_GREEN_SHIFT = 8;
static constexpr uint8_t SURFACE_BLUE_SHIFT = 0;

// A boot framebuffer layout that differs from the driver surface's in every field GFXFB_GET_INFO reports
static constexpr uint64_t BOOT_WIDTH = 48;
static constexpr uint64_t BOOT_HEIGHT = 16;
static constexpr uint16_t BOOT_BPP = 16;
static constexpr uint64_t BOOT_PITCH = 128;
static constexpr uint64_t BOOT_SIZE = BOOT_PITCH * BOOT_HEIGHT;
static constexpr uint8_t BOOT_RED_SHIFT = 0;
static constexpr uint8_t BOOT_GREEN_SHIFT = 5;
static constexpr uint8_t BOOT_BLUE_SHIFT = 11;

// Where the flush tests place their request and rect array in the user page
static constexpr size_t ARGS_OFFSET = 0;
static constexpr size_t RECTS_OFFSET = 64;

// A node's number and its path under /dev/gfxfb_test, each with room for its terminator
static constexpr size_t NODE_NAME_BYTES = 24;
static constexpr size_t NODE_PATH_BYTES = 48;

// Records the first MAX_RECORDED rects the node hands to it
class recording_display : public gfxfb::display {
public:
    static constexpr uint32_t MAX_RECORDED = 64;

    gfxfb::gfxfb_rect recorded[MAX_RECORDED] = {};
    uint32_t recorded_count = 0;

    int32_t flush(const gfxfb::gfxfb_rect* rects, uint32_t count) override {
        for (uint32_t i = 0; i < count && recorded_count < MAX_RECORDED; i++) {
            recorded[recorded_count++] = rects[i];
        }

        return gfxfb::OK;
    }
};

// One test's gfxfb, published as its own node apart from /dev/gfxfb, with `node` null when publishing failed
struct gfxfb_test_node {
    gfxfb::gfxfb_node* node;
    char               path[NODE_PATH_BYTES];
};

// A surface's memory is never freed, so every test's surface uses this one allocation
static pmm::phys_addr_t g_surface_phys;

// devfs never removes a node, so each test's node takes the next number
static uint64_t g_next_node_number;

// Allocates the surface memory every test shares, before the suite's first test
static int32_t allocate_surface_memory() {
    uintptr_t kernel_va = 0;
    int32_t rc = vmm::OK;
    RUN_ELEVATED(rc = vmm::alloc_contiguous(
        SURFACE_PAGES, pmm::ZONE_ANY, paging::PAGE_KERNEL_RW,
        vmm::ALLOC_ZERO, kva::tag::generic, kernel_va, g_surface_phys));

    return rc;
}

BEFORE_ALL(gfxfb, allocate_surface_memory);

// The surface of every driver display in the tests, in write-back RAM
static gfxfb::surface driver_surface() {
    return {
        .phys        = g_surface_phys,
        .width       = SURFACE_WIDTH,
        .height      = SURFACE_HEIGHT,
        .pitch       = SURFACE_PITCH,
        .bpp         = SURFACE_BPP,
        .red_shift   = SURFACE_RED_SHIFT,
        .green_shift = SURFACE_GREEN_SHIFT,
        .blue_shift  = SURFACE_BLUE_SHIFT,
        .cacheable   = true,
    };
}

// The boot framebuffer of every boot node, cacheable unlike `init()`'s, so a GFXFB_INFO_NEEDS_FLUSH set from
// `cacheable` rather than from the display fails a GFXFB_GET_INFO test
static gfxfb::surface boot_surface() {
    return {
        .phys        = g_surface_phys,
        .width       = BOOT_WIDTH,
        .height      = BOOT_HEIGHT,
        .pitch       = BOOT_PITCH,
        .bpp         = BOOT_BPP,
        .red_shift   = BOOT_RED_SHIFT,
        .green_shift = BOOT_GREEN_SHIFT,
        .blue_shift  = BOOT_BLUE_SHIFT,
        .cacheable   = true,
    };
}

// Writes the next node's number into `name` and its path into `test`, both zeroed, since the appends add no terminator
static void name_next_node(char (&name)[NODE_NAME_BYTES], gfxfb_test_node& test) {
    uint64_t number = g_next_node_number++;
    devfs::append_u64(name, NODE_NAME_BYTES - 1, 0, number);

    size_t pos = devfs::append_str(test.path, NODE_PATH_BYTES - 1, 0, "/dev/gfxfb_test/");
    devfs::append_u64(test.path, NODE_PATH_BYTES - 1, pos, number);
}

// Publishes `node` under /dev/gfxfb_test, or returns nullptr, freeing any node devfs refused
static gfxfb::gfxfb_node* publish(gfxfb::gfxfb_node* node) {
    if (!node) {
        return nullptr;
    }

    int32_t rc = devfs::ERR;
    RUN_ELEVATED({
        fs::node* dir = devfs::ensure_dir("gfxfb_test");
        if (dir) {
            rc = devfs::add_char_device_at(dir, node);
        }

        if (rc != devfs::OK) {
            heap::kfree_delete(node);
        }
    });

    return rc == devfs::OK ? node : nullptr;
}

// A fresh gfxfb showing the boot surface through its boot display
static gfxfb_test_node publish_boot_node() {
    gfxfb_test_node test = {};
    char name[NODE_NAME_BYTES] = {};
    name_next_node(name, test);

    gfxfb::gfxfb_node* node = nullptr;
    RUN_ELEVATED(node = heap::kalloc_new<gfxfb::gfxfb_node>(name, boot_surface()));
    test.node = publish(node);

    return test;
}

// A fresh gfxfb showing `driver_display` from the start, as for a registration that finds no /dev/gfxfb
static gfxfb_test_node publish_driver_node(gfxfb::display* driver_display) {
    gfxfb_test_node test = {};
    char name[NODE_NAME_BYTES] = {};
    name_next_node(name, test);

    gfxfb::gfxfb_node* node = nullptr;
    RUN_ELEVATED(node = heap::kalloc_new<gfxfb::gfxfb_node>(name, driver_surface(), driver_display));
    test.node = publish(node);

    return test;
}

// Asks the test's node to register `driver_display` with the driver surface, as a driver's `attach()` asks gfxfb
static int32_t register_driver_display(const gfxfb_test_node& test, gfxfb::display* driver_display) {
    int32_t rc = gfxfb::ERR;
    RUN_ELEVATED(rc = test.node->register_display(driver_surface(), driver_display));

    return rc;
}

// Asks the test's node whether it shows a driver display, elevated since the node lives on the privileged heap
static bool has_driver_display(const gfxfb_test_node& test) {
    bool registered = false;
    RUN_ELEVATED(registered = test.node->has_driver_display());

    return registered;
}

// The size `fs::stat` reports for the test's node, or 0 when the call fails
static size_t stat_size(const gfxfb_test_node& test) {
    fs::vattr attr = {};
    if (fs::stat(test.path, &attr) != fs::OK) {
        return 0;
    }

    return attr.size;
}

// Calls an ioctl on the node at `path` inside the page's address space, as a syscall body would
static int32_t call_gfxfb_ioctl(const char* path, user_page& page, uint32_t cmd, uint64_t arg) {
    int32_t open_err = fs::OK;
    fs::file* file = fs::open(path, fs::O_RDWR, &open_err);
    if (!file) {
        return open_err;
    }

    int32_t rc = fs::OK;
    {
        user_space_scope scope(page.ctx);
        rc = fs::ioctl(file, cmd, arg);
    }

    fs::close(file);

    return rc;
}

// Writes a flush request for `count` rects already placed at RECTS_OFFSET
static void write_flush_args(user_page& page, uint32_t count) {
    auto* args = page.at<gfxfb::gfxfb_flush_args>(ARGS_OFFSET);
    args->rects = page.addr + RECTS_OFFSET;
    args->count = count;
    args->reserved = 0;
}

// True when `rect` holds exactly the given position and size
static bool rect_equals(const gfxfb::gfxfb_rect& rect, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

static recording_display g_info_display;

// GFXFB_GET_INFO and stat describe the driver display, and GFXFB_GET_INFO tells userland to flush
TEST(gfxfb, get_info_reports_the_driver_display) {
    gfxfb_test_node test = publish_driver_node(&g_info_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_INFO, page.addr), fs::OK);

    auto* info = page.at<gfxfb::gfxfb_info>(0);
    EXPECT_EQ(info->width, SURFACE_WIDTH);
    EXPECT_EQ(info->height, SURFACE_HEIGHT);
    EXPECT_EQ(info->pitch, SURFACE_PITCH);
    EXPECT_EQ(info->bpp, SURFACE_BPP);
    EXPECT_EQ(info->red_shift, SURFACE_RED_SHIFT);
    EXPECT_EQ(info->green_shift, SURFACE_GREEN_SHIFT);
    EXPECT_EQ(info->blue_shift, SURFACE_BLUE_SHIFT);
    EXPECT_EQ(info->flags, gfxfb::GFXFB_INFO_NEEDS_FLUSH);
    EXPECT_EQ(info->size, SURFACE_SIZE);
    EXPECT_EQ(stat_size(test), SURFACE_SIZE);
}

// GFXFB_GET_INFO and stat describe the boot framebuffer as given, and userland need not flush it
TEST(gfxfb, get_info_reports_the_boot_display) {
    gfxfb_test_node test = publish_boot_node();
    ASSERT_NOT_NULL(test.node);
    EXPECT_FALSE(has_driver_display(test));

    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_INFO, page.addr), fs::OK);

    auto* info = page.at<gfxfb::gfxfb_info>(0);
    EXPECT_EQ(info->width, BOOT_WIDTH);
    EXPECT_EQ(info->height, BOOT_HEIGHT);
    EXPECT_EQ(info->pitch, BOOT_PITCH);
    EXPECT_EQ(info->bpp, BOOT_BPP);
    EXPECT_EQ(info->red_shift, BOOT_RED_SHIFT);
    EXPECT_EQ(info->green_shift, BOOT_GREEN_SHIFT);
    EXPECT_EQ(info->blue_shift, BOOT_BLUE_SHIFT);
    EXPECT_EQ(info->flags, static_cast<uint8_t>(0));
    EXPECT_EQ(info->size, BOOT_SIZE);
    EXPECT_EQ(stat_size(test), BOOT_SIZE);
}

static recording_display g_clip_display;

// Rects crossing the display edge are trimmed, and empty or outside ones are dropped
TEST(gfxfb, flush_clips_rects_to_the_display) {
    gfxfb_test_node test = publish_driver_node(&g_clip_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    rects[0] = {2, 3, 10, 4};                  // inside, kept whole
    rects[1] = {60, 30, 10, 10};               // crosses the corner, trimmed to 4x2
    rects[2] = {64, 0, 5, 5};                  // right of the display, dropped
    rects[3] = {0, 0, 0, 7};                   // empty, dropped
    rects[4] = {0xFFFFFFF0, 0, 0x20, 1};       // x + width overflows, dropped
    rects[5] = {1, 1, 0xFFFFFFFF, 0xFFFFFFFF}; // oversized, trimmed to 63x31

    write_flush_args(page, 6);
    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::OK);

    ASSERT_EQ(g_clip_display.recorded_count, static_cast<uint32_t>(3));
    EXPECT_TRUE(rect_equals(g_clip_display.recorded[0], 2, 3, 10, 4));
    EXPECT_TRUE(rect_equals(g_clip_display.recorded[1], 60, 30, 4, 2));
    EXPECT_TRUE(rect_equals(g_clip_display.recorded[2], 1, 1, 63, 31));
}

static recording_display g_long_list_display;

// A list longer than one internal batch reaches the display complete and in order
TEST(gfxfb, flush_delivers_every_rect_of_a_long_list) {
    gfxfb_test_node test = publish_driver_node(&g_long_list_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    constexpr uint32_t COUNT = 40;
    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    for (uint32_t i = 0; i < COUNT; i++) {
        rects[i] = {i, 0, 1, 1};
    }

    write_flush_args(page, COUNT);
    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::OK);

    ASSERT_EQ(g_long_list_display.recorded_count, COUNT);
    for (uint32_t i = 0; i < COUNT; i++) {
        EXPECT_EQ(g_long_list_display.recorded[i].x, i);
    }
}

// Expects ERR_INVAL from each malformed flush: too many rects, a reserved field set, unreadable rects, a null request
static void expect_malformed_flushes_refused(const gfxfb_test_node& test, user_page& page) {
    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    rects[0] = {0, 0, 1, 1};

    write_flush_args(page, gfxfb::GFXFB_MAX_FLUSH_RECTS + 1);
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::ERR_INVAL);

    auto* args = page.at<gfxfb::gfxfb_flush_args>(ARGS_OFFSET);
    write_flush_args(page, 1);
    args->reserved = 1;
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::ERR_INVAL);

    write_flush_args(page, 1);
    args->rects = page.addr + pmm::PAGE_SIZE;
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::ERR_INVAL);

    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, 0), fs::ERR_INVAL);
}

static recording_display g_malformed_display;

// Malformed requests fail before reaching the display
TEST(gfxfb, flush_rejects_malformed_requests) {
    gfxfb_test_node test = publish_driver_node(&g_malformed_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    expect_malformed_flushes_refused(test, page);
    EXPECT_EQ(g_malformed_display.recorded_count, static_cast<uint32_t>(0));
}

// FLUSH on the boot display accepts a valid request and refuses each malformed one, as on a driver display
TEST(gfxfb, flush_on_the_boot_display_checks_its_request) {
    gfxfb_test_node test = publish_boot_node();
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    rects[0] = {2, 3, 10, 4};

    write_flush_args(page, 1);
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::OK);

    expect_malformed_flushes_refused(test, page);
}

static recording_display g_mmap_display;

// Mapping the node reaches the surface memory, write-back because it is RAM
TEST(gfxfb, mmap_maps_the_display_surface) {
    gfxfb_test_node test = publish_driver_node(&g_mmap_display);
    ASSERT_NOT_NULL(test.node);

    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uintptr_t addr = 0;
    uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
    EXPECT_EQ(fs::mmap(file, ctx, 0, SURFACE_SIZE, prot, mm::MM_MAP_SHARED, 0, &addr), mm::MM_CTX_OK);
    EXPECT_EQ(paging::get_physical(addr, ctx->pt_root), g_surface_phys);
    EXPECT_EQ(paging::get_physical(addr + pmm::PAGE_SIZE, ctx->pt_root), g_surface_phys + pmm::PAGE_SIZE);

    paging::page_flags_t flags = paging::get_page_flags(addr, ctx->pt_root);
    EXPECT_EQ(flags & paging::PAGE_TYPE_MASK, paging::PAGE_NORMAL);

    fs::close(file);
    mm::mm_context_release(ctx);
}

static recording_display g_replacing_display;

// A driver display takes the boot display's place: GFXFB_GET_INFO and stat describe its surface, and flushes reach it
TEST(gfxfb, driver_display_replaces_the_boot_display) {
    gfxfb_test_node test = publish_boot_node();
    ASSERT_NOT_NULL(test.node);

    ASSERT_EQ(register_driver_display(test, &g_replacing_display), gfxfb::OK);
    EXPECT_TRUE(has_driver_display(test));

    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_INFO, page.addr), fs::OK);

    auto* info = page.at<gfxfb::gfxfb_info>(0);
    EXPECT_EQ(info->width, SURFACE_WIDTH);
    EXPECT_EQ(info->pitch, SURFACE_PITCH);
    EXPECT_EQ(info->flags, gfxfb::GFXFB_INFO_NEEDS_FLUSH);
    EXPECT_EQ(info->size, SURFACE_SIZE);
    EXPECT_EQ(stat_size(test), SURFACE_SIZE);

    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    rects[0] = {0, 0, 1, 1};
    write_flush_args(page, 1);

    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::OK);
    EXPECT_EQ(g_replacing_display.recorded_count, static_cast<uint32_t>(1));
}

static recording_display g_first_display;
static recording_display g_rival_display;

// Once a node shows a driver display, a later one cannot take its place
TEST(gfxfb, second_driver_display_is_refused) {
    gfxfb_test_node test = publish_driver_node(&g_first_display);
    ASSERT_NOT_NULL(test.node);

    EXPECT_EQ(register_driver_display(test, &g_rival_display), gfxfb::ERR);

    user_page page;
    ASSERT_TRUE(page.ready());

    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    rects[0] = {0, 0, 1, 1};
    write_flush_args(page, 1);

    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::OK);
    EXPECT_EQ(g_first_display.recorded_count, static_cast<uint32_t>(1));
    EXPECT_EQ(g_rival_display.recorded_count, static_cast<uint32_t>(0));
}

// Registration is permanent, so /dev/gfxfb shows `g_registered_display` for the rest of the run
static recording_display g_registered_display;
static recording_display g_refused_display;

// `gfxfb::register_display` refuses a null display, puts the first driver display behind /dev/gfxfb and refuses a
// second, and `gfxfb::has_driver_display()` is false before the first and true after it
TEST(gfxfb, register_display_puts_the_first_driver_display_behind_dev_gfxfb) {
    int32_t null_rc = gfxfb::OK;
    bool registered_before = true;
    int32_t first_rc = gfxfb::ERR;
    bool registered_after = false;
    int32_t second_rc = gfxfb::OK;
    RUN_ELEVATED({
        null_rc = gfxfb::register_display(driver_surface(), nullptr);
        registered_before = gfxfb::has_driver_display();
        first_rc = gfxfb::register_display(driver_surface(), &g_registered_display);
        registered_after = gfxfb::has_driver_display();
        second_rc = gfxfb::register_display(driver_surface(), &g_refused_display);
    });

    EXPECT_EQ(null_rc, gfxfb::ERR);
    EXPECT_FALSE(registered_before);
    ASSERT_EQ(first_rc, gfxfb::OK);
    EXPECT_TRUE(registered_after);
    EXPECT_EQ(second_rc, gfxfb::ERR);

    user_page page;
    ASSERT_TRUE(page.ready());

    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    rects[0] = {0, 0, 1, 1};
    write_flush_args(page, 1);

    ASSERT_EQ(call_gfxfb_ioctl("/dev/gfxfb", page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::OK);
    EXPECT_EQ(g_registered_display.recorded_count, static_cast<uint32_t>(1));
    EXPECT_EQ(g_refused_display.recorded_count, static_cast<uint32_t>(0));
}
