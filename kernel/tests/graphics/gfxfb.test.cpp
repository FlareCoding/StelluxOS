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

// The pixel layout GFXFB_GET_INFO reports for a driver display's buffers
static constexpr uint16_t DRIVER_BPP = 32;
static constexpr uint8_t DRIVER_RED_SHIFT = 16;
static constexpr uint8_t DRIVER_GREEN_SHIFT = 8;
static constexpr uint8_t DRIVER_BLUE_SHIFT = 0;

// Output 0's base buffer, the largest buffer the tests use, sized in whole pages for their shared allocation
static constexpr uint64_t OUTPUT_0_WIDTH = 64;
static constexpr uint64_t OUTPUT_0_HEIGHT = 32;
static constexpr uint64_t OUTPUT_0_PITCH = OUTPUT_0_WIDTH * (DRIVER_BPP / 8);
static constexpr uint64_t OUTPUT_0_SIZE = OUTPUT_0_PITCH * OUTPUT_0_HEIGHT;
static constexpr size_t OUTPUT_0_PAGES = OUTPUT_0_SIZE / pmm::PAGE_SIZE;

// Output 1's base buffer, a page into the shared allocation and smaller than output 0's in every dimension, so
// GFXFB_GET_INFO and mmap tell the primary apart
static constexpr uint64_t OUTPUT_1_OFFSET = pmm::PAGE_SIZE;
static constexpr uint64_t OUTPUT_1_WIDTH = 32;
static constexpr uint64_t OUTPUT_1_HEIGHT = 8;
static constexpr uint64_t OUTPUT_1_PITCH = OUTPUT_1_WIDTH * (DRIVER_BPP / 8);
static constexpr uint64_t OUTPUT_1_SIZE = OUTPUT_1_PITCH * OUTPUT_1_HEIGHT;

// A boot framebuffer that differs from output 0's buffer and the driver layout in every field GFXFB_GET_INFO reports
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

// A display with two outputs, dark until a test lights them, that records its last flush's output and the first
// MAX_RECORDED rects of its flushes
class recording_display : public gfxfb::display {
public:
    static constexpr uint32_t OUTPUTS = 2;
    static constexpr uint32_t MAX_RECORDED = 64;

    gfxfb::output_state states[OUTPUTS] = {};
    gfxfb::buffer       bases[OUTPUTS] = {};
    uint32_t            count = OUTPUTS; // `output_count()`'s answer, which a test may lower below OUTPUTS
    bool                flush_needed = true;
    int32_t             flush_result = gfxfb::OK;

    gfxfb::gfxfb_rect recorded[MAX_RECORDED] = {};
    uint32_t          recorded_count = 0;
    uint32_t          flushed_output = OUTPUTS; // the last flush's output, OUTPUTS before any

    uint32_t output_count() const override { return count; }
    bool needs_flush() const override { return flush_needed; }

    int32_t read_output(uint32_t output, gfxfb::output_state& out) override {
        if (output >= OUTPUTS) {
            return gfxfb::ERR_INVALID_ARG;
        }

        out = states[output];
        return gfxfb::OK;
    }

    gfxfb::buffer* base_buffer(uint32_t output) override {
        return output < OUTPUTS && states[output].lit ? &bases[output] : nullptr;
    }

    int32_t flush(uint32_t output, const gfxfb::gfxfb_rect* rects, uint32_t count) override {
        flushed_output = output;
        for (uint32_t i = 0; i < count && recorded_count < MAX_RECORDED; i++) {
            recorded[recorded_count++] = rects[i];
        }

        return flush_result;
    }
};

// One test's gfxfb, published as its own node apart from /dev/gfxfb, with `node` null when publishing failed
struct gfxfb_test_node {
    gfxfb::gfxfb_node* node;
    char               path[NODE_PATH_BYTES];
};

// A base buffer's memory is never freed, so every test's buffers use this one allocation
static pmm::phys_addr_t g_buffer_phys;

// devfs never removes a node, so each test's node takes the next number
static uint64_t g_next_node_number;

// Allocates the buffer memory every test shares, before the suite's first test
static int32_t allocate_buffer_memory() {
    uintptr_t kernel_va = 0;
    int32_t rc = vmm::OK;
    RUN_ELEVATED(rc = vmm::alloc_contiguous(
        OUTPUT_0_PAGES, pmm::ZONE_ANY, paging::PAGE_KERNEL_RW,
        vmm::ALLOC_ZERO, kva::tag::generic, kernel_va, g_buffer_phys));

    return rc;
}

