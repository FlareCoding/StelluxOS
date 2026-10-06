#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "helpers.h"
#include "drivers/graphics/gfxfb.h"
#include "fs/fs.h"
#include "fs/file.h"
#include "fs/fstypes.h"
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

// Where the flush tests place their request and rect array in the user page
static constexpr size_t ARGS_OFFSET = 0;
static constexpr size_t RECTS_OFFSET = 64;

// Records every rect the device hands to the display
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

// Registration is permanent, so the display and its surface outlive the suite
static recording_display g_display;
static gfxfb::surface g_surface = {};

static int32_t register_test_display() {
    uintptr_t kernel_va = 0;
    pmm::phys_addr_t phys = 0;
    int32_t rc = vmm::alloc_contiguous(
        SURFACE_PAGES, pmm::ZONE_ANY, paging::PAGE_KERNEL_RW,
        vmm::ALLOC_ZERO, kva::tag::generic, kernel_va, phys);
    if (rc != vmm::OK) {
        return rc;
    }

    g_surface = {
        .phys        = phys,
        .width       = SURFACE_WIDTH,
        .height      = SURFACE_HEIGHT,
        .pitch       = SURFACE_PITCH,
        .bpp         = SURFACE_BPP,
        .red_shift   = SURFACE_RED_SHIFT,
        .green_shift = SURFACE_GREEN_SHIFT,
        .blue_shift  = SURFACE_BLUE_SHIFT,
        .cacheable   = true,
    };

    return gfxfb::register_display(g_surface, &g_display);
}

BEFORE_ALL(gfxfb, register_test_display);

// Calls an ioctl on /dev/gfxfb inside the page's address space, as a syscall body would
static int32_t call_gfxfb_ioctl(user_page& page, uint32_t cmd, uint64_t arg) {
    int32_t open_err = fs::OK;
    fs::file* file = fs::open("/dev/gfxfb", fs::O_RDWR, &open_err);
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

static bool rect_equals(const gfxfb::gfxfb_rect& rect, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

// GFXFB_GET_INFO describes the driver display and tells userland to flush
TEST(gfxfb, get_info_reports_the_driver_display) {
    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(call_gfxfb_ioctl(page, gfxfb::GFXFB_GET_INFO, page.addr), fs::OK);

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
}

// Rects crossing the display edge are trimmed, and empty or outside ones are dropped
TEST(gfxfb, flush_clips_rects_to_the_display) {
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
    g_display.recorded_count = 0;
    ASSERT_EQ(call_gfxfb_ioctl(page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::OK);

    ASSERT_EQ(g_display.recorded_count, static_cast<uint32_t>(3));
    EXPECT_TRUE(rect_equals(g_display.recorded[0], 2, 3, 10, 4));
    EXPECT_TRUE(rect_equals(g_display.recorded[1], 60, 30, 4, 2));
    EXPECT_TRUE(rect_equals(g_display.recorded[2], 1, 1, 63, 31));
}

// A list longer than one internal batch reaches the display complete and in order
TEST(gfxfb, flush_delivers_every_rect_of_a_long_list) {
    user_page page;
    ASSERT_TRUE(page.ready());

    constexpr uint32_t COUNT = 40;
    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    for (uint32_t i = 0; i < COUNT; i++) {
        rects[i] = {i, 0, 1, 1};
    }

    write_flush_args(page, COUNT);
    g_display.recorded_count = 0;
    ASSERT_EQ(call_gfxfb_ioctl(page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::OK);

    ASSERT_EQ(g_display.recorded_count, COUNT);
    for (uint32_t i = 0; i < COUNT; i++) {
        EXPECT_EQ(g_display.recorded[i].x, i);
    }
}

// Oversized lists, nonzero reserved fields and unreadable pointers fail before reaching the display
TEST(gfxfb, flush_rejects_malformed_requests) {
    user_page page;
    ASSERT_TRUE(page.ready());

    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    rects[0] = {0, 0, 1, 1};
    g_display.recorded_count = 0;

    write_flush_args(page, gfxfb::GFXFB_MAX_FLUSH_RECTS + 1);
    EXPECT_EQ(call_gfxfb_ioctl(page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::ERR_INVAL);

    auto* args = page.at<gfxfb::gfxfb_flush_args>(ARGS_OFFSET);
    write_flush_args(page, 1);
    args->reserved = 1;
    EXPECT_EQ(call_gfxfb_ioctl(page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::ERR_INVAL);

    write_flush_args(page, 1);
    args->rects = page.addr + pmm::PAGE_SIZE;
    EXPECT_EQ(call_gfxfb_ioctl(page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::ERR_INVAL);

    EXPECT_EQ(call_gfxfb_ioctl(page, gfxfb::GFXFB_FLUSH, 0), fs::ERR_INVAL);
    EXPECT_EQ(g_display.recorded_count, static_cast<uint32_t>(0));
}

// Mapping /dev/gfxfb reaches the surface memory, write-back because it is RAM
TEST(gfxfb, mmap_maps_the_display_surface) {
    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    fs::file* file = fs::open("/dev/gfxfb", fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uintptr_t addr = 0;
    uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
    EXPECT_EQ(fs::mmap(file, ctx, 0, SURFACE_SIZE, prot, mm::MM_MAP_SHARED, 0, &addr), mm::MM_CTX_OK);
    EXPECT_EQ(paging::get_physical(addr, ctx->pt_root), g_surface.phys);
    EXPECT_EQ(paging::get_physical(addr + pmm::PAGE_SIZE, ctx->pt_root), g_surface.phys + pmm::PAGE_SIZE);

    paging::page_flags_t flags = paging::get_page_flags(addr, ctx->pt_root);
    EXPECT_EQ(flags & paging::PAGE_TYPE_MASK, paging::PAGE_NORMAL);

    fs::close(file);
    mm::mm_context_release(ctx);
}

static recording_display g_rival_display;

// Once a driver display owns /dev/gfxfb, a later one cannot take it over
TEST(gfxfb, second_driver_display_is_refused) {
    EXPECT_EQ(gfxfb::register_display(g_surface, &g_rival_display), gfxfb::ERR);

    user_page page;
    ASSERT_TRUE(page.ready());

    auto* rects = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    rects[0] = {0, 0, 1, 1};
    write_flush_args(page, 1);

    g_display.recorded_count = 0;
    ASSERT_EQ(call_gfxfb_ioctl(page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::OK);
    EXPECT_EQ(g_display.recorded_count, static_cast<uint32_t>(1));
    EXPECT_EQ(g_rival_display.recorded_count, static_cast<uint32_t>(0));
}