BEFORE_ALL(gfxfb, allocate_buffer_memory);

// Lights `output` of `fake`, the boot output when `is_boot_output` holds. Output 0's base buffer is cacheable at the
// start of the shared memory, and output 1's is write-combining a page in, so a test can tell which one is mapped
static void light_output(recording_display& fake, uint32_t output, bool is_boot_output) {
    bool is_first = output == 0;

    fake.states[output] = {.lit = true, .boot_output = is_boot_output};
    fake.bases[output] = {
        .width     = static_cast<uint32_t>(is_first ? OUTPUT_0_WIDTH : OUTPUT_1_WIDTH),
        .height    = static_cast<uint32_t>(is_first ? OUTPUT_0_HEIGHT : OUTPUT_1_HEIGHT),
        .pitch     = static_cast<uint32_t>(is_first ? OUTPUT_0_PITCH : OUTPUT_1_PITCH),
        .phys      = is_first ? g_buffer_phys : g_buffer_phys + OUTPUT_1_OFFSET,
        .cacheable = is_first,
    };
}

// The boot framebuffer of every boot node, cacheable unlike `init()`'s, so a GFXFB_INFO_NEEDS_FLUSH set from
// `cacheable` rather than from the display fails a GFXFB_GET_INFO test
static gfxfb::boot_framebuffer test_boot_framebuffer() {
    return {
        .pixels = {
            .width     = BOOT_WIDTH,
            .height    = BOOT_HEIGHT,
            .pitch     = BOOT_PITCH,
            .phys      = g_buffer_phys,
            .cacheable = true,
        },
        .bpp         = BOOT_BPP,
        .red_shift   = BOOT_RED_SHIFT,
        .green_shift = BOOT_GREEN_SHIFT,
        .blue_shift  = BOOT_BLUE_SHIFT,
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

// A fresh gfxfb showing the boot framebuffer through its boot display
static gfxfb_test_node publish_boot_node() {
    gfxfb_test_node test = {};
    char name[NODE_NAME_BYTES] = {};
    name_next_node(name, test);

    gfxfb::gfxfb_node* node = nullptr;
    RUN_ELEVATED(node = heap::kalloc_new<gfxfb::gfxfb_node>(name, test_boot_framebuffer()));
    test.node = publish(node);

    return test;
}

// A fresh gfxfb showing `driver_display` from the start, as for a registration that finds no /dev/gfxfb
static gfxfb_test_node publish_driver_node(gfxfb::display* driver_display) {
    gfxfb_test_node test = {};
    char name[NODE_NAME_BYTES] = {};
    name_next_node(name, test);

    gfxfb::gfxfb_node* node = nullptr;
    RUN_ELEVATED(node = heap::kalloc_new<gfxfb::gfxfb_node>(name, driver_display));
    test.node = publish(node);

    return test;
}

// Asks the test's node to register `driver_display`, as a driver's `attach()` asks gfxfb
static int32_t register_driver_display(const gfxfb_test_node& test, gfxfb::display* driver_display) {
    int32_t rc = gfxfb::ERR;
    RUN_ELEVATED(rc = test.node->register_display(driver_display));

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

// Flushes one rect at the origin through the node at `path`
static int32_t flush_one_rect(const char* path, user_page& page) {
    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    rects[0] = {0, 0, 1, 1};
    write_flush_args(page, 1);

    return call_gfxfb_ioctl(path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET);
}

// True when `rect` holds exactly the given position and size
static bool rect_equals(const gfxfb::gfxfb_rect& rect, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

static recording_display g_info_display;

// GFXFB_GET_INFO and stat describe the primary output's base buffer, and GFXFB_GET_INFO tells userland to flush
TEST(gfxfb, get_info_reports_the_driver_display) {
    light_output(g_info_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_info_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_INFO, page.addr), fs::OK);

    auto* info = page.at<gfxfb::gfxfb_info>(0);
    EXPECT_EQ(info->width, OUTPUT_0_WIDTH);
    EXPECT_EQ(info->height, OUTPUT_0_HEIGHT);
    EXPECT_EQ(info->pitch, OUTPUT_0_PITCH);
    EXPECT_EQ(info->bpp, DRIVER_BPP);
    EXPECT_EQ(info->red_shift, DRIVER_RED_SHIFT);
    EXPECT_EQ(info->green_shift, DRIVER_GREEN_SHIFT);
    EXPECT_EQ(info->blue_shift, DRIVER_BLUE_SHIFT);
    EXPECT_EQ(info->flags, gfxfb::GFXFB_INFO_NEEDS_FLUSH);
    EXPECT_EQ(info->size, OUTPUT_0_SIZE);
    EXPECT_EQ(stat_size(test), OUTPUT_0_SIZE);
}

static recording_display g_no_flush_display;

// GFXFB_GET_INFO takes its flag from the display, so a driver display whose `needs_flush()` is false needs no flush
TEST(gfxfb, get_info_reads_needs_flush_from_the_display) {
    light_output(g_no_flush_display, 0, false);
    g_no_flush_display.flush_needed = false;
    gfxfb_test_node test = publish_driver_node(&g_no_flush_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_INFO, page.addr), fs::OK);
    EXPECT_EQ(page.at<gfxfb::gfxfb_info>(0)->flags, static_cast<uint8_t>(0));
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

// The boot display drives one output, lit and the boot output, whose base buffer is the boot framebuffer
TEST(gfxfb, boot_display_has_one_lit_boot_output) {
    gfxfb::boot_display boot(test_boot_framebuffer());

    gfxfb::output_state state = {};
    EXPECT_EQ(boot.output_count(), static_cast<uint32_t>(1));
    EXPECT_FALSE(boot.needs_flush());
    ASSERT_EQ(boot.read_output(0, state), gfxfb::OK);
    EXPECT_TRUE(state.lit);
    EXPECT_TRUE(state.boot_output);
    EXPECT_EQ(boot.read_output(1, state), gfxfb::ERR_INVALID_ARG);

    gfxfb::buffer* base = boot.base_buffer(0);
    ASSERT_NOT_NULL(base);
    EXPECT_EQ(base->width, BOOT_WIDTH);
    EXPECT_EQ(base->phys, g_buffer_phys);
    EXPECT_NULL(boot.base_buffer(1));
}

static recording_display g_clip_display;

// Rects crossing the edge of the primary output's base buffer are trimmed, and empty or outside ones are dropped
TEST(gfxfb, flush_clips_rects_to_the_primary_base_buffer) {
    light_output(g_clip_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_clip_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    rects[0] = {2, 3, 10, 4};                  // inside, kept whole
    rects[1] = {60, 30, 10, 10};               // crosses the corner, trimmed to 4x2
    rects[2] = {64, 0, 5, 5};                  // right of the base buffer, dropped
    rects[3] = {0, 0, 0, 7};                   // empty, dropped
    rects[4] = {0xFFFFFFF0, 0, 0x20, 1};       // x + width overflows, dropped
    rects[5] = {1, 1, 0xFFFFFFFF, 0xFFFFFFFF}; // oversized, trimmed to 63x31

    write_flush_args(page, 6);
    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::OK);

    EXPECT_EQ(g_clip_display.flushed_output, static_cast<uint32_t>(0));
    ASSERT_EQ(g_clip_display.recorded_count, static_cast<uint32_t>(3));
    EXPECT_TRUE(rect_equals(g_clip_display.recorded[0], 2, 3, 10, 4));
    EXPECT_TRUE(rect_equals(g_clip_display.recorded[1], 60, 30, 4, 2));
    EXPECT_TRUE(rect_equals(g_clip_display.recorded[2], 1, 1, 63, 31));
}

static recording_display g_long_list_display;

// A list longer than one internal batch reaches the display complete and in order
TEST(gfxfb, flush_delivers_every_rect_of_a_long_list) {
    light_output(g_long_list_display, 0, false);
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
    light_output(g_malformed_display, 0, false);
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

static recording_display g_failing_display;

// A display's ERR_DARK from `flush()` fails GFXFB_FLUSH with ERR_AGAIN, and its ERR_DEVICE with ERR_IO
TEST(gfxfb, flush_returns_the_display_failures_as_file_errors) {
    light_output(g_failing_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_failing_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    g_failing_display.flush_result = gfxfb::ERR_DARK;
    EXPECT_EQ(flush_one_rect(test.path, page), fs::ERR_AGAIN);

    g_failing_display.flush_result = gfxfb::ERR_DEVICE;
    EXPECT_EQ(flush_one_rect(test.path, page), fs::ERR_IO);
}

static recording_display g_mmap_display;

// Mapping the node reaches the primary output's base buffer, write-back because the display marks it cacheable
TEST(gfxfb, mmap_maps_the_primary_base_buffer) {
    light_output(g_mmap_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_mmap_display);
    ASSERT_NOT_NULL(test.node);

    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uintptr_t addr = 0;
    uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
    EXPECT_EQ(fs::mmap(file, ctx, 0, OUTPUT_0_SIZE, prot, mm::MM_MAP_SHARED, 0, &addr), mm::MM_CTX_OK);
    EXPECT_EQ(paging::get_physical(addr, ctx->pt_root), g_buffer_phys);
    EXPECT_EQ(paging::get_physical(addr + pmm::PAGE_SIZE, ctx->pt_root), g_buffer_phys + pmm::PAGE_SIZE);

    paging::page_flags_t flags = paging::get_page_flags(addr, ctx->pt_root);
    EXPECT_EQ(flags & paging::PAGE_TYPE_MASK, paging::PAGE_NORMAL);

    fs::close(file);
    mm::mm_context_release(ctx);
}

static recording_display g_two_lit_display;

// The lit boot output is primary even with a lit output of lower index: GFXFB_GET_INFO and stat describe its base
// buffer, mmap maps it, and FLUSH clips rects to that buffer and hands them to the display for that output
TEST(gfxfb, primary_is_the_lit_boot_output) {
    light_output(g_two_lit_display, 0, false);
    light_output(g_two_lit_display, 1, true);
    gfxfb_test_node test = publish_driver_node(&g_two_lit_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_INFO, page.addr), fs::OK);
    EXPECT_EQ(page.at<gfxfb::gfxfb_info>(0)->width, OUTPUT_1_WIDTH);
    EXPECT_EQ(stat_size(test), OUTPUT_1_SIZE);

    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    rects[0] = {30, 6, 10, 10}; // crosses output 1's corner, trimmed to 2x2

    write_flush_args(page, 1);
    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::OK);
    EXPECT_EQ(g_two_lit_display.flushed_output, static_cast<uint32_t>(1));
    ASSERT_EQ(g_two_lit_display.recorded_count, static_cast<uint32_t>(1));
    EXPECT_TRUE(rect_equals(g_two_lit_display.recorded[0], 30, 6, 2, 2));

    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uintptr_t addr = 0;
    uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
    EXPECT_EQ(fs::mmap(file, ctx, 0, OUTPUT_1_SIZE, prot, mm::MM_MAP_SHARED, 0, &addr), mm::MM_CTX_OK);
    EXPECT_EQ(paging::get_physical(addr, ctx->pt_root), g_buffer_phys + OUTPUT_1_OFFSET);

    paging::page_flags_t flags = paging::get_page_flags(addr, ctx->pt_root);
    EXPECT_EQ(flags & paging::PAGE_TYPE_MASK, paging::PAGE_WC);

    fs::close(file);
    mm::mm_context_release(ctx);
}

static recording_display g_dark_boot_display;
static recording_display g_no_boot_display;

// Without a lit boot output the lit output with the lowest index is primary, and a dark boot output is not
TEST(gfxfb, primary_is_the_lowest_lit_output_without_a_lit_boot_output) {
    g_dark_boot_display.states[0].boot_output = true;
    light_output(g_dark_boot_display, 1, false);
    gfxfb_test_node dark_boot = publish_driver_node(&g_dark_boot_display);
    ASSERT_NOT_NULL(dark_boot.node);

    light_output(g_no_boot_display, 0, false);
    light_output(g_no_boot_display, 1, false);
    gfxfb_test_node no_boot = publish_driver_node(&g_no_boot_display);
    ASSERT_NOT_NULL(no_boot.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(flush_one_rect(dark_boot.path, page), fs::OK);
    EXPECT_EQ(g_dark_boot_display.flushed_output, static_cast<uint32_t>(1));
    EXPECT_EQ(stat_size(dark_boot), OUTPUT_1_SIZE);

    ASSERT_EQ(flush_one_rect(no_boot.path, page), fs::OK);
    EXPECT_EQ(g_no_boot_display.flushed_output, static_cast<uint32_t>(0));
    EXPECT_EQ(stat_size(no_boot), OUTPUT_0_SIZE);
}

static recording_display g_short_count_display;

// Only outputs below `output_count()` can be primary, so a lit boot output at an index of `output_count()` is not,
// though the fake's `read_output()` answers for it
TEST(gfxfb, primary_is_chosen_below_the_output_count) {
    light_output(g_short_count_display, 0, false);
    light_output(g_short_count_display, 1, true);
    g_short_count_display.count = 1;
    gfxfb_test_node test = publish_driver_node(&g_short_count_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(flush_one_rect(test.path, page), fs::OK);
    EXPECT_EQ(g_short_count_display.flushed_output, static_cast<uint32_t>(0));
    EXPECT_EQ(stat_size(test), OUTPUT_0_SIZE);
}

static recording_display g_all_dark_display;

// A display registered with every output dark has no primary: stat reports size 0, GFXFB_GET_INFO and GFXFB_FLUSH fail
// with ERR_AGAIN, mmap is refused, and `flush()` is never called
TEST(gfxfb, display_with_every_output_dark_has_no_primary) {
    gfxfb_test_node test = publish_driver_node(&g_all_dark_display);
    ASSERT_NOT_NULL(test.node);
    EXPECT_EQ(stat_size(test), static_cast<size_t>(0));

    user_page page;
    ASSERT_TRUE(page.ready());

    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_INFO, page.addr), fs::ERR_AGAIN);
    EXPECT_EQ(flush_one_rect(test.path, page), fs::ERR_AGAIN);
    EXPECT_EQ(g_all_dark_display.recorded_count, static_cast<uint32_t>(0));

    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uintptr_t addr = 0;
    uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
    EXPECT_EQ(fs::mmap(file, ctx, 0, pmm::PAGE_SIZE, prot, mm::MM_MAP_SHARED, 0, &addr), mm::MM_CTX_ERR_INVALID_ARG);

    fs::close(file);
    mm::mm_context_release(ctx);
}

static recording_display g_dark_request_display;

// With no primary, GFXFB_GET_INFO refuses a null address, and GFXFB_FLUSH a null or unreadable request, a set reserved
// field or too many rects, with ERR_INVAL before either fails with ERR_AGAIN, and FLUSH fails before it reads a rect
TEST(gfxfb, requests_without_a_primary_are_checked_first) {
    gfxfb_test_node test = publish_driver_node(&g_dark_request_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_INFO, 0), fs::ERR_INVAL);
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, 0), fs::ERR_INVAL);
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + pmm::PAGE_SIZE), fs::ERR_INVAL);

    auto* args = page.at<gfxfb::gfxfb_flush_args>(ARGS_OFFSET);
    write_flush_args(page, 1);
    args->reserved = 1;
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::ERR_INVAL);

    write_flush_args(page, gfxfb::GFXFB_MAX_FLUSH_RECTS + 1);
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::ERR_INVAL);

    write_flush_args(page, 1);
    args->rects = page.addr + pmm::PAGE_SIZE;
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::ERR_AGAIN);
    EXPECT_EQ(g_dark_request_display.recorded_count, static_cast<uint32_t>(0));
}

static recording_display g_replacing_display;

// A driver display takes the boot display's place with its own primary, the lit boot output 1 beside a lit output 0:
// GFXFB_GET_INFO and stat describe that output's base buffer, and flushes reach the driver display for it
TEST(gfxfb, driver_display_replaces_the_boot_display) {
    light_output(g_replacing_display, 0, false);
    light_output(g_replacing_display, 1, true);
    gfxfb_test_node test = publish_boot_node();
    ASSERT_NOT_NULL(test.node);

    ASSERT_EQ(register_driver_display(test, &g_replacing_display), gfxfb::OK);
    EXPECT_TRUE(has_driver_display(test));

    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_INFO, page.addr), fs::OK);

    auto* info = page.at<gfxfb::gfxfb_info>(0);
    EXPECT_EQ(info->width, OUTPUT_1_WIDTH);
    EXPECT_EQ(info->pitch, OUTPUT_1_PITCH);
    EXPECT_EQ(info->bpp, DRIVER_BPP);
    EXPECT_EQ(info->flags, gfxfb::GFXFB_INFO_NEEDS_FLUSH);
    EXPECT_EQ(info->size, OUTPUT_1_SIZE);
    EXPECT_EQ(stat_size(test), OUTPUT_1_SIZE);

    ASSERT_EQ(flush_one_rect(test.path, page), fs::OK);
    EXPECT_EQ(g_replacing_display.recorded_count, static_cast<uint32_t>(1));
    EXPECT_EQ(g_replacing_display.flushed_output, static_cast<uint32_t>(1));
}

static recording_display g_late_display;

// Once the node has been opened, even by a file already closed, a driver display is refused and the boot display stays
TEST(gfxfb, register_display_is_refused_once_the_node_was_opened) {
    light_output(g_late_display, 0, false);
    gfxfb_test_node test = publish_boot_node();
    ASSERT_NOT_NULL(test.node);

    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);
    fs::close(file);

    EXPECT_EQ(register_driver_display(test, &g_late_display), gfxfb::ERR);
    EXPECT_FALSE(has_driver_display(test));
    EXPECT_EQ(stat_size(test), BOOT_SIZE);
}

static recording_display g_unlit_display;

// While the node shows its boot display, a driver display with no output lit is refused and the boot framebuffer stays
TEST(gfxfb, register_display_is_refused_without_an_output_lit) {
    gfxfb_test_node test = publish_boot_node();
    ASSERT_NOT_NULL(test.node);

    EXPECT_EQ(register_driver_display(test, &g_unlit_display), gfxfb::ERR);
    EXPECT_FALSE(has_driver_display(test));
    EXPECT_EQ(stat_size(test), BOOT_SIZE);
}

static recording_display g_first_display;
static recording_display g_rival_display;

// Once a node shows a driver display, a later one cannot take its place
TEST(gfxfb, second_driver_display_is_refused) {
    light_output(g_first_display, 0, false);
    light_output(g_rival_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_first_display);
    ASSERT_NOT_NULL(test.node);

    EXPECT_EQ(register_driver_display(test, &g_rival_display), gfxfb::ERR);

    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(flush_one_rect(test.path, page), fs::OK);
    EXPECT_EQ(g_first_display.recorded_count, static_cast<uint32_t>(1));
    EXPECT_EQ(g_rival_display.recorded_count, static_cast<uint32_t>(0));
}

// Registration is permanent, so /dev/gfxfb shows `g_registered_display` for the rest of the run
static recording_display g_registered_display;
static recording_display g_refused_display;

// `gfxfb::register_display` refuses a null display, puts the first driver display behind /dev/gfxfb and refuses a
// second, and `gfxfb::has_driver_display()` and `gfxfb::has_boot_display()` report which display /dev/gfxfb shows
TEST(gfxfb, register_display_puts_the_first_driver_display_behind_dev_gfxfb) {
    light_output(g_registered_display, 0, false);
    light_output(g_refused_display, 0, false);

    fs::vattr attr = {};
    bool had_boot_node = fs::stat("/dev/gfxfb", &attr) == fs::OK;

    int32_t null_rc = gfxfb::OK;
    bool registered_before = true;
    bool boot_before = !had_boot_node;
    int32_t first_rc = gfxfb::ERR;
    bool registered_after = false;
    bool boot_after = true;
    int32_t second_rc = gfxfb::OK;
    RUN_ELEVATED({
        null_rc = gfxfb::register_display(nullptr);
        registered_before = gfxfb::has_driver_display();
        boot_before = gfxfb::has_boot_display();
        first_rc = gfxfb::register_display(&g_registered_display);
        registered_after = gfxfb::has_driver_display();
        boot_after = gfxfb::has_boot_display();
        second_rc = gfxfb::register_display(&g_refused_display);
    });

    EXPECT_EQ(null_rc, gfxfb::ERR);
    EXPECT_FALSE(registered_before);
    EXPECT_EQ(boot_before, had_boot_node);
    ASSERT_EQ(first_rc, gfxfb::OK);
    EXPECT_TRUE(registered_after);
    EXPECT_FALSE(boot_after);
    EXPECT_EQ(second_rc, gfxfb::ERR);

    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(flush_one_rect("/dev/gfxfb", page), fs::OK);
    EXPECT_EQ(g_registered_display.recorded_count, static_cast<uint32_t>(1));
    EXPECT_EQ(g_refused_display.recorded_count, static_cast<uint32_t>(0));
}
