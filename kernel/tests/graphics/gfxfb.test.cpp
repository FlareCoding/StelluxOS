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
#include "hw/cpu.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/paging.h"
#include "mm/uaccess.h"
#include "mm/vma.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "sync/poll.h"

using test_helpers::spin_wait;
using test_helpers::spin_wait_ge;
using test_helpers::unpin;
using test_helpers::user_page;
using test_helpers::user_space_scope;

TEST_SUITE(gfxfb);

// The pixel layout GFXFB_GET_INFO reports for a driver display's buffers
static constexpr uint16_t DRIVER_BPP = 32;
static constexpr uint8_t DRIVER_RED_SHIFT = 16;
static constexpr uint8_t DRIVER_GREEN_SHIFT = 8;
static constexpr uint8_t DRIVER_BLUE_SHIFT = 0;

// Output 0's base buffer, the largest base buffer the tests use, sized in whole pages for their shared allocation
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

// The refresh rates outputs 0 and 1 are lit at, unlike each other so a GFXFB_GET_OUTPUT reply shows whose mode it
// holds, and above 0xFFFF so a rate narrowed to 16 bits fails
static constexpr uint64_t OUTPUT_0_REFRESH_MILLIHERTZ = 144000;
static constexpr uint64_t OUTPUT_1_REFRESH_MILLIHERTZ = 75000;

// A mode whose width and height are above 0xFFFF and unlike any base buffer's size, so a reply narrowing either
// dimension or reporting the base buffer's size fails
static constexpr uint32_t WIDE_MODE_WIDTH = 70000;
static constexpr uint32_t WIDE_MODE_HEIGHT = 66000;

// An output index above 0xFFFF, which a 16-bit index would read as output 0
static constexpr uint32_t WIDE_OUTPUT_INDEX = 0x10000;

// A boot framebuffer that differs from output 0's buffer and the driver layout in every field GFXFB_GET_INFO reports
static constexpr uint64_t BOOT_WIDTH = 48;
static constexpr uint64_t BOOT_HEIGHT = 16;
static constexpr uint16_t BOOT_BPP = 16;
static constexpr uint64_t BOOT_PITCH = 128;
static constexpr uint64_t BOOT_SIZE = BOOT_PITCH * BOOT_HEIGHT;
static constexpr uint8_t BOOT_RED_SHIFT = 0;
static constexpr uint8_t BOOT_GREEN_SHIFT = 5;
static constexpr uint8_t BOOT_BLUE_SHIFT = 11;

// Where the flush and present tests place their request and rect array in the user page
static constexpr size_t ARGS_OFFSET = 0;
static constexpr size_t RECTS_OFFSET = 64;

// How many regions `map_many_regions` writes, one more than a present may name so the bound is tested with every region
// readable, in pages of their own since they need more than one
static constexpr uint32_t MANY_REGION_COUNT = gfxfb::GFXFB_MAX_FLUSH_RECTS + 1;
static constexpr size_t MANY_REGIONS_PAGES =
    (MANY_REGION_COUNT * sizeof(gfxfb::gfxfb_rect) + pmm::PAGE_SIZE - 1) / pmm::PAGE_SIZE;

// A region count whose low 16 bits alone name one region, so a count narrowed to 16 bits passes the bound and fails its
// refusal test
static constexpr uint32_t WIDE_REGION_COUNT = 0x10001;

// What `get_output` fills the GFXFB_GET_OUTPUT reply's 32-bit fields and its refresh rate with first, so a field left
// unwritten fails its check
static constexpr uint32_t UNWRITTEN_FIELD = 0xFFFFFFFF;
static constexpr uint64_t UNWRITTEN_RATE = 0xFFFFFFFFFFFFFFFF;

// A created buffer's size, past what its pixels need and above output 0's base buffer's, and the padding ending each of
// its rows, so a size or pitch gfxfb derives from the width and height or reads from the primary output fails
static constexpr size_t CREATED_BUFFER_PAGES = 3;
static constexpr uint64_t CREATED_BUFFER_SIZE = CREATED_BUFFER_PAGES * pmm::PAGE_SIZE;
static constexpr uint32_t CREATED_ROW_PADDING = 32;

// The places after output 0's base buffer that created buffers take in turn, so two buffers created in a row lie on
// different pages, neither on a base buffer's
static constexpr size_t CREATED_BUFFER_SLOTS = 2;
static constexpr size_t SHARED_PAGES = OUTPUT_0_PAGES + CREATED_BUFFER_SLOTS * CREATED_BUFFER_PAGES;

// The buffer most tests create, whose pixels fit in a page with a pitch above 0xFF, so a pitch narrowed to 8 bits fails
static constexpr uint32_t SMALL_BUFFER_WIDTH = 72;
static constexpr uint32_t SMALL_BUFFER_HEIGHT = 8;
static constexpr uint32_t SMALL_BUFFER_PITCH = SMALL_BUFFER_WIDTH * (DRIVER_BPP / 8) + CREATED_ROW_PADDING;

// A width and a format whose low 16 bits alone the recording display accepts, so a field narrowed on its way to the
// display fails a refusal test
static constexpr uint32_t WIDE_BUFFER_WIDTH = 0x10000 + SMALL_BUFFER_WIDTH;
static constexpr uint32_t WIDE_FORMAT = 0x10000 + gfxfb::GFXFB_FORMAT_XRGB8888;

// A node's number and its path under /dev/gfxfb_test, each with room for its terminator
static constexpr size_t NODE_NAME_BYTES = 24;
static constexpr size_t NODE_PATH_BYTES = 48;

// A display with two outputs, dark until a test lights them, that records the first MAX_RECORDED rects of its flushes
// and regions of its last accepted present, creates write-combining buffers and counts buffers handed back
class recording_display : public gfxfb::display {
public:
    static constexpr uint32_t OUTPUTS = 2;
    static constexpr uint32_t MAX_RECORDED = 64;
    static constexpr uint32_t MAX_CREATED = gfxfb::GFXFB_CLIENT_BUFFERS_MAX + 2; // a full file, then two refills

    gfxfb::output_state states[OUTPUTS] = {};
    gfxfb::buffer       bases[OUTPUTS] = {};
    uint32_t            count = OUTPUTS; // `output_count()`'s answer, which a test may lower below OUTPUTS
    bool                has_base_buffers = true; // false makes `base_buffer()` return nullptr even for a lit output
    bool                flush_needed = true;
    int32_t             flush_result = gfxfb::OK;
    int32_t             create_result = gfxfb::OK; // when not OK, what `create_buffer()` returns without creating
    void                (*while_creating)() = nullptr; // run once by the next `create_buffer()`, before it creates
    int32_t             present_answer = gfxfb::OK; // when not OK, what `present()` returns without presenting
    void                (*while_presenting)() = nullptr; // run once by the next present accepted, before it returns

    gfxfb::gfxfb_rect recorded[MAX_RECORDED] = {};
    uint32_t          recorded_count = 0;
    uint32_t          flushed_output = OUTPUTS; // the last flush's output, OUTPUTS before any

    uint32_t               create_calls = 0; // refused calls included
    gfxfb::buffer          created[MAX_CREATED] = {};
    uint32_t               created_count = 0;
    gfxfb::buffer*         last_released = nullptr;
    sync::atomic<uint32_t> released_count{0}; // counted in the reaper's task, after `last_released` is written

    uint32_t       present_calls = 0; // refused calls included
    gfxfb::buffer* presented = nullptr; // the buffer of the last present accepted
    uint32_t       presented_output = OUTPUTS; // the output of the last present accepted, OUTPUTS before any
    gfxfb::buffer* presented_on[OUTPUTS] = {}; // the buffer of the last present accepted on each output
    bool           named_no_region_on[OUTPUTS] = {}; // whether that present passed null regions and a count of 0

    bool              got_null_regions = false; // the last present accepted passed null regions
    uint32_t          presented_region_count = 0; // that present's region count
    gfxfb::gfxfb_rect presented_regions[MAX_RECORDED] = {}; // its first MAX_RECORDED regions
    gfxfb::gfxfb_rect last_presented_region = {}; // its last region, zeros for none

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
        return has_base_buffers && output < OUTPUTS && states[output].lit ? &bases[output] : nullptr;
    }

    int32_t flush(uint32_t output, const gfxfb::gfxfb_rect* rects, uint32_t count) override {
        flushed_output = output;
        for (uint32_t i = 0; i < count && recorded_count < MAX_RECORDED; i++) {
            recorded[recorded_count++] = rects[i];
        }

        return flush_result;
    }

    int32_t create_buffer(uint32_t width, uint32_t height, uint32_t format, gfxfb::buffer** out) override;
    int32_t present(
        uint32_t output,
        gfxfb::buffer* buf,
        const gfxfb::gfxfb_rect* regions,
        uint32_t region_count
    ) override;

    void release_buffer(gfxfb::buffer* buf) override {
        last_released = buf;
        released_count.fetch_add_release(1);
    }
};

// One test's gfxfb, published as its own node apart from /dev/gfxfb, with `node` null when publishing failed
struct gfxfb_test_node {
    gfxfb::gfxfb_node* node;
    char               path[NODE_PATH_BYTES];
};

// A close run on a task of its own: the file it closes and the flag the task sets once the close returns
struct task_close {
    fs::file*              file;
    sync::atomic<uint32_t> done;
};

// A read run on a task of its own: the file, size and flags it reads with, its result and the flag set once it returns
struct task_read {
    fs::file*              file;
    size_t                 size;
    uint32_t               flags;
    ssize_t                result;
    sync::atomic<uint32_t> done;
};

// A creation run on a task of its own: the file and page it asks through, its result and the flag set once it returns
struct task_create {
    fs::file*              file;
    user_page*             page;
    int32_t                result;
    sync::atomic<uint32_t> done;
};

// A base buffer's memory is never freed, so every test's buffers use this one allocation
static pmm::phys_addr_t g_buffer_phys;

// devfs never removes a node, so each test's node takes the next number
static uint64_t g_next_node_number;

// The close `closes_before_deadline()` runs, static since a close that never returns leaves its task pointing to it
static task_close g_deadline_close;

// The read `reads_before_deadline()` runs and the events it takes, static since its task may outlive the call
static task_read g_deadline_read;
static gfxfb::gfxfb_event g_deadline_read_events[gfxfb::CLIENT_EVENTS_MAX];

// The creation `creates_before_deadline()` runs, static since one that never returns leaves its task pointing to it
static task_create g_deadline_create;

// Allocates the buffer memory every test shares, before the suite's first test
static int32_t allocate_buffer_memory() {
    uintptr_t kernel_va = 0;
    int32_t rc = vmm::OK;
    RUN_ELEVATED(rc = vmm::alloc_contiguous(
        SHARED_PAGES, pmm::ZONE_ANY, paging::PAGE_KERNEL_RW,
        vmm::ALLOC_ZERO, kva::tag::generic, kernel_va, g_buffer_phys));

    return rc;
}

BEFORE_ALL(gfxfb, allocate_buffer_memory);

int32_t recording_display::create_buffer(uint32_t width, uint32_t height, uint32_t format, gfxfb::buffer** out) {
    create_calls++;

    if (while_creating) {
        void (*hook)() = while_creating;
        while_creating = nullptr;
        hook();
    }

    if (create_result != gfxfb::OK) {
        return create_result;
    }

    uint64_t pitch = static_cast<uint64_t>(width) * (DRIVER_BPP / 8) + CREATED_ROW_PADDING;
    if (pitch * height > CREATED_BUFFER_SIZE || format != gfxfb::GFXFB_FORMAT_XRGB8888) {
        return gfxfb::ERR_INVALID_ARG;
    }

    if (created_count == MAX_CREATED) {
        return gfxfb::ERR_NO_MEMORY;
    }

    size_t page = OUTPUT_0_PAGES + (created_count % CREATED_BUFFER_SLOTS) * CREATED_BUFFER_PAGES;
    gfxfb::buffer& buf = created[created_count];
    buf = {
        .width     = width,
        .height    = height,
        .pitch     = static_cast<uint32_t>(pitch),
        .size      = CREATED_BUFFER_SIZE,
        .phys      = g_buffer_phys + page * pmm::PAGE_SIZE,
        .cacheable = false,
    };

    created_count++;
    *out = &buf;

    return gfxfb::OK;
}

int32_t recording_display::present(
    uint32_t output,
    gfxfb::buffer* buf,
    const gfxfb::gfxfb_rect* regions,
    uint32_t region_count
) {
    present_calls++;

    if (present_answer != gfxfb::OK) {
        return present_answer;
    }

    if (!states[output].lit) {
        return gfxfb::ERR_DARK;
    }

    const gfxfb::gfxfb_mode& mode = states[output].current;
    bool fits_mode = buf->width == mode.width && buf->height == mode.height;
    if (!fits_mode) {
        return gfxfb::ERR_INVALID_ARG;
    }

    presented = buf;
    presented_output = output;
    presented_on[output] = buf;
    named_no_region_on[output] = regions == nullptr && region_count == 0;
    got_null_regions = regions == nullptr;
    presented_region_count = region_count;
    uint32_t readable_count = regions ? region_count : 0;
    for (uint32_t i = 0; i < readable_count && i < MAX_RECORDED; i++) {
        presented_regions[i] = regions[i];
    }

    last_presented_region = readable_count > 0 ? regions[readable_count - 1] : gfxfb::gfxfb_rect{};

    if (while_presenting) {
        void (*hook)() = while_presenting;
        while_presenting = nullptr;
        hook();
    }

    return gfxfb::OK;
}

// Lights `output` of `fake`, the boot output when `is_boot_output` holds. Output 0's base buffer is cacheable at the
// start of the shared memory, and output 1's is write-combining a page in, so a test can tell which one is mapped
static void light_output(recording_display& fake, uint32_t output, bool is_boot_output) {
    bool is_first = output == 0;

    fake.bases[output] = {
        .width     = static_cast<uint32_t>(is_first ? OUTPUT_0_WIDTH : OUTPUT_1_WIDTH),
        .height    = static_cast<uint32_t>(is_first ? OUTPUT_0_HEIGHT : OUTPUT_1_HEIGHT),
        .pitch     = static_cast<uint32_t>(is_first ? OUTPUT_0_PITCH : OUTPUT_1_PITCH),
        .size      = pmm::page_align_up(is_first ? OUTPUT_0_SIZE : OUTPUT_1_SIZE),
        .phys      = is_first ? g_buffer_phys : g_buffer_phys + OUTPUT_1_OFFSET,
        .cacheable = is_first,
    };

    fake.states[output] = {
        .lit         = true,
        .boot_output = is_boot_output,
        .current     = {
            .width              = fake.bases[output].width,
            .height             = fake.bases[output].height,
            .refresh_millihertz = is_first ? OUTPUT_0_REFRESH_MILLIHERTZ : OUTPUT_1_REFRESH_MILLIHERTZ,
        },
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
            .size      = pmm::page_align_up(BOOT_SIZE),
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

// Publishes `node` under /dev/gfxfb_test, or returns nullptr and leaves allocated a node devfs refused, as gfxfb does
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

// Calls an ioctl on `file` inside the page's address space, as a syscall body would
static int32_t file_ioctl(fs::file* file, user_page& page, uint32_t cmd, uint64_t arg) {
    user_space_scope scope(page.ctx);
    return fs::ioctl(file, cmd, arg);
}

// Calls an ioctl on the node at `path` through a file opened for the call
static int32_t call_gfxfb_ioctl(const char* path, user_page& page, uint32_t cmd, uint64_t arg) {
    int32_t open_err = fs::OK;
    fs::file* file = fs::open(path, fs::O_RDWR, &open_err);
    if (!file) {
        return open_err;
    }

    int32_t rc = file_ioctl(file, page, cmd, arg);
    fs::close(file);

    return rc;
}

// Asks `file` for a buffer through GFXFB_CREATE_BUFFER, whose request and reply sit at the page's start
static int32_t request_buffer(fs::file* file, user_page& page, uint32_t width, uint32_t height, uint32_t format) {
    *page.at<gfxfb::gfxfb_buffer_args>(0) = {
        .width  = width,
        .height = height,
        .format = format,
    };

    return file_ioctl(file, page, gfxfb::GFXFB_CREATE_BUFFER, page.addr);
}

// Creates a SMALL_BUFFER_WIDTH by SMALL_BUFFER_HEIGHT XRGB8888 buffer and returns the reply, with a zero handle when
// the creation failed
static gfxfb::gfxfb_buffer_args request_small_buffer(fs::file* file, user_page& page) {
    if (request_buffer(file, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, gfxfb::GFXFB_FORMAT_XRGB8888) != fs::OK) {
        return {};
    }

    return *page.at<gfxfb::gfxfb_buffer_args>(0);
}

// Writes the small buffer's creation request at the page's start, with `reserved` in its reserved field
static void write_small_request(user_page& page, uint32_t reserved) {
    *page.at<gfxfb::gfxfb_buffer_args>(0) = {
        .width    = SMALL_BUFFER_WIDTH,
        .height   = SMALL_BUFFER_HEIGHT,
        .format   = gfxfb::GFXFB_FORMAT_XRGB8888,
        .reserved = reserved,
    };
}

// Drops `handle` through GFXFB_DESTROY_BUFFER on `file`
static int32_t destroy_buffer(fs::file* file, user_page& page, uint64_t handle) {
    return file_ioctl(file, page, gfxfb::GFXFB_DESTROY_BUFFER, handle);
}

// Creates a buffer of output 0's width and height on `file` and returns its handle, or 0 when the creation failed
static uint32_t create_output_0_buffer(fs::file* file, user_page& page) {
    uint32_t width = static_cast<uint32_t>(OUTPUT_0_WIDTH);
    uint32_t height = static_cast<uint32_t>(OUTPUT_0_HEIGHT);
    if (request_buffer(file, page, width, height, gfxfb::GFXFB_FORMAT_XRGB8888) != fs::OK) {
        return 0;
    }

    return page.at<gfxfb::gfxfb_buffer_args>(0)->handle;
}

// Presents `handle` on `output` through GFXFB_PRESENT on `file` with the `region_count` regions at user address
// `regions`, writing the request at the page's start
static int32_t present_regions(
    fs::file* file,
    user_page& page,
    uint32_t output,
    uint32_t handle,
    uint64_t regions,
    uint32_t region_count
) {
    *page.at<gfxfb::gfxfb_present_args>(ARGS_OFFSET) = {
        .output       = output,
        .handle       = handle,
        .regions      = regions,
        .region_count = region_count,
    };

    return file_ioctl(file, page, gfxfb::GFXFB_PRESENT, page.addr + ARGS_OFFSET);
}

// Presents `handle` on `output` through GFXFB_PRESENT on `file` naming no region, which stands for the whole buffer
static int32_t present_buffer(fs::file* file, user_page& page, uint32_t output, uint32_t handle) {
    return present_regions(file, page, output, handle, 0, 0);
}

// Presents `handle` on `output` through GFXFB_PRESENT on `file` with `region` alone, written at RECTS_OFFSET
static int32_t present_one_region(
    fs::file* file,
    user_page& page,
    uint32_t output,
    uint32_t handle,
    const gfxfb::gfxfb_rect& region
) {
    *page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET) = region;

    return present_regions(file, page, output, handle, page.addr + RECTS_OFFSET, 1);
}

// Writes at the page's start a present of output 1's base buffer naming one empty region, refused before control
static void write_empty_region_present(user_page& page) {
    *page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET) = {0, 0, 0, 1};
    *page.at<gfxfb::gfxfb_present_args>(ARGS_OFFSET) = {
        .output       = 1,
        .handle       = 0,
        .regions      = page.addr + RECTS_OFFSET,
        .region_count = 1,
    };
}

// Region `i` of `map_many_regions`: one pixel at column i / OUTPUT_0_HEIGHT and row i % OUTPUT_0_HEIGHT of output 0's
// buffer, so no two regions are alike and both coordinates vary among the first MAX_RECORDED
static gfxfb::gfxfb_rect many_regions_entry(uint32_t i) {
    return {
        .x      = static_cast<uint32_t>(i / OUTPUT_0_HEIGHT),
        .y      = static_cast<uint32_t>(i % OUTPUT_0_HEIGHT),
        .width  = 1,
        .height = 1,
    };
}

// Maps MANY_REGIONS_PAGES pages into the page's address space holding MANY_REGION_COUNT regions, region i being
// `many_regions_entry(i)`, and returns their user address, or 0 when the mapping or a write fails
static uintptr_t map_many_regions(user_page& page) {
    uintptr_t addr = 0;
    uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
    uint32_t flags = mm::MM_MAP_PRIVATE | mm::MM_MAP_ANONYMOUS;
    size_t length = MANY_REGIONS_PAGES * pmm::PAGE_SIZE;
    int32_t map_rc = mm::MM_CTX_OK;
    RUN_ELEVATED(map_rc = mm::mm_context_map_anonymous(page.ctx, 0, length, prot, flags, &addr));
    if (map_rc != mm::MM_CTX_OK) {
        return 0;
    }

    user_space_scope scope(page.ctx);
    for (uint32_t i = 0; i < MANY_REGION_COUNT; i++) {
        gfxfb::gfxfb_rect region = many_regions_entry(i);
        void* destination = reinterpret_cast<void*>(addr + i * sizeof(region));
        int32_t copy_rc = mm::uaccess::OK;
        RUN_ELEVATED(copy_rc = mm::uaccess::copy_to_user(destination, &region, sizeof(region)));
        if (copy_rc != mm::uaccess::OK) {
            return 0;
        }
    }

    return addr;
}

// Ends the unended present the test's node holds on `output` with `status`, as its display would, reporting `frame` and
// `timestamp_ns` with it
static void end_present(
    const gfxfb_test_node& test,
    uint32_t output,
    uint32_t status,
    uint64_t frame = 0,
    uint64_t timestamp_ns = 0
) {
    RUN_ELEVATED(test.node->present_done(output, {.status = status, .frame = frame, .timestamp_ns = timestamp_ns}));
}

// Reads one event from `file` without waiting, or returns a record of zeros when the read fails
static gfxfb::gfxfb_event read_event(fs::file* file) {
    gfxfb::gfxfb_event event = {};
    if (fs::read(file, &event, sizeof(event), fs::O_NONBLOCK) != static_cast<ssize_t>(sizeof(event))) {
        return {};
    }

    return event;
}

// What `file`'s node reports `file` ready for, subscribing nothing
static uint32_t poll_file(fs::file* file) {
    uint32_t ready = 0;
    RUN_ELEVATED(ready = file->get_node()->poll(file, nullptr));

    return ready;
}

// Closes the file of `arg`, a `task_close`, then sets its flag
static void close_on_a_task(void* arg) {
    task_close* request = static_cast<task_close*>(arg);
    fs::close(request->file);
    request->done.store_release(1);
    sched::exit(0);
}

// Starts closing `file` on a task of its own, which sets `request.done` once the close returns, or returns nullptr when
// the task cannot start
static sched::task* start_close(task_close& request, fs::file* file) {
    request.file = file;
    request.done.store_relaxed(0);

    return test_helpers::start_pinned_task(close_on_a_task, &request, "gfxfb_close");
}

// True when `file`'s close, run on a task of its own, returns before the tests' deadline, so a close that never returns
// fails its test instead of hanging the suite
static bool closes_before_deadline(fs::file* file) {
    sched::task* closer = start_close(g_deadline_close, file);
    bool is_closed = closer != nullptr && spin_wait(g_deadline_close.done);
    if (is_closed) {
        unpin(closer);
    }

    return is_closed;
}

// True when `file`'s close, run on a task of its own, presents `fake`'s base buffer on `output` naming no region and
// waits for it, then returns before the tests' deadline once the helper ends that present latched
static bool closes_giving_output_back(
    const gfxfb_test_node& test,
    const recording_display& fake,
    fs::file* file,
    uint32_t output
) {
    sched::task* closer = start_close(g_deadline_close, file);
    if (!closer || !test_helpers::blocks_before_deadline(closer)) {
        return false;
    }

    bool presented_base = fake.presented == &fake.bases[output] && fake.presented_output == output;
    bool named_no_region = fake.got_null_regions && fake.presented_region_count == 0;
    if (!presented_base || !named_no_region) {
        return false;
    }

    end_present(test, output, gfxfb::GFXFB_PRESENT_LATCHED);
    bool is_closed = spin_wait(g_deadline_close.done);
    if (is_closed) {
        unpin(closer);
    }

    return is_closed;
}

// Reads the file of `arg`, a `task_read`, into the deadline read's events, then records the result and sets its flag
static void read_on_a_task(void* arg) {
    task_read* request = static_cast<task_read*>(arg);
    request->result = fs::read(request->file, g_deadline_read_events, request->size, request->flags);
    request->done.store_release(1);
    sched::exit(0);
}

// True when a read of `size` bytes of `file` with `flags`, run on a task of its own, returns `expected` before the
// tests' deadline. A read still waiting then is killed, so it takes no later event and its test fails, never hangs
static bool reads_before_deadline(fs::file* file, size_t size, uint32_t flags, ssize_t expected) {
    g_deadline_read.file = file;
    g_deadline_read.size = size;
    g_deadline_read.flags = flags;
    g_deadline_read.done.store_relaxed(0);

    sched::task* reader = test_helpers::start_pinned_task(read_on_a_task, &g_deadline_read, "gfxfb_read");
    if (!reader) {
        return false;
    }

    bool returned_in_time = spin_wait(g_deadline_read.done);
    if (!returned_in_time) {
        RUN_ELEVATED(sched::force_wake_for_kill(reader));
    }

    if (spin_wait(g_deadline_read.done)) {
        unpin(reader);
    }

    return returned_in_time && g_deadline_read.result == expected;
}

// Asks the file of `arg`, a `task_create`, for the small buffer, then records the result and sets its flag
static void create_on_a_task(void* arg) {
    task_create* request = static_cast<task_create*>(arg);
    uint32_t format = gfxfb::GFXFB_FORMAT_XRGB8888;
    request->result = request_buffer(request->file, *request->page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format);
    request->done.store_release(1);
    sched::exit(0);
}

// True when a creation of the small buffer on `file` through `page`, run on a task of its own, returns `expected`
// before the tests' deadline, so a creation that never returns fails its test instead of hanging the suite
static bool creates_before_deadline(fs::file* file, user_page& page, int32_t expected) {
    g_deadline_create.file = file;
    g_deadline_create.page = &page;
    g_deadline_create.done.store_relaxed(0);

    sched::task* creator = test_helpers::start_pinned_task(create_on_a_task, &g_deadline_create, "gfxfb_create");
    bool returned_in_time = creator != nullptr && spin_wait(g_deadline_create.done);
    if (returned_in_time) {
        unpin(creator);
    }

    return returned_in_time && g_deadline_create.result == expected;
}

// Maps `length` bytes of `file` from `offset` into `ctx`, readable and writable, returning the address or 0
static uintptr_t map_file(fs::file* file, mm::mm_context* ctx, uint64_t offset, size_t length) {
    uintptr_t addr = 0;
    uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
    if (fs::mmap(file, ctx, 0, length, prot, mm::MM_MAP_SHARED, offset, &addr) != mm::MM_CTX_OK) {
        return 0;
    }

    return addr;
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

// Reads `output`'s state through GFXFB_GET_OUTPUT on the node at `path` into the page's start
static int32_t get_output(const char* path, user_page& page, uint32_t output) {
    *page.at<gfxfb::gfxfb_output_info>(0) = {
        .output       = output,
        .output_count = UNWRITTEN_FIELD,
        .flags        = UNWRITTEN_FIELD,
        .reserved     = UNWRITTEN_FIELD,
        .current      = {
            .width              = UNWRITTEN_FIELD,
            .height             = UNWRITTEN_FIELD,
            .refresh_millihertz = UNWRITTEN_RATE,
        },
    };

    return call_gfxfb_ioctl(path, page, gfxfb::GFXFB_GET_OUTPUT, page.addr);
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

static recording_display g_outputs_display;

// GFXFB_GET_OUTPUT writes every reply field, an output's mode only while it is lit, and refuses an index of the output
// count or more, a null or unreadable argument, and with no field written, a request ending on an unmapped page
TEST(gfxfb, get_output_reports_each_output) {
    light_output(g_outputs_display, 0, false);
    g_outputs_display.states[0].current.width = WIDE_MODE_WIDTH;
    g_outputs_display.states[0].current.height = WIDE_MODE_HEIGHT;
    g_outputs_display.states[1].current = {.width = 1, .height = 1, .refresh_millihertz = 1}; // stale while dark
    gfxfb_test_node test = publish_driver_node(&g_outputs_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    auto* info = page.at<gfxfb::gfxfb_output_info>(0);

    ASSERT_EQ(get_output(test.path, page, 0), fs::OK);
    EXPECT_EQ(info->output, static_cast<uint32_t>(0));
    EXPECT_EQ(info->output_count, recording_display::OUTPUTS);
    EXPECT_EQ(info->flags, gfxfb::GFXFB_OUTPUT_LIT | gfxfb::GFXFB_OUTPUT_PRIMARY);
    EXPECT_EQ(info->reserved, static_cast<uint32_t>(0));
    EXPECT_EQ(info->current.width, WIDE_MODE_WIDTH);
    EXPECT_EQ(info->current.height, WIDE_MODE_HEIGHT);
    EXPECT_EQ(info->current.refresh_millihertz, OUTPUT_0_REFRESH_MILLIHERTZ);

    ASSERT_EQ(get_output(test.path, page, 1), fs::OK);
    EXPECT_EQ(info->output, static_cast<uint32_t>(1));
    EXPECT_EQ(info->output_count, recording_display::OUTPUTS);
    EXPECT_EQ(info->flags, static_cast<uint32_t>(0));
    EXPECT_EQ(info->reserved, static_cast<uint32_t>(0));
    EXPECT_EQ(info->current.width, static_cast<uint32_t>(0));
    EXPECT_EQ(info->current.height, static_cast<uint32_t>(0));
    EXPECT_EQ(info->current.refresh_millihertz, static_cast<uint64_t>(0));

    EXPECT_EQ(get_output(test.path, page, recording_display::OUTPUTS), fs::ERR_INVAL);
    EXPECT_EQ(get_output(test.path, page, WIDE_OUTPUT_INDEX), fs::ERR_INVAL);
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_OUTPUT, 0), fs::ERR_INVAL);
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_OUTPUT, page.addr + pmm::PAGE_SIZE), fs::ERR_INVAL);

    // A request whose mode lies on the unmapped page after `page` is refused before any reply field is written
    size_t crossing_offset = pmm::PAGE_SIZE - __builtin_offsetof(gfxfb::gfxfb_output_info, current);
    auto* crossing_request = page.at<gfxfb::gfxfb_output_info>(crossing_offset);
    crossing_request->output = 0;
    crossing_request->output_count = UNWRITTEN_FIELD;
    crossing_request->flags = UNWRITTEN_FIELD;
    crossing_request->reserved = UNWRITTEN_FIELD;

    uint64_t crossing_addr = page.addr + crossing_offset;
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_OUTPUT, crossing_addr), fs::ERR_INVAL);
    EXPECT_EQ(crossing_request->output_count, UNWRITTEN_FIELD);
    EXPECT_EQ(crossing_request->flags, UNWRITTEN_FIELD);
    EXPECT_EQ(crossing_request->reserved, UNWRITTEN_FIELD);
}

// The boot display's one output is primary and lit at the boot framebuffer's size, with an unknown refresh rate
TEST(gfxfb, get_output_reports_the_boot_display) {
    gfxfb_test_node test = publish_boot_node();
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    auto* info = page.at<gfxfb::gfxfb_output_info>(0);

    ASSERT_EQ(get_output(test.path, page, 0), fs::OK);
    EXPECT_EQ(info->output_count, static_cast<uint32_t>(1));
    EXPECT_EQ(info->flags, gfxfb::GFXFB_OUTPUT_LIT | gfxfb::GFXFB_OUTPUT_PRIMARY);
    EXPECT_EQ(info->current.width, BOOT_WIDTH);
    EXPECT_EQ(info->current.height, BOOT_HEIGHT);
    EXPECT_EQ(info->current.refresh_millihertz, static_cast<uint64_t>(0));

    EXPECT_EQ(get_output(test.path, page, 1), fs::ERR_INVAL);
}

static recording_display g_primary_flag_display;

// Only the primary output carries the primary flag: the lit boot output 1, with output 0 also lit
TEST(gfxfb, get_output_flags_the_primary_output) {
    light_output(g_primary_flag_display, 0, false);
    light_output(g_primary_flag_display, 1, true);
    gfxfb_test_node test = publish_driver_node(&g_primary_flag_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    auto* info = page.at<gfxfb::gfxfb_output_info>(0);

    ASSERT_EQ(get_output(test.path, page, 0), fs::OK);
    EXPECT_EQ(info->flags, gfxfb::GFXFB_OUTPUT_LIT);
    EXPECT_EQ(info->current.refresh_millihertz, OUTPUT_0_REFRESH_MILLIHERTZ);

    ASSERT_EQ(get_output(test.path, page, 1), fs::OK);
    EXPECT_EQ(info->flags, gfxfb::GFXFB_OUTPUT_LIT | gfxfb::GFXFB_OUTPUT_PRIMARY);
    EXPECT_EQ(info->current.width, OUTPUT_1_WIDTH);
    EXPECT_EQ(info->current.height, OUTPUT_1_HEIGHT);
    EXPECT_EQ(info->current.refresh_millihertz, OUTPUT_1_REFRESH_MILLIHERTZ);
}

static recording_display g_baseless_display;

// A display giving no base buffer for its lit output 0, against `base_buffer()`'s contract, leaves no primary output:
// GFXFB_GET_OUTPUT flags output 0 lit but not primary, and GFXFB_GET_INFO fails with ERR_AGAIN
TEST(gfxfb, lit_output_without_a_base_buffer_is_not_primary) {
    light_output(g_baseless_display, 0, false);
    g_baseless_display.has_base_buffers = false;
    gfxfb_test_node test = publish_driver_node(&g_baseless_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    ASSERT_EQ(get_output(test.path, page, 0), fs::OK);
    EXPECT_EQ(page.at<gfxfb::gfxfb_output_info>(0)->flags, gfxfb::GFXFB_OUTPUT_LIT);
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_GET_INFO, page.addr), fs::ERR_AGAIN);
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

// A display's ERR_DARK and ERR_REFUSED from `flush()` fail only their GFXFB_FLUSH, with ERR_AGAIN and ERR_IO, while its
// ERR_DEVICE fails it and every later flush and present past its request checks with ERR_IO without a display call
TEST(gfxfb, flush_returns_the_display_failures_as_file_errors) {
    light_output(g_failing_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_failing_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());

    g_failing_display.flush_result = gfxfb::ERR_DARK;
    EXPECT_EQ(flush_one_rect(test.path, page), fs::ERR_AGAIN);
    g_failing_display.flush_result = gfxfb::ERR_REFUSED;
    EXPECT_EQ(flush_one_rect(test.path, page), fs::ERR_IO);

    g_failing_display.flush_result = gfxfb::ERR_DEVICE;
    EXPECT_EQ(flush_one_rect(test.path, page), fs::ERR_IO);
    EXPECT_EQ(g_failing_display.recorded_count, static_cast<uint32_t>(3));

    g_failing_display.flush_result = gfxfb::OK;
    EXPECT_EQ(flush_one_rect(test.path, page), fs::ERR_IO);
    page.at<gfxfb::gfxfb_flush_args>(ARGS_OFFSET)->reserved = 1;
    EXPECT_EQ(call_gfxfb_ioctl(test.path, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::ERR_INVAL);
    EXPECT_EQ(g_failing_display.recorded_count, static_cast<uint32_t>(3));

    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);
    EXPECT_EQ(present_buffer(file, page, 0, 0), fs::ERR_IO);
    EXPECT_EQ(g_failing_display.present_calls, static_cast<uint32_t>(0));
    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_dark_failed_display;

// A creation's ERR_DEVICE counts the display as failed, so FLUSH then fails with ERR_IO even with no primary output
TEST(gfxfb, a_creation_failing_the_display_fails_flush_even_with_no_primary_output) {
    gfxfb_test_node test = publish_driver_node(&g_dark_failed_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    EXPECT_EQ(flush_one_rect(test.path, page), fs::ERR_AGAIN);
    g_dark_failed_display.create_result = gfxfb::ERR_DEVICE;
    EXPECT_EQ(request_small_buffer(file, page).handle, static_cast<uint32_t>(0));
    EXPECT_EQ(flush_one_rect(test.path, page), fs::ERR_IO);

    ASSERT_TRUE(closes_before_deadline(file));
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

// A display registered with every output dark has no primary: GFXFB_GET_OUTPUT sets no flag for either output, stat
// reports size 0, GFXFB_GET_INFO and GFXFB_FLUSH fail with ERR_AGAIN, mmap is refused, and `flush()` is never called
TEST(gfxfb, display_with_every_output_dark_has_no_primary) {
    gfxfb_test_node test = publish_driver_node(&g_all_dark_display);
    ASSERT_NOT_NULL(test.node);
    EXPECT_EQ(stat_size(test), static_cast<size_t>(0));

    user_page page;
    ASSERT_TRUE(page.ready());

    for (uint32_t output = 0; output < recording_display::OUTPUTS; output++) {
        ASSERT_EQ(get_output(test.path, page, output), fs::OK);
        EXPECT_EQ(page.at<gfxfb::gfxfb_output_info>(0)->flags, static_cast<uint32_t>(0));
    }

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

static recording_display g_create_display;

// GFXFB_CREATE_BUFFER passes the request to the display and replies with a fresh handle, the display's pitch and size
// and a page-aligned mmap offset, each buffer its own
TEST(gfxfb, create_buffer_replies_with_the_buffer_layout) {
    light_output(g_create_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_create_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    gfxfb::gfxfb_buffer_args first = request_small_buffer(file, page);
    gfxfb::gfxfb_buffer_args second = request_small_buffer(file, page);

    EXPECT_NE(first.handle, static_cast<uint32_t>(0));
    EXPECT_NE(second.handle, static_cast<uint32_t>(0));
    EXPECT_NE(first.handle, second.handle);
    EXPECT_EQ(first.pitch, SMALL_BUFFER_PITCH);
    EXPECT_EQ(first.size, CREATED_BUFFER_SIZE);
    EXPECT_NE(first.map_offset, static_cast<uint64_t>(0));
    EXPECT_NE(first.map_offset, second.map_offset);
    EXPECT_ALIGNED(first.map_offset, pmm::PAGE_SIZE);
    EXPECT_ALIGNED(second.map_offset, pmm::PAGE_SIZE);

    ASSERT_EQ(g_create_display.created_count, static_cast<uint32_t>(2));
    EXPECT_EQ(g_create_display.created[0].width, SMALL_BUFFER_WIDTH);
    EXPECT_EQ(g_create_display.created[0].height, SMALL_BUFFER_HEIGHT);

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_refusing_display;

// gfxfb refuses a zero size, a nonzero reserved field or an unreadable request without asking the display, passes the
// width and format whole for the display to refuse, maps those refusals to file codes, and a refusal frees its handle
TEST(gfxfb, create_buffer_refuses_bad_requests) {
    light_output(g_refusing_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_refusing_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t format = gfxfb::GFXFB_FORMAT_XRGB8888;
    EXPECT_EQ(request_buffer(file, page, 0, SMALL_BUFFER_HEIGHT, format), fs::ERR_INVAL);
    EXPECT_EQ(request_buffer(file, page, SMALL_BUFFER_WIDTH, 0, format), fs::ERR_INVAL);

    write_small_request(page, 1);
    EXPECT_EQ(file_ioctl(file, page, gfxfb::GFXFB_CREATE_BUFFER, page.addr), fs::ERR_INVAL);
    write_small_request(page, 0x80000000);
    EXPECT_EQ(file_ioctl(file, page, gfxfb::GFXFB_CREATE_BUFFER, page.addr), fs::ERR_INVAL);

    // A request ending on the unmapped page after `page` is refused without asking the display
    size_t crossing_offset = pmm::PAGE_SIZE - __builtin_offsetof(gfxfb::gfxfb_buffer_args, size);
    auto* crossing_request = page.at<gfxfb::gfxfb_buffer_args>(crossing_offset);
    crossing_request->width = SMALL_BUFFER_WIDTH;
    crossing_request->height = SMALL_BUFFER_HEIGHT;
    crossing_request->format = format;
    crossing_request->reserved = 0;

    uint64_t crossing_addr = page.addr + crossing_offset;
    EXPECT_EQ(file_ioctl(file, page, gfxfb::GFXFB_CREATE_BUFFER, crossing_addr), fs::ERR_INVAL);
    EXPECT_EQ(g_refusing_display.create_calls, static_cast<uint32_t>(0));

    EXPECT_EQ(request_buffer(file, page, WIDE_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_INVAL);
    EXPECT_EQ(request_buffer(file, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);

    g_refusing_display.create_result = gfxfb::ERR_NO_MEMORY;
    EXPECT_EQ(request_buffer(file, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_NOMEM);
    g_refusing_display.create_result = gfxfb::ERR_REFUSED;
    EXPECT_EQ(request_buffer(file, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_IO);
    g_refusing_display.create_result = gfxfb::ERR_DEVICE;
    EXPECT_EQ(request_buffer(file, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_IO);
    g_refusing_display.create_result = gfxfb::OK;

    // No refusal created a buffer or took a handle, so the next creation gets handle 1, on a fake still creating once
    // failed, which no display keeping the contract would do
    EXPECT_EQ(g_refusing_display.create_calls, static_cast<uint32_t>(5));
    EXPECT_EQ(g_refusing_display.created_count, static_cast<uint32_t>(0));
    EXPECT_EQ(request_small_buffer(file, page).handle, static_cast<uint32_t>(1));

    // Only the one buffer created comes back once its handle goes, so no refusal reached `release_buffer()`
    ASSERT_EQ(destroy_buffer(file, page, 1), fs::OK);
    ASSERT_TRUE(spin_wait_ge(g_refusing_display.released_count, 1));
    EXPECT_EQ(g_refusing_display.released_count.load_acquire(), static_cast<uint32_t>(1));
    EXPECT_EQ(g_refusing_display.last_released, &g_refusing_display.created[0]);

    ASSERT_TRUE(closes_before_deadline(file));
}

// The boot display creates no buffers, so GFXFB_CREATE_BUFFER fails with ERR_NOSYS on a boot node
TEST(gfxfb, boot_display_creates_no_buffers) {
    gfxfb_test_node test = publish_boot_node();
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t format = gfxfb::GFXFB_FORMAT_XRGB8888;
    EXPECT_EQ(request_buffer(file, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_NOSYS);

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_unreplied_display;

// A creation whose reply cannot be written hands the new buffer back to its display and leaves its handle free
TEST(gfxfb, a_creation_that_cannot_reply_hands_its_buffer_back) {
    light_output(g_unreplied_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_unreplied_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    // A read-only page lets gfxfb read the request but not write the reply
    write_small_request(page, 0);
    ASSERT_EQ(mm::mm_context_mprotect(page.ctx, page.addr, pmm::PAGE_SIZE, mm::MM_PROT_READ), mm::MM_CTX_OK);
    EXPECT_EQ(file_ioctl(file, page, gfxfb::GFXFB_CREATE_BUFFER, page.addr), fs::ERR_INVAL);
    ASSERT_TRUE(spin_wait_ge(g_unreplied_display.released_count, 1));
    EXPECT_EQ(g_unreplied_display.last_released, &g_unreplied_display.created[0]);

    uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
    ASSERT_EQ(mm::mm_context_mprotect(page.ctx, page.addr, pmm::PAGE_SIZE, prot), mm::MM_CTX_OK);
    EXPECT_EQ(request_small_buffer(file, page).handle, static_cast<uint32_t>(1));

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_full_display;

// A file holds at most GFXFB_CLIENT_BUFFERS_MAX handles, the last of which maps like any other, and a creation reuses
// the lowest handle a destroy freed
TEST(gfxfb, a_file_holds_at_most_client_buffers_max_handles) {
    light_output(g_full_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_full_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    gfxfb::gfxfb_buffer_args last = {};
    for (uint32_t i = 0; i < gfxfb::GFXFB_CLIENT_BUFFERS_MAX; i++) {
        last = request_small_buffer(file, page);
        ASSERT_NE(last.handle, static_cast<uint32_t>(0));
    }

    uint32_t format = gfxfb::GFXFB_FORMAT_XRGB8888;
    EXPECT_EQ(request_buffer(file, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_NOMEM);
    EXPECT_EQ(g_full_display.create_calls, gfxfb::GFXFB_CLIENT_BUFFERS_MAX);
    EXPECT_EQ(last.handle, gfxfb::GFXFB_CLIENT_BUFFERS_MAX);

    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    EXPECT_NE(map_file(file, ctx, last.map_offset, last.size), static_cast<uintptr_t>(0));

    ASSERT_EQ(destroy_buffer(file, page, gfxfb::GFXFB_CLIENT_BUFFERS_MAX), fs::OK);
    ASSERT_EQ(destroy_buffer(file, page, 5), fs::OK);
    EXPECT_EQ(request_small_buffer(file, page).handle, static_cast<uint32_t>(5));
    EXPECT_EQ(request_small_buffer(file, page).handle, gfxfb::GFXFB_CLIENT_BUFFERS_MAX);

    ASSERT_TRUE(closes_before_deadline(file));
    mm::mm_context_release(ctx);
}

static recording_display g_nested_display;

// The file and page the nested creation asks through, and the result it got
static fs::file* g_nested_file;
static user_page* g_nested_page;
static int32_t g_nested_result;

// Asks for the small buffer from inside the display's `create_buffer()`, in the address space the outer request's ioctl
// entered, since `user_space_scope` does not nest
static void request_while_creating() {
    write_small_request(*g_nested_page, 0);
    g_nested_result = fs::ioctl(g_nested_file, gfxfb::GFXFB_CREATE_BUFFER, g_nested_page->addr);
}

// A creation holds its handle while the display creates the buffer, so a creation that runs meanwhile on the same file
// cannot take that handle
TEST(gfxfb, a_creation_holds_its_handle_while_the_display_creates) {
    light_output(g_nested_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_nested_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    for (uint32_t i = 1; i < gfxfb::GFXFB_CLIENT_BUFFERS_MAX; i++) {
        ASSERT_NE(request_small_buffer(file, page).handle, static_cast<uint32_t>(0));
    }

    g_nested_file = file;
    g_nested_page = &page;
    g_nested_result = fs::OK;
    g_nested_display.while_creating = request_while_creating;
    EXPECT_EQ(request_small_buffer(file, page).handle, gfxfb::GFXFB_CLIENT_BUFFERS_MAX);
    EXPECT_EQ(g_nested_result, fs::ERR_NOMEM);
    EXPECT_EQ(g_nested_display.create_calls, gfxfb::GFXFB_CLIENT_BUFFERS_MAX);

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_destroy_display;

// GFXFB_DESTROY_BUFFER hands an unmapped buffer back to its display, and refuses a handle the file does not hold
TEST(gfxfb, destroy_buffer_hands_the_buffer_back) {
    light_output(g_destroy_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_destroy_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t handle = request_small_buffer(file, page).handle;
    ASSERT_NE(handle, static_cast<uint32_t>(0));

    // A handle in the argument's high half names no buffer, even with the low half holding a real one
    EXPECT_EQ(destroy_buffer(file, page, (1ULL << 32) | handle), fs::ERR_INVAL);
    EXPECT_EQ(g_destroy_display.released_count.load_acquire(), static_cast<uint32_t>(0));

    ASSERT_EQ(destroy_buffer(file, page, handle), fs::OK);
    ASSERT_TRUE(spin_wait_ge(g_destroy_display.released_count, 1));
    EXPECT_EQ(g_destroy_display.last_released, &g_destroy_display.created[0]);

    EXPECT_EQ(destroy_buffer(file, page, handle), fs::ERR_INVAL);
    EXPECT_EQ(destroy_buffer(file, page, 0), fs::ERR_INVAL);
    EXPECT_EQ(destroy_buffer(file, page, gfxfb::GFXFB_CLIENT_BUFFERS_MAX + 1), fs::ERR_INVAL);
    EXPECT_EQ(g_destroy_display.released_count.load_acquire(), static_cast<uint32_t>(1));

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_buffer_mmap_display;

// mmap at a buffer's offset maps that buffer with its own caching, and an offset below the first buffer's maps into the
// primary output's base buffer
TEST(gfxfb, mmap_maps_each_buffer_at_its_offset) {
    light_output(g_buffer_mmap_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_buffer_mmap_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    gfxfb::gfxfb_buffer_args first = request_small_buffer(file, page);
    gfxfb::gfxfb_buffer_args second = request_small_buffer(file, page);
    ASSERT_NE(second.handle, static_cast<uint32_t>(0));

    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    uintptr_t base_addr = map_file(file, ctx, pmm::PAGE_SIZE, pmm::PAGE_SIZE);
    uintptr_t first_addr = map_file(file, ctx, first.map_offset, first.size);
    uintptr_t second_addr = map_file(file, ctx, second.map_offset + pmm::PAGE_SIZE, pmm::PAGE_SIZE);
    ASSERT_NE(base_addr, static_cast<uintptr_t>(0));
    ASSERT_NE(first_addr, static_cast<uintptr_t>(0));
    ASSERT_NE(second_addr, static_cast<uintptr_t>(0));

    const gfxfb::buffer* created = g_buffer_mmap_display.created;
    EXPECT_EQ(paging::get_physical(base_addr, ctx->pt_root), g_buffer_phys + pmm::PAGE_SIZE);
    EXPECT_EQ(paging::get_physical(first_addr, ctx->pt_root), created[0].phys);
    EXPECT_EQ(paging::get_physical(first_addr + pmm::PAGE_SIZE, ctx->pt_root), created[0].phys + pmm::PAGE_SIZE);
    EXPECT_EQ(paging::get_physical(second_addr, ctx->pt_root), created[1].phys + pmm::PAGE_SIZE);

    paging::page_flags_t base_flags = paging::get_page_flags(base_addr, ctx->pt_root);
    paging::page_flags_t first_flags = paging::get_page_flags(first_addr, ctx->pt_root);
    EXPECT_EQ(base_flags & paging::PAGE_TYPE_MASK, paging::PAGE_NORMAL);
    EXPECT_EQ(first_flags & paging::PAGE_TYPE_MASK, paging::PAGE_WC);

    ASSERT_TRUE(closes_before_deadline(file));
    mm::mm_context_release(ctx);
}

static recording_display g_mmap_bounds_display;

// mmap refuses a range running past a buffer's size, an offset whose handle names no buffer, and an offset so large
// that adding the length wraps
TEST(gfxfb, mmap_refuses_ranges_outside_a_buffer) {
    light_output(g_mmap_bounds_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_mmap_bounds_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    gfxfb::gfxfb_buffer_args kept = request_small_buffer(file, page);
    gfxfb::gfxfb_buffer_args destroyed = request_small_buffer(file, page);
    ASSERT_NE(destroyed.handle, static_cast<uint32_t>(0));
    ASSERT_EQ(destroy_buffer(file, page, destroyed.handle), fs::OK);

    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    EXPECT_EQ(map_file(file, ctx, kept.map_offset, kept.size + 1), static_cast<uintptr_t>(0));
    EXPECT_EQ(map_file(file, ctx, kept.map_offset + pmm::PAGE_SIZE, kept.size), static_cast<uintptr_t>(0));
    EXPECT_EQ(map_file(file, ctx, kept.map_offset + kept.size, pmm::PAGE_SIZE), static_cast<uintptr_t>(0));
    EXPECT_EQ(map_file(file, ctx, destroyed.map_offset, pmm::PAGE_SIZE), static_cast<uintptr_t>(0));
    EXPECT_EQ(map_file(file, ctx, ~static_cast<uint64_t>(pmm::PAGE_SIZE - 1), pmm::PAGE_SIZE),
              static_cast<uintptr_t>(0));
    EXPECT_NE(map_file(file, ctx, kept.map_offset, kept.size), static_cast<uintptr_t>(0));

    ASSERT_TRUE(closes_before_deadline(file));
    mm::mm_context_release(ctx);
}

static recording_display g_mapped_display;

// A mapping keeps its buffer after GFXFB_DESTROY_BUFFER drops the handle, and unmapping it hands the buffer back
TEST(gfxfb, a_mapped_buffer_outlives_its_handle) {
    light_output(g_mapped_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_mapped_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    gfxfb::gfxfb_buffer_args mapped = request_small_buffer(file, page);
    gfxfb::gfxfb_buffer_args unmapped = request_small_buffer(file, page);
    ASSERT_NE(unmapped.handle, static_cast<uint32_t>(0));

    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);
    uintptr_t addr = map_file(file, ctx, mapped.map_offset, mapped.size);
    ASSERT_NE(addr, static_cast<uintptr_t>(0));

    // The reaper hands buffers back in the order their last references went, so seeing only the unmapped buffer back
    // shows the mapping kept the other
    ASSERT_EQ(destroy_buffer(file, page, mapped.handle), fs::OK);
    ASSERT_EQ(destroy_buffer(file, page, unmapped.handle), fs::OK);
    ASSERT_TRUE(spin_wait_ge(g_mapped_display.released_count, 1));
    EXPECT_EQ(g_mapped_display.released_count.load_acquire(), static_cast<uint32_t>(1));
    EXPECT_EQ(g_mapped_display.last_released, &g_mapped_display.created[1]);
    EXPECT_EQ(paging::get_physical(addr, ctx->pt_root), g_mapped_display.created[0].phys);

    ASSERT_TRUE(closes_before_deadline(file));
    ASSERT_EQ(mm::mm_context_unmap(ctx, addr, mapped.size), mm::MM_CTX_OK);
    ASSERT_TRUE(spin_wait_ge(g_mapped_display.released_count, 2));
    EXPECT_EQ(g_mapped_display.last_released, &g_mapped_display.created[0]);

    mm::mm_context_release(ctx);
}

static recording_display g_two_files_display;

// A handle names a buffer only in the file that created it, so another file can neither destroy nor map it
TEST(gfxfb, handles_stay_within_their_file) {
    light_output(g_two_files_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_two_files_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* owner = fs::open(test.path, fs::O_RDWR);
    fs::file* other = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(owner);
    ASSERT_NOT_NULL(other);

    gfxfb::gfxfb_buffer_args args = request_small_buffer(owner, page);
    ASSERT_NE(args.handle, static_cast<uint32_t>(0));

    mm::mm_context* ctx = mm::mm_context_create();
    ASSERT_NOT_NULL(ctx);

    EXPECT_EQ(destroy_buffer(other, page, args.handle), fs::ERR_INVAL);
    EXPECT_EQ(map_file(other, ctx, args.map_offset, args.size), static_cast<uintptr_t>(0));
    EXPECT_EQ(destroy_buffer(owner, page, args.handle), fs::OK);

    ASSERT_TRUE(closes_before_deadline(other));
    ASSERT_TRUE(closes_before_deadline(owner));
    mm::mm_context_release(ctx);
}

static recording_display g_closing_display;

// Closing a file drops every handle it holds, handing its unmapped buffers back to the display
TEST(gfxfb, closing_a_file_hands_its_buffers_back) {
    light_output(g_closing_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_closing_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    ASSERT_NE(request_small_buffer(file, page).handle, static_cast<uint32_t>(0));
    ASSERT_NE(request_small_buffer(file, page).handle, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(file));
    EXPECT_TRUE(spin_wait_ge(g_closing_display.released_count, 2));
}

static recording_display g_control_display;

// Until the controlling file closes, even after the display or gfxfb refuses a creation of that file, another file's
// creations fail with ERR_BUSY without reaching the display, and a third file's flush and close leave control unchanged
TEST(gfxfb, the_first_file_to_create_a_buffer_controls_the_buffers_until_it_closes) {
    light_output(g_control_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_control_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* controller = fs::open(test.path, fs::O_RDWR);
    fs::file* other = fs::open(test.path, fs::O_RDWR);
    fs::file* next = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(controller);
    ASSERT_NOT_NULL(other);
    ASSERT_NOT_NULL(next);

    uint32_t format = gfxfb::GFXFB_FORMAT_XRGB8888;
    ASSERT_NE(request_small_buffer(controller, page).handle, static_cast<uint32_t>(0));
    EXPECT_NE(request_small_buffer(controller, page).handle, static_cast<uint32_t>(0));
    EXPECT_EQ(request_buffer(other, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);
    ASSERT_EQ(flush_one_rect(test.path, page), fs::OK);
    EXPECT_EQ(g_control_display.recorded_count, static_cast<uint32_t>(1));

    EXPECT_EQ(request_buffer(controller, page, 0, SMALL_BUFFER_HEIGHT, format), fs::ERR_INVAL);
    EXPECT_EQ(request_buffer(other, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);

    // Refusing `other` leaves control with `controller`, so the next creation of `other` fails too
    EXPECT_EQ(request_buffer(other, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);

    // Two refusals by the display in a row leave control with `controller`, so `other`'s creation after each fails too
    EXPECT_EQ(request_buffer(controller, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);
    EXPECT_EQ(request_buffer(other, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);
    EXPECT_EQ(request_buffer(controller, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);
    EXPECT_EQ(request_buffer(other, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);

    // Destroying every buffer it holds leaves control with `controller`, so the next creation of `other` fails too
    ASSERT_EQ(destroy_buffer(controller, page, 1), fs::OK);
    ASSERT_EQ(destroy_buffer(controller, page, 2), fs::OK);
    EXPECT_EQ(request_buffer(other, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);
    EXPECT_EQ(g_control_display.create_calls, static_cast<uint32_t>(4));

    // Once `controller` closes, a creation the display refuses takes no control, so another file's then succeeds
    ASSERT_TRUE(closes_before_deadline(controller));
    EXPECT_EQ(request_buffer(other, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);
    EXPECT_NE(request_small_buffer(next, page).handle, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(next));
    ASSERT_TRUE(closes_before_deadline(other));
}

static recording_display g_failed_creation_display;

// A creation that fails takes no control, whether gfxfb refuses its malformed request, the display refuses it or gfxfb
// cannot write its reply, so another file's creation then succeeds and takes control
TEST(gfxfb, a_creation_that_fails_takes_no_control) {
    light_output(g_failed_creation_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_failed_creation_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* failing = fs::open(test.path, fs::O_RDWR);
    fs::file* other = fs::open(test.path, fs::O_RDWR);
    fs::file* next = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(failing);
    ASSERT_NOT_NULL(other);
    ASSERT_NOT_NULL(next);

    uint32_t format = gfxfb::GFXFB_FORMAT_XRGB8888;
    EXPECT_EQ(request_buffer(failing, page, 0, SMALL_BUFFER_HEIGHT, format), fs::ERR_INVAL);

    // A read-only page lets gfxfb read the request but not write the reply, after the display created the buffer
    write_small_request(page, 0);
    ASSERT_EQ(mm::mm_context_mprotect(page.ctx, page.addr, pmm::PAGE_SIZE, mm::MM_PROT_READ), mm::MM_CTX_OK);
    EXPECT_EQ(file_ioctl(failing, page, gfxfb::GFXFB_CREATE_BUFFER, page.addr), fs::ERR_INVAL);
    EXPECT_EQ(g_failed_creation_display.created_count, static_cast<uint32_t>(1));

    uint32_t prot = mm::MM_PROT_READ | mm::MM_PROT_WRITE;
    ASSERT_EQ(mm::mm_context_mprotect(page.ctx, page.addr, pmm::PAGE_SIZE, prot), mm::MM_CTX_OK);

    g_failed_creation_display.create_result = gfxfb::ERR_REFUSED;
    EXPECT_EQ(request_buffer(failing, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_IO);
    g_failed_creation_display.create_result = gfxfb::ERR_UNSUPPORTED;
    EXPECT_EQ(request_buffer(failing, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_NOSYS);
    g_failed_creation_display.create_result = gfxfb::OK;

    EXPECT_NE(request_small_buffer(other, page).handle, static_cast<uint32_t>(0));
    EXPECT_EQ(request_buffer(failing, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);

    // Even while another file controls, gfxfb refuses a zero width with ERR_INVAL before it checks control, and control
    // stays with `other`, so the next creation of `failing` fails with ERR_BUSY
    EXPECT_EQ(request_buffer(failing, page, 0, SMALL_BUFFER_HEIGHT, format), fs::ERR_INVAL);
    EXPECT_EQ(request_buffer(failing, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);

    // The display refusing a creation of `other` leaves control with `other`, so the next creation of `failing` fails
    EXPECT_EQ(request_buffer(other, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);
    EXPECT_EQ(request_buffer(failing, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);

    // Once `other` closes, a creation the display refuses takes no control, so another file's then succeeds
    ASSERT_TRUE(closes_before_deadline(other));
    EXPECT_EQ(request_buffer(failing, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);
    EXPECT_NE(request_small_buffer(next, page).handle, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(next));
    ASSERT_TRUE(closes_before_deadline(failing));
}

static recording_display g_reserving_display;

// The files the hook's calls are made on, the page holding their requests, and what those calls returned
static fs::file* g_reserving_file;
static fs::file* g_rival_file;
static fs::file* g_closing_file;
static user_page* g_reserving_page;
static int32_t g_reserving_refused_format_result;
static int32_t g_reserving_reserved_field_result;
static int32_t g_rival_reserved_field_result;
static int32_t g_rival_results[2];
static int32_t g_rival_result_after_close;

// Runs inside the reserving file's first creation: asks that file for a refused format, asks both files with a nonzero
// reserved field, asks the rival twice, closes a third file, asks the rival again, then fails the first creation
static void create_on_both_files_while_creating() {
    *g_reserving_page->at<gfxfb::gfxfb_buffer_args>(0) = {
        .width  = SMALL_BUFFER_WIDTH,
        .height = SMALL_BUFFER_HEIGHT,
        .format = WIDE_FORMAT,
    };

    g_reserving_refused_format_result = fs::ioctl(g_reserving_file, gfxfb::GFXFB_CREATE_BUFFER, g_reserving_page->addr);

    write_small_request(*g_reserving_page, 1);
    g_reserving_reserved_field_result = fs::ioctl(g_reserving_file, gfxfb::GFXFB_CREATE_BUFFER, g_reserving_page->addr);
    g_rival_reserved_field_result = fs::ioctl(g_rival_file, gfxfb::GFXFB_CREATE_BUFFER, g_reserving_page->addr);

    write_small_request(*g_reserving_page, 0);
    for (int32_t& result : g_rival_results) {
        result = fs::ioctl(g_rival_file, gfxfb::GFXFB_CREATE_BUFFER, g_reserving_page->addr);
    }

    fs::close(g_closing_file);
    g_rival_result_after_close = fs::ioctl(g_rival_file, gfxfb::GFXFB_CREATE_BUFFER, g_reserving_page->addr);

    g_reserving_display.create_result = gfxfb::ERR_NO_MEMORY;
}

// A file's running creations reserve control, even after one is refused or another file closes, so a rival's creations
// past gfxfb's checks fail with ERR_BUSY without reaching the display until the reserving file's last creation fails
TEST(gfxfb, creations_in_the_display_reserve_control_for_their_file) {
    light_output(g_reserving_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_reserving_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* reserving = fs::open(test.path, fs::O_RDWR);
    fs::file* rival = fs::open(test.path, fs::O_RDWR);
    fs::file* closing = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(reserving);
    ASSERT_NOT_NULL(rival);
    ASSERT_NOT_NULL(closing);

    g_reserving_file = reserving;
    g_rival_file = rival;
    g_closing_file = closing;
    g_reserving_page = &page;
    g_reserving_refused_format_result = fs::OK;
    g_reserving_reserved_field_result = fs::OK;
    g_rival_reserved_field_result = fs::OK;
    g_rival_results[0] = fs::OK;
    g_rival_results[1] = fs::OK;
    g_rival_result_after_close = fs::OK;
    g_reserving_display.while_creating = create_on_both_files_while_creating;

    uint32_t format = gfxfb::GFXFB_FORMAT_XRGB8888;
    EXPECT_EQ(request_buffer(reserving, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_NOMEM);
    EXPECT_EQ(g_reserving_refused_format_result, fs::ERR_INVAL);
    EXPECT_EQ(g_rival_results[0], fs::ERR_BUSY);
    EXPECT_EQ(g_rival_results[1], fs::ERR_BUSY);
    EXPECT_EQ(g_rival_result_after_close, fs::ERR_BUSY);
    EXPECT_EQ(g_reserving_display.create_calls, static_cast<uint32_t>(2));

    // While a file reserves control, a nonzero reserved field fails with ERR_INVAL, never ERR_BUSY, from either file
    EXPECT_EQ(g_reserving_reserved_field_result, fs::ERR_INVAL);
    EXPECT_EQ(g_rival_reserved_field_result, fs::ERR_INVAL);

    // Once the reservation ends, the rival's creation succeeds under handle 1, as its refused creations took no handle
    g_reserving_display.create_result = gfxfb::OK;
    EXPECT_EQ(request_small_buffer(rival, page).handle, static_cast<uint32_t>(1));
    EXPECT_EQ(request_buffer(reserving, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);

    ASSERT_TRUE(closes_before_deadline(rival));
    ASSERT_TRUE(closes_before_deadline(reserving));
}

static recording_display g_overlapping_display;

// The files the hook's calls are made on, the page holding their requests, and what those calls returned
static fs::file* g_overlapping_file;
static fs::file* g_overlapping_rival;
static fs::file* g_overlapping_closing;
static user_page* g_overlapping_page;
static int32_t g_overlapping_results[2];
static int32_t g_overlapping_refused_format_result;
static int32_t g_overlapping_reserved_field_result;
static int32_t g_overlapping_rival_reserved_field_result;
static int32_t g_overlapping_rival_result;

// Runs inside the file's first creation: asks that file for two small buffers and one in a format the display refuses,
// closes a third file, asks both files with a nonzero reserved field and the rival without, then fails that creation
static void create_then_refuse_while_creating() {
    uintptr_t args_addr = g_overlapping_page->addr;

    for (int32_t& result : g_overlapping_results) {
        write_small_request(*g_overlapping_page, 0);
        result = fs::ioctl(g_overlapping_file, gfxfb::GFXFB_CREATE_BUFFER, args_addr);
    }

    *g_overlapping_page->at<gfxfb::gfxfb_buffer_args>(0) = {
        .width  = SMALL_BUFFER_WIDTH,
        .height = SMALL_BUFFER_HEIGHT,
        .format = WIDE_FORMAT,
    };

    g_overlapping_refused_format_result = fs::ioctl(g_overlapping_file, gfxfb::GFXFB_CREATE_BUFFER, args_addr);

    fs::close(g_overlapping_closing);

    write_small_request(*g_overlapping_page, 1);
    g_overlapping_reserved_field_result = fs::ioctl(g_overlapping_file, gfxfb::GFXFB_CREATE_BUFFER, args_addr);
    g_overlapping_rival_reserved_field_result = fs::ioctl(g_overlapping_rival, gfxfb::GFXFB_CREATE_BUFFER, args_addr);

    write_small_request(*g_overlapping_page, 0);
    g_overlapping_rival_result = fs::ioctl(g_overlapping_rival, gfxfb::GFXFB_CREATE_BUFFER, args_addr);

    g_overlapping_display.create_result = gfxfb::ERR_NO_MEMORY;
}

// A creation succeeding while another creation of its file runs takes control, kept through each call made meanwhile
// and the other's failure, so until the file closes, a rival's creations past gfxfb's checks fail with ERR_BUSY
TEST(gfxfb, a_creation_that_succeeds_while_another_runs_takes_control) {
    light_output(g_overlapping_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_overlapping_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* controller = fs::open(test.path, fs::O_RDWR);
    fs::file* rival = fs::open(test.path, fs::O_RDWR);
    fs::file* closing = fs::open(test.path, fs::O_RDWR);
    fs::file* next = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(controller);
    ASSERT_NOT_NULL(rival);
    ASSERT_NOT_NULL(closing);
    ASSERT_NOT_NULL(next);

    g_overlapping_file = controller;
    g_overlapping_rival = rival;
    g_overlapping_closing = closing;
    g_overlapping_page = &page;
    g_overlapping_results[0] = fs::ERR_INVAL;
    g_overlapping_results[1] = fs::ERR_INVAL;
    g_overlapping_refused_format_result = fs::OK;
    g_overlapping_reserved_field_result = fs::OK;
    g_overlapping_rival_reserved_field_result = fs::OK;
    g_overlapping_rival_result = fs::OK;
    g_overlapping_display.while_creating = create_then_refuse_while_creating;

    uint32_t format = gfxfb::GFXFB_FORMAT_XRGB8888;
    EXPECT_EQ(request_buffer(controller, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_NOMEM);
    EXPECT_EQ(g_overlapping_results[0], fs::OK);
    EXPECT_EQ(g_overlapping_results[1], fs::OK);
    EXPECT_EQ(g_overlapping_refused_format_result, fs::ERR_INVAL);
    EXPECT_EQ(g_overlapping_reserved_field_result, fs::ERR_INVAL);
    EXPECT_EQ(g_overlapping_rival_reserved_field_result, fs::ERR_INVAL);
    EXPECT_EQ(g_overlapping_rival_result, fs::ERR_BUSY);

    g_overlapping_display.create_result = gfxfb::OK;
    EXPECT_EQ(request_buffer(rival, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);
    EXPECT_NE(request_small_buffer(controller, page).handle, static_cast<uint32_t>(0));
    EXPECT_EQ(g_overlapping_display.create_calls, static_cast<uint32_t>(5));

    // Once `controller` closes, a creation the display refuses takes no control, so another file's then succeeds
    ASSERT_TRUE(closes_before_deadline(controller));
    EXPECT_EQ(request_buffer(rival, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);
    EXPECT_NE(request_small_buffer(next, page).handle, static_cast<uint32_t>(0));
    ASSERT_TRUE(closes_before_deadline(next));

    // Once `next` closes, the rival's creation succeeds under handle 1, as none of its refused creations kept a handle
    EXPECT_EQ(request_small_buffer(rival, page).handle, static_cast<uint32_t>(1));

    ASSERT_TRUE(closes_before_deadline(rival));
}

static recording_display g_after_close_display;

// After the controlling file closes, a creation the display refuses takes no control, so another file's then succeeds
TEST(gfxfb, a_creation_refused_after_the_controlling_file_closes_takes_no_control) {
    light_output(g_after_close_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_after_close_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* controller = fs::open(test.path, fs::O_RDWR);
    fs::file* refused = fs::open(test.path, fs::O_RDWR);
    fs::file* next = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(controller);
    ASSERT_NOT_NULL(refused);
    ASSERT_NOT_NULL(next);

    ASSERT_NE(request_small_buffer(controller, page).handle, static_cast<uint32_t>(0));
    ASSERT_TRUE(closes_before_deadline(controller));

    EXPECT_EQ(request_buffer(refused, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);
    EXPECT_NE(request_small_buffer(next, page).handle, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(next));
    ASSERT_TRUE(closes_before_deadline(refused));
}

static recording_display g_latching_display;

// An output keeps a presented buffer from its present until another present latches, so destroying its handle then
// hands nothing back, and a base buffer stays allocated while another buffer replaces it, so handle 0 presents it again
TEST(gfxfb, an_output_keeps_a_presented_buffer_until_another_latches) {
    light_output(g_latching_display, 0, false);
    light_output(g_latching_display, 1, false);
    gfxfb_test_node test = publish_driver_node(&g_latching_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t first = create_output_0_buffer(file, page);
    uint32_t second = create_output_0_buffer(file, page);
    ASSERT_NE(second, static_cast<uint32_t>(0));

    ASSERT_EQ(present_buffer(file, page, 0, first), fs::OK);
    EXPECT_EQ(g_latching_display.presented, &g_latching_display.created[0]);
    EXPECT_EQ(g_latching_display.presented_output, static_cast<uint32_t>(0));

    // The output holds one present at a time, so a second fails without reaching the display until the first latches,
    // and the file reads the first's event before it presents again
    EXPECT_EQ(present_buffer(file, page, 0, second), fs::ERR_BUSY);
    EXPECT_EQ(g_latching_display.present_calls, static_cast<uint32_t>(1));
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, first);
    ASSERT_EQ(present_buffer(file, page, 0, second), fs::OK);

    // `first` is shown and `second` held for its present, so `first` comes back alone once `second` latches
    ASSERT_EQ(destroy_buffer(file, page, first), fs::OK);
    ASSERT_EQ(destroy_buffer(file, page, second), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, second);
    ASSERT_TRUE(spin_wait_ge(g_latching_display.released_count, 1));
    EXPECT_EQ(g_latching_display.released_count.load_acquire(), static_cast<uint32_t>(1));
    EXPECT_EQ(g_latching_display.last_released, &g_latching_display.created[0]);

    // Once the base buffer latches again, only `second`, which it replaced, comes back
    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    EXPECT_EQ(g_latching_display.presented, &g_latching_display.bases[0]);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).type, gfxfb::GFXFB_EVENT_PRESENT);
    ASSERT_TRUE(spin_wait_ge(g_latching_display.released_count, 2));
    EXPECT_EQ(g_latching_display.released_count.load_acquire(), static_cast<uint32_t>(2));
    EXPECT_EQ(g_latching_display.last_released, &g_latching_display.created[1]);

    // Every lit output has a base buffer, not only the primary one, and output 1's stays allocated while a latched
    // buffer replaces it, so once handle 0 latches back there only that buffer comes back
    uint32_t width = static_cast<uint32_t>(OUTPUT_1_WIDTH);
    uint32_t height = static_cast<uint32_t>(OUTPUT_1_HEIGHT);
    ASSERT_EQ(request_buffer(file, page, width, height, gfxfb::GFXFB_FORMAT_XRGB8888), fs::OK);
    uint32_t replacing = page.at<gfxfb::gfxfb_buffer_args>(0)->handle;
    ASSERT_EQ(present_buffer(file, page, 1, replacing), fs::OK);
    end_present(test, 1, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).output, static_cast<uint32_t>(1));
    ASSERT_EQ(destroy_buffer(file, page, replacing), fs::OK);

    ASSERT_EQ(present_buffer(file, page, 1, 0), fs::OK);
    EXPECT_EQ(g_latching_display.presented, &g_latching_display.bases[1]);
    EXPECT_EQ(g_latching_display.presented_output, static_cast<uint32_t>(1));
    end_present(test, 1, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_TRUE(spin_wait_ge(g_latching_display.released_count, 3));
    EXPECT_EQ(g_latching_display.released_count.load_acquire(), static_cast<uint32_t>(3));
    EXPECT_EQ(g_latching_display.last_released, &g_latching_display.created[2]);

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_bad_present_display;

// gfxfb refuses a present it cannot read, one for an output of the count or more and one for a handle its file does not
// hold with ERR_INVAL, and handle 0 on an output dark from the start with ERR_AGAIN, before control and the display
TEST(gfxfb, present_refuses_requests_before_control) {
    light_output(g_bad_present_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_bad_present_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* controller = fs::open(test.path, fs::O_RDWR);
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(controller);
    ASSERT_NOT_NULL(file);

    // With no file in control, `file`'s refusals neither reserve nor take control, so `controller`'s creation succeeds
    EXPECT_EQ(present_buffer(file, page, 0, 1), fs::ERR_INVAL);
    EXPECT_EQ(present_buffer(file, page, 1, 0), fs::ERR_AGAIN);
    uint32_t foreign = create_output_0_buffer(controller, page);
    ASSERT_NE(foreign, static_cast<uint32_t>(0));

    EXPECT_EQ(file_ioctl(file, page, gfxfb::GFXFB_PRESENT, 0), fs::ERR_INVAL);
    EXPECT_EQ(present_buffer(file, page, recording_display::OUTPUTS, 0), fs::ERR_INVAL);
    EXPECT_EQ(present_buffer(file, page, WIDE_OUTPUT_INDEX, 0), fs::ERR_INVAL);
    EXPECT_EQ(present_buffer(file, page, 0, foreign), fs::ERR_INVAL);
    EXPECT_EQ(present_buffer(file, page, 0, gfxfb::GFXFB_CLIENT_BUFFERS_MAX + 1), fs::ERR_INVAL);
    EXPECT_EQ(present_buffer(file, page, 1, 0), fs::ERR_AGAIN);
    EXPECT_EQ(g_bad_present_display.present_calls, static_cast<uint32_t>(0));

    // `file`'s refusals leave `controller`'s control taken, not merely reserved, so it outlasts a failed creation
    uint32_t format = gfxfb::GFXFB_FORMAT_XRGB8888;
    EXPECT_EQ(request_buffer(controller, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);
    EXPECT_EQ(request_buffer(file, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);

    // The handle `controller` destroyed is refused too and leaves control taken, so after another failed creation
    // `file`'s creation fails without reaching the display, while `controller`'s present of handle 0 still reaches it
    ASSERT_EQ(destroy_buffer(controller, page, foreign), fs::OK);
    EXPECT_EQ(present_buffer(controller, page, 0, foreign), fs::ERR_INVAL);
    EXPECT_EQ(request_buffer(controller, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);
    EXPECT_EQ(request_buffer(file, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);
    EXPECT_EQ(g_bad_present_display.create_calls, static_cast<uint32_t>(3));
    ASSERT_EQ(present_buffer(controller, page, 0, 0), fs::OK);
    EXPECT_EQ(g_bad_present_display.present_calls, static_cast<uint32_t>(1));
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);

    ASSERT_TRUE(closes_before_deadline(file));
    ASSERT_TRUE(closes_before_deadline(controller));
}

static recording_display g_full_present_display;

// A file holding GFXFB_CLIENT_BUFFERS_MAX handles presents the last of them, and the handle past it fails with
// ERR_INVAL without reaching the display
TEST(gfxfb, a_full_file_presents_its_last_handle) {
    light_output(g_full_present_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_full_present_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    for (uint32_t i = 1; i < gfxfb::GFXFB_CLIENT_BUFFERS_MAX; i++) {
        ASSERT_NE(request_small_buffer(file, page).handle, static_cast<uint32_t>(0));
    }

    ASSERT_EQ(create_output_0_buffer(file, page), gfxfb::GFXFB_CLIENT_BUFFERS_MAX);
    EXPECT_EQ(present_buffer(file, page, 0, gfxfb::GFXFB_CLIENT_BUFFERS_MAX + 1), fs::ERR_INVAL);
    EXPECT_EQ(g_full_present_display.present_calls, static_cast<uint32_t>(0));
    ASSERT_EQ(present_buffer(file, page, 0, gfxfb::GFXFB_CLIENT_BUFFERS_MAX), fs::OK);
    EXPECT_EQ(g_full_present_display.presented, &g_full_present_display.created[gfxfb::GFXFB_CLIENT_BUFFERS_MAX - 1]);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);

    ASSERT_TRUE(closes_giving_output_back(test, g_full_present_display, file, 0));
}

static recording_display g_refused_present_display;

// A present the display refuses returns its refusal as a file error and keeps nothing, so the output takes the next
// present and the refused buffer goes back once its handle is destroyed
TEST(gfxfb, present_returns_the_display_refusals_as_file_errors) {
    light_output(g_refused_present_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_refused_present_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t fitting = create_output_0_buffer(file, page);
    uint32_t small = request_small_buffer(file, page).handle;
    ASSERT_NE(fitting, static_cast<uint32_t>(0));
    ASSERT_NE(small, static_cast<uint32_t>(0));

    EXPECT_EQ(present_buffer(file, page, 0, small), fs::ERR_INVAL);
    EXPECT_EQ(present_buffer(file, page, 1, fitting), fs::ERR_AGAIN);

    g_refused_present_display.present_answer = gfxfb::ERR_REFUSED;
    EXPECT_EQ(present_buffer(file, page, 0, fitting), fs::ERR_IO);
    g_refused_present_display.present_answer = gfxfb::ERR_UNSUPPORTED;
    EXPECT_EQ(present_buffer(file, page, 0, fitting), fs::ERR_NOSYS);
    EXPECT_EQ(g_refused_present_display.present_calls, static_cast<uint32_t>(4));
    EXPECT_NULL(g_refused_present_display.presented);

    g_refused_present_display.present_answer = gfxfb::OK;
    ASSERT_EQ(destroy_buffer(file, page, fitting), fs::OK);
    ASSERT_TRUE(spin_wait_ge(g_refused_present_display.released_count, 1));
    EXPECT_EQ(g_refused_present_display.last_released, &g_refused_present_display.created[0]);
    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);

    ASSERT_TRUE(closes_before_deadline(file));
}

// The boot display presents nothing, and its refusal takes no control, so another file's creation reaches the display
TEST(gfxfb, boot_display_presents_nothing) {
    gfxfb_test_node test = publish_boot_node();
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    fs::file* other = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);
    ASSERT_NOT_NULL(other);

    EXPECT_EQ(present_buffer(file, page, 0, 0), fs::ERR_NOSYS);
    EXPECT_EQ(request_buffer(other, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, gfxfb::GFXFB_FORMAT_XRGB8888),
              fs::ERR_NOSYS);

    ASSERT_TRUE(closes_before_deadline(other));
    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_early_end_display;
static gfxfb_test_node g_early_end_node;

// Ends the present being made, as a display whose hardware latches before `present()` returns
static void latch_while_presenting() {
    end_present(g_early_end_node, 0, gfxfb::GFXFB_PRESENT_LATCHED);
}

// A present that latches before the display's `present()` returns leaves the output showing its buffer
TEST(gfxfb, a_present_that_latches_inside_the_display_call_is_shown) {
    light_output(g_early_end_display, 0, false);
    g_early_end_node = publish_driver_node(&g_early_end_display);
    ASSERT_NOT_NULL(g_early_end_node.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(g_early_end_node.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t shown = create_output_0_buffer(file, page);
    uint32_t unshown = create_output_0_buffer(file, page);
    ASSERT_NE(unshown, static_cast<uint32_t>(0));

    g_early_end_display.while_presenting = latch_while_presenting;
    ASSERT_EQ(present_buffer(file, page, 0, shown), fs::OK);
    EXPECT_EQ(read_event(file).handle, shown);

    // The output shows `shown`, so only `unshown` comes back, and the output takes the next present at once
    ASSERT_EQ(destroy_buffer(file, page, shown), fs::OK);
    ASSERT_EQ(destroy_buffer(file, page, unshown), fs::OK);
    ASSERT_TRUE(spin_wait_ge(g_early_end_display.released_count, 1));
    EXPECT_EQ(g_early_end_display.released_count.load_acquire(), static_cast<uint32_t>(1));
    EXPECT_EQ(g_early_end_display.last_released, &g_early_end_display.created[1]);

    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    end_present(g_early_end_node, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_TRUE(spin_wait_ge(g_early_end_display.released_count, 2));
    EXPECT_EQ(g_early_end_display.last_released, &g_early_end_display.created[0]);

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_dark_end_display;

// A present that ends dark drops only its own buffer and frees its output for the next present, while one that ends
// failed keeps its buffer and the shown one, which failed hardware may read, and fails every later flush and present
TEST(gfxfb, a_dark_end_drops_the_present_and_a_failed_end_keeps_it) {
    light_output(g_dark_end_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_dark_end_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t shown = create_output_0_buffer(file, page);
    uint32_t darkened = create_output_0_buffer(file, page);
    uint32_t failed = create_output_0_buffer(file, page);
    uint32_t unshown = create_output_0_buffer(file, page);
    uint32_t shown_when_failed = create_output_0_buffer(file, page);
    ASSERT_NE(shown_when_failed, static_cast<uint32_t>(0));

    // The output shows `shown` with its handle gone, so the dark end hands back `darkened` alone
    ASSERT_EQ(present_buffer(file, page, 0, shown), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).status, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_EQ(destroy_buffer(file, page, shown), fs::OK);
    ASSERT_EQ(present_buffer(file, page, 0, darkened), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_DARK);
    EXPECT_EQ(read_event(file).status, gfxfb::GFXFB_PRESENT_DARK);
    ASSERT_EQ(destroy_buffer(file, page, darkened), fs::OK);
    ASSERT_TRUE(spin_wait_ge(g_dark_end_display.released_count, 1));
    EXPECT_EQ(g_dark_end_display.released_count.load_acquire(), static_cast<uint32_t>(1));
    EXPECT_EQ(g_dark_end_display.last_released, &g_dark_end_display.created[1]);

    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).status, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_TRUE(spin_wait_ge(g_dark_end_display.released_count, 2));
    EXPECT_EQ(g_dark_end_display.last_released, &g_dark_end_display.created[0]);

    // The output shows `shown_when_failed` with its handle gone when the present of `failed` ends failed, and a flush
    // then fails even though it would skip the display
    ASSERT_EQ(present_buffer(file, page, 0, shown_when_failed), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).status, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_EQ(destroy_buffer(file, page, shown_when_failed), fs::OK);
    ASSERT_EQ(present_buffer(file, page, 0, failed), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_FAILED);
    EXPECT_EQ(read_event(file).status, gfxfb::GFXFB_PRESENT_FAILED);
    EXPECT_EQ(present_buffer(file, page, 0, 0), fs::ERR_IO);
    EXPECT_EQ(flush_one_rect(test.path, page), fs::ERR_IO);
    EXPECT_EQ(g_dark_end_display.present_calls, static_cast<uint32_t>(5));
    EXPECT_EQ(g_dark_end_display.recorded_count, static_cast<uint32_t>(0));

    // Buffers come back in the order their last references went, so seeing only `unshown` back shows `failed` and
    // `shown_when_failed` are kept
    ASSERT_EQ(destroy_buffer(file, page, failed), fs::OK);
    ASSERT_EQ(destroy_buffer(file, page, unshown), fs::OK);
    ASSERT_TRUE(spin_wait_ge(g_dark_end_display.released_count, 3));
    EXPECT_EQ(g_dark_end_display.released_count.load_acquire(), static_cast<uint32_t>(3));
    EXPECT_EQ(g_dark_end_display.last_released, &g_dark_end_display.created[3]);

    // The display failed, so the close's present of the base buffer never reaches it
    ASSERT_TRUE(closes_before_deadline(file));
    EXPECT_EQ(g_dark_end_display.present_calls, static_cast<uint32_t>(5));
}

static recording_display g_device_failure_display;

// A present's ERR_DEVICE counts the display as failed, so every later flush and present fails with ERR_IO without
// calling the display, even while the primary shows a created buffer
TEST(gfxfb, a_present_failing_the_display_fails_every_later_flush_and_present) {
    light_output(g_device_failure_display, 0, false);
    light_output(g_device_failure_display, 1, false);
    gfxfb_test_node test = publish_driver_node(&g_device_failure_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t created = create_output_0_buffer(file, page);
    ASSERT_NE(created, static_cast<uint32_t>(0));
    ASSERT_EQ(present_buffer(file, page, 0, created), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, created);

    g_device_failure_display.present_answer = gfxfb::ERR_DEVICE;
    EXPECT_EQ(present_buffer(file, page, 1, 0), fs::ERR_IO);
    g_device_failure_display.present_answer = gfxfb::OK;
    EXPECT_EQ(present_buffer(file, page, 1, 0), fs::ERR_IO);
    EXPECT_EQ(present_buffer(file, page, 0, 0), fs::ERR_IO);
    EXPECT_EQ(flush_one_rect(test.path, page), fs::ERR_IO);
    EXPECT_EQ(g_device_failure_display.present_calls, static_cast<uint32_t>(2));
    EXPECT_EQ(g_device_failure_display.recorded_count, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_skipped_flush_display;

// With a created buffer shown on the primary and no base present held, FLUSH returns OK without reading its rects or
// calling the display, and the base buffer's next present passes the display no region, whatever regions it names
TEST(gfxfb, flush_skips_the_display_while_the_primary_shows_a_created_buffer) {
    light_output(g_skipped_flush_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_skipped_flush_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    gfxfb::gfxfb_rect region = {0, 0, 1, 1};
    uint32_t created = create_output_0_buffer(file, page);
    ASSERT_NE(created, static_cast<uint32_t>(0));
    ASSERT_EQ(present_one_region(file, page, 0, created, region), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, created);

    EXPECT_EQ(flush_one_rect(test.path, page), fs::OK);
    page.at<gfxfb::gfxfb_flush_args>(ARGS_OFFSET)->rects = 0;
    EXPECT_EQ(file_ioctl(file, page, gfxfb::GFXFB_FLUSH, page.addr + ARGS_OFFSET), fs::OK);
    EXPECT_EQ(g_skipped_flush_display.recorded_count, static_cast<uint32_t>(0));

    // A present of the created buffer passes its region and leaves the base buffer stale
    ASSERT_EQ(present_one_region(file, page, 0, created, region), fs::OK);
    EXPECT_FALSE(g_skipped_flush_display.named_no_region_on[0]);
    EXPECT_EQ(g_skipped_flush_display.presented_region_count, static_cast<uint32_t>(1));
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, created);

    ASSERT_EQ(present_one_region(file, page, 0, 0, region), fs::OK);
    EXPECT_TRUE(g_skipped_flush_display.named_no_region_on[0]);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, static_cast<uint32_t>(0));

    // With the base buffer shown again, a flush reaches the display and the next present passes its region
    EXPECT_EQ(flush_one_rect(test.path, page), fs::OK);
    EXPECT_EQ(g_skipped_flush_display.recorded_count, static_cast<uint32_t>(1));
    ASSERT_EQ(present_one_region(file, page, 0, 0, region), fs::OK);
    EXPECT_FALSE(g_skipped_flush_display.named_no_region_on[0]);
    EXPECT_EQ(g_skipped_flush_display.presented_region_count, static_cast<uint32_t>(1));
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_primary_skip_display;

// With output 1 the primary showing a created buffer and holding no base present, FLUSH skips the display and marks
// only output 1's base buffer stale, so a base present passes the display its region on output 0 and none on output 1
TEST(gfxfb, a_flush_skipped_on_the_primary_output_marks_only_its_base_buffer_stale) {
    light_output(g_primary_skip_display, 0, false);
    light_output(g_primary_skip_display, 1, true);
    gfxfb_test_node test = publish_driver_node(&g_primary_skip_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    gfxfb::gfxfb_rect region = {0, 0, 1, 1};
    uint32_t width = static_cast<uint32_t>(OUTPUT_1_WIDTH);
    uint32_t height = static_cast<uint32_t>(OUTPUT_1_HEIGHT);
    ASSERT_EQ(request_buffer(file, page, width, height, gfxfb::GFXFB_FORMAT_XRGB8888), fs::OK);
    uint32_t created = page.at<gfxfb::gfxfb_buffer_args>(0)->handle;
    ASSERT_EQ(present_one_region(file, page, 1, created, region), fs::OK);
    end_present(test, 1, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, created);

    EXPECT_EQ(flush_one_rect(test.path, page), fs::OK);
    EXPECT_EQ(g_primary_skip_display.recorded_count, static_cast<uint32_t>(0));

    ASSERT_EQ(present_one_region(file, page, 0, 0, region), fs::OK);
    EXPECT_FALSE(g_primary_skip_display.named_no_region_on[0]);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).output, static_cast<uint32_t>(0));

    ASSERT_EQ(present_one_region(file, page, 1, 0, region), fs::OK);
    EXPECT_TRUE(g_primary_skip_display.named_no_region_on[1]);
    end_present(test, 1, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).output, static_cast<uint32_t>(1));

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_held_base_flush_display;

// FLUSH reaches the display while the primary holds a present of its base buffer, even while it shows a created buffer,
// and the base buffer's next present then passes the display its regions
TEST(gfxfb, flush_reaches_the_display_while_a_present_of_the_base_buffer_is_held) {
    light_output(g_held_base_flush_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_held_base_flush_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    gfxfb::gfxfb_rect region = {0, 0, 1, 1};
    uint32_t created = create_output_0_buffer(file, page);
    ASSERT_NE(created, static_cast<uint32_t>(0));
    ASSERT_EQ(present_buffer(file, page, 0, created), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, created);

    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    EXPECT_EQ(flush_one_rect(test.path, page), fs::OK);
    EXPECT_EQ(g_held_base_flush_display.recorded_count, static_cast<uint32_t>(1));
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, static_cast<uint32_t>(0));

    ASSERT_EQ(present_one_region(file, page, 0, 0, region), fs::OK);
    EXPECT_FALSE(g_held_base_flush_display.named_no_region_on[0]);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_refused_stale_display;

// A refused present of the base buffer, by gfxfb or by the display, leaves it stale only if it was, so the next
// accepted one passes the display no region after a skipped flush and its own region otherwise
TEST(gfxfb, a_refused_present_of_the_base_buffer_leaves_it_stale_only_if_it_was) {
    light_output(g_refused_stale_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_refused_stale_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    gfxfb::gfxfb_rect region = {0, 0, 1, 1};
    uint32_t created = create_output_0_buffer(file, page);
    ASSERT_NE(created, static_cast<uint32_t>(0));
    ASSERT_EQ(present_one_region(file, page, 0, created, region), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(flush_one_rect(test.path, page), fs::OK);

    // The base buffer stays stale through gfxfb's ERR_BUSY for the unread event and through the display's refusal
    EXPECT_EQ(present_one_region(file, page, 0, 0, region), fs::ERR_BUSY);
    EXPECT_EQ(read_event(file).handle, created);
    g_refused_stale_display.present_answer = gfxfb::ERR_REFUSED;
    EXPECT_EQ(present_one_region(file, page, 0, 0, region), fs::ERR_IO);
    g_refused_stale_display.present_answer = gfxfb::OK;
    ASSERT_EQ(present_one_region(file, page, 0, 0, region), fs::OK);
    EXPECT_TRUE(g_refused_stale_display.named_no_region_on[0]);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, static_cast<uint32_t>(0));

    // With no skipped flush since, a refused present marks nothing stale, so the next one passes its region
    g_refused_stale_display.present_answer = gfxfb::ERR_REFUSED;
    EXPECT_EQ(present_one_region(file, page, 0, 0, region), fs::ERR_IO);
    g_refused_stale_display.present_answer = gfxfb::OK;
    ASSERT_EQ(present_one_region(file, page, 0, 0, region), fs::OK);
    EXPECT_FALSE(g_refused_stale_display.named_no_region_on[0]);
    EXPECT_EQ(g_refused_stale_display.presented_region_count, static_cast<uint32_t>(1));
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_interrupt_end_display;

// A latch reported with interrupts disabled, as from an interrupt handler, still hands the replaced buffer back
TEST(gfxfb, a_latch_with_interrupts_disabled_hands_the_replaced_buffer_back) {
    light_output(g_interrupt_end_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_interrupt_end_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t replaced = create_output_0_buffer(file, page);
    ASSERT_NE(replaced, static_cast<uint32_t>(0));
    ASSERT_EQ(present_buffer(file, page, 0, replaced), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, replaced);
    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    ASSERT_EQ(destroy_buffer(file, page, replaced), fs::OK);

    RUN_ELEVATED({
        uint64_t flags = cpu::irq_save();
        test.node->present_done(0, {.status = gfxfb::GFXFB_PRESENT_LATCHED, .frame = 0, .timestamp_ns = 0});
        cpu::irq_restore(flags);
    });

    ASSERT_TRUE(spin_wait_ge(g_interrupt_end_display.released_count, 1));
    EXPECT_EQ(g_interrupt_end_display.last_released, &g_interrupt_end_display.created[0]);

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_present_control_display;

// A successful present takes control as a creation does, one refused by the display or a failed display takes none, and
// while one file is in control another file's present fails with ERR_BUSY without reaching the display
TEST(gfxfb, a_present_takes_control_as_a_creation_does) {
    light_output(g_present_control_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_present_control_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* presenter = fs::open(test.path, fs::O_RDWR);
    fs::file* other = fs::open(test.path, fs::O_RDWR);
    fs::file* next = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(presenter);
    ASSERT_NOT_NULL(other);
    ASSERT_NOT_NULL(next);

    // Presenting the base buffer takes control with no creation, and a refusal by the output's held present or by the
    // display leaves it with the file
    uint32_t format = gfxfb::GFXFB_FORMAT_XRGB8888;
    ASSERT_EQ(present_buffer(presenter, page, 0, 0), fs::OK);
    EXPECT_EQ(present_buffer(presenter, page, 0, 0), fs::ERR_BUSY);
    EXPECT_EQ(request_buffer(other, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);
    EXPECT_EQ(present_buffer(other, page, 0, 0), fs::ERR_BUSY);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(presenter).status, gfxfb::GFXFB_PRESENT_LATCHED);
    g_present_control_display.present_answer = gfxfb::ERR_REFUSED;
    EXPECT_EQ(present_buffer(presenter, page, 0, 0), fs::ERR_IO);
    EXPECT_EQ(present_buffer(other, page, 0, 0), fs::ERR_BUSY);

    // The refusal leaves control taken, not merely reserved, so it outlasts a second one and `other`'s creation fails
    EXPECT_EQ(present_buffer(presenter, page, 0, 0), fs::ERR_IO);
    EXPECT_EQ(request_buffer(other, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);
    EXPECT_EQ(g_present_control_display.present_calls, static_cast<uint32_t>(3));
    EXPECT_EQ(g_present_control_display.create_calls, static_cast<uint32_t>(0));

    // With no file in control, a present the display refuses takes none, so another file's creation succeeds
    ASSERT_TRUE(closes_before_deadline(presenter));
    EXPECT_EQ(present_buffer(other, page, 0, 0), fs::ERR_IO);
    g_present_control_display.present_answer = gfxfb::OK;
    ASSERT_NE(request_small_buffer(next, page).handle, static_cast<uint32_t>(0));
    EXPECT_EQ(present_buffer(other, page, 0, 0), fs::ERR_BUSY);

    // Once `next`'s present ends failed and `next` closes, `other`'s present fails with ERR_IO and takes no control, as
    // `last`'s creation shows, on a fake still creating once failed, which no display keeping the contract would do
    ASSERT_EQ(present_buffer(next, page, 0, 0), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_FAILED);
    ASSERT_TRUE(closes_before_deadline(next));
    EXPECT_EQ(present_buffer(other, page, 0, 0), fs::ERR_IO);
    fs::file* last = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(last);
    EXPECT_NE(request_small_buffer(last, page).handle, static_cast<uint32_t>(0));
    EXPECT_EQ(present_buffer(other, page, 0, 0), fs::ERR_BUSY);
    EXPECT_EQ(g_present_control_display.present_calls, static_cast<uint32_t>(5));

    ASSERT_TRUE(closes_before_deadline(last));
    ASSERT_TRUE(closes_before_deadline(other));
}

static recording_display g_present_while_creating_display;

// The files the hook's presents are made on, the page holding their requests, and what those presents returned
static fs::file* g_creating_file;
static fs::file* g_presenting_rival;
static user_page* g_creating_page;
static int32_t g_rival_present_result;
static int32_t g_creating_present_result;
static int32_t g_creating_second_present_result;
static int32_t g_creating_display_refusal_result;
static int32_t g_creating_unheld_handle_result;
static int32_t g_creating_empty_region_result;
static int32_t g_rival_second_present_result;
static int32_t g_rival_unheld_handle_result;
static int32_t g_rival_empty_region_result;

// Runs inside the creating file's creation: presents through the rival, twice through that file on output 0, thrice on
// output 1 (a display refusal, an unheld handle, an empty region), thrice by the rival, then refuses the creation
static void present_on_both_files_while_creating() {
    *g_creating_page->at<gfxfb::gfxfb_present_args>(0) = {
        .output = 0,
        .handle = 0,
    };

    g_rival_present_result = fs::ioctl(g_presenting_rival, gfxfb::GFXFB_PRESENT, g_creating_page->addr);
    g_creating_present_result = fs::ioctl(g_creating_file, gfxfb::GFXFB_PRESENT, g_creating_page->addr);
    g_creating_second_present_result = fs::ioctl(g_creating_file, gfxfb::GFXFB_PRESENT, g_creating_page->addr);

    g_creating_page->at<gfxfb::gfxfb_present_args>(0)->output = 1;
    g_present_while_creating_display.present_answer = gfxfb::ERR_REFUSED;
    g_creating_display_refusal_result = fs::ioctl(g_creating_file, gfxfb::GFXFB_PRESENT, g_creating_page->addr);
    g_present_while_creating_display.present_answer = gfxfb::OK;

    g_creating_page->at<gfxfb::gfxfb_present_args>(0)->handle = 2;
    g_creating_unheld_handle_result = fs::ioctl(g_creating_file, gfxfb::GFXFB_PRESENT, g_creating_page->addr);
    write_empty_region_present(*g_creating_page);
    g_creating_empty_region_result = fs::ioctl(g_creating_file, gfxfb::GFXFB_PRESENT, g_creating_page->addr);

    *g_creating_page->at<gfxfb::gfxfb_present_args>(0) = {
        .output = 1,
        .handle = 0,
    };

    g_rival_second_present_result = fs::ioctl(g_presenting_rival, gfxfb::GFXFB_PRESENT, g_creating_page->addr);
    g_creating_page->at<gfxfb::gfxfb_present_args>(0)->handle = 2;
    g_rival_unheld_handle_result = fs::ioctl(g_presenting_rival, gfxfb::GFXFB_PRESENT, g_creating_page->addr);
    write_empty_region_present(*g_creating_page);
    g_rival_empty_region_result = fs::ioctl(g_presenting_rival, gfxfb::GFXFB_PRESENT, g_creating_page->addr);
    g_present_while_creating_display.create_result = gfxfb::ERR_NO_MEMORY;
}

// A creation in the display reserves control against a rival's present, and its file's present that succeeds meanwhile
// takes control, which refused presents of either file and the creation's refusal all leave with that file
TEST(gfxfb, a_present_during_a_creation_of_its_file_takes_control) {
    light_output(g_present_while_creating_display, 0, false);
    light_output(g_present_while_creating_display, 1, false);
    gfxfb_test_node test = publish_driver_node(&g_present_while_creating_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* creating = fs::open(test.path, fs::O_RDWR);
    fs::file* rival = fs::open(test.path, fs::O_RDWR);
    fs::file* next = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(creating);
    ASSERT_NOT_NULL(rival);
    ASSERT_NOT_NULL(next);

    g_creating_file = creating;
    g_presenting_rival = rival;
    g_creating_page = &page;
    g_rival_present_result = fs::OK;
    g_creating_present_result = fs::ERR_IO;
    g_creating_second_present_result = fs::OK;
    g_creating_display_refusal_result = fs::OK;
    g_creating_unheld_handle_result = fs::OK;
    g_creating_empty_region_result = fs::OK;
    g_rival_second_present_result = fs::OK;
    g_rival_unheld_handle_result = fs::OK;
    g_rival_empty_region_result = fs::OK;
    g_present_while_creating_display.while_creating = present_on_both_files_while_creating;

    uint32_t format = gfxfb::GFXFB_FORMAT_XRGB8888;
    EXPECT_EQ(request_buffer(creating, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_NOMEM);
    EXPECT_EQ(g_rival_present_result, fs::ERR_BUSY);
    ASSERT_EQ(g_creating_present_result, fs::OK);
    EXPECT_EQ(g_creating_second_present_result, fs::ERR_BUSY);
    EXPECT_EQ(g_creating_display_refusal_result, fs::ERR_IO);
    EXPECT_EQ(g_creating_unheld_handle_result, fs::ERR_INVAL);
    EXPECT_EQ(g_creating_empty_region_result, fs::ERR_INVAL);
    EXPECT_EQ(g_rival_second_present_result, fs::ERR_BUSY);
    EXPECT_EQ(g_rival_unheld_handle_result, fs::ERR_INVAL);
    EXPECT_EQ(g_rival_empty_region_result, fs::ERR_INVAL);
    EXPECT_EQ(g_present_while_creating_display.present_calls, static_cast<uint32_t>(2));

    g_present_while_creating_display.create_result = gfxfb::OK;
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(request_buffer(rival, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_BUSY);
    EXPECT_EQ(present_buffer(rival, page, 0, 0), fs::ERR_BUSY);
    EXPECT_EQ(present_buffer(rival, page, 0, 2), fs::ERR_INVAL);

    // Once `creating` closes, a creation the display refuses takes no control, so another file's then succeeds
    ASSERT_TRUE(closes_before_deadline(creating));
    EXPECT_EQ(request_buffer(rival, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);
    EXPECT_NE(request_small_buffer(next, page).handle, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(next));
    ASSERT_TRUE(closes_before_deadline(rival));
}

static recording_display g_refused_while_reserving_display;

// The files the hook's calls are made on, the page holding their requests, and what those calls returned
static fs::file* g_refused_presenter;
static fs::file* g_refusals_rival;
static user_page* g_refusals_page;
static int32_t g_unheld_handle_result;
static int32_t g_rival_after_unheld_handle_result;
static int32_t g_empty_region_result;
static int32_t g_rival_after_empty_region_result;
static int32_t g_display_failure_result;
static int32_t g_rival_after_display_failure_result;
static int32_t g_failed_display_result;
static int32_t g_rival_after_failed_display_result;
static int32_t g_refusals_rival_base_result;
static int32_t g_rival_after_its_base_result;
static int32_t g_refusals_rival_unheld_handle_result;
static int32_t g_rival_after_its_unheld_handle_result;
static int32_t g_refusals_rival_empty_region_result;
static int32_t g_rival_after_its_empty_region_result;

// Runs inside the presenter's creation, which it fails last: presents refused by gfxfb's checks, the display's failure
// and the failed display, then the rival's refused for control and by the checks, each followed by a rival creation
static void refuse_presents_while_reserving() {
    *g_refusals_page->at<gfxfb::gfxfb_present_args>(0) = {
        .output = 0,
        .handle = 2,
    };

    g_unheld_handle_result = fs::ioctl(g_refused_presenter, gfxfb::GFXFB_PRESENT, g_refusals_page->addr);
    write_small_request(*g_refusals_page, 0);
    g_rival_after_unheld_handle_result = fs::ioctl(g_refusals_rival, gfxfb::GFXFB_CREATE_BUFFER, g_refusals_page->addr);

    write_empty_region_present(*g_refusals_page);
    g_empty_region_result = fs::ioctl(g_refused_presenter, gfxfb::GFXFB_PRESENT, g_refusals_page->addr);
    write_small_request(*g_refusals_page, 0);
    g_rival_after_empty_region_result = fs::ioctl(g_refusals_rival, gfxfb::GFXFB_CREATE_BUFFER, g_refusals_page->addr);

    *g_refusals_page->at<gfxfb::gfxfb_present_args>(0) = {
        .output = 1,
        .handle = 0,
    };

    g_refused_while_reserving_display.present_answer = gfxfb::ERR_DEVICE;
    g_display_failure_result = fs::ioctl(g_refused_presenter, gfxfb::GFXFB_PRESENT, g_refusals_page->addr);
    write_small_request(*g_refusals_page, 0);
    g_rival_after_display_failure_result = fs::ioctl(
        g_refusals_rival,
        gfxfb::GFXFB_CREATE_BUFFER,
        g_refusals_page->addr
    );
    g_refused_while_reserving_display.present_answer = gfxfb::OK;

    *g_refusals_page->at<gfxfb::gfxfb_present_args>(0) = {
        .output = 0,
        .handle = 0,
    };

    g_failed_display_result = fs::ioctl(g_refused_presenter, gfxfb::GFXFB_PRESENT, g_refusals_page->addr);
    write_small_request(*g_refusals_page, 0);
    g_rival_after_failed_display_result = fs::ioctl(
        g_refusals_rival,
        gfxfb::GFXFB_CREATE_BUFFER,
        g_refusals_page->addr
    );

    // A rival admitted past control would fail with ERR_IO for the failed display, not with ERR_BUSY
    *g_refusals_page->at<gfxfb::gfxfb_present_args>(0) = {
        .output = 1,
        .handle = 0,
    };

    g_refusals_rival_base_result = fs::ioctl(g_refusals_rival, gfxfb::GFXFB_PRESENT, g_refusals_page->addr);
    write_small_request(*g_refusals_page, 0);
    g_rival_after_its_base_result = fs::ioctl(g_refusals_rival, gfxfb::GFXFB_CREATE_BUFFER, g_refusals_page->addr);

    *g_refusals_page->at<gfxfb::gfxfb_present_args>(0) = {
        .output = 1,
        .handle = 2,
    };

    g_refusals_rival_unheld_handle_result = fs::ioctl(g_refusals_rival, gfxfb::GFXFB_PRESENT, g_refusals_page->addr);
    write_small_request(*g_refusals_page, 0);
    g_rival_after_its_unheld_handle_result = fs::ioctl(
        g_refusals_rival,
        gfxfb::GFXFB_CREATE_BUFFER,
        g_refusals_page->addr
    );

    write_empty_region_present(*g_refusals_page);
    g_refusals_rival_empty_region_result = fs::ioctl(g_refusals_rival, gfxfb::GFXFB_PRESENT, g_refusals_page->addr);
    write_small_request(*g_refusals_page, 0);
    g_rival_after_its_empty_region_result = fs::ioctl(
        g_refusals_rival,
        gfxfb::GFXFB_CREATE_BUFFER,
        g_refusals_page->addr
    );

    g_refused_while_reserving_display.create_result = gfxfb::ERR_NO_MEMORY;
}

// A present refused during a creation, by gfxfb's checks, the display's failure, the failed display or control, keeps
// the creating file's reservation and takes no control, so the rival's creation fails with ERR_BUSY meanwhile
TEST(gfxfb, a_present_refused_during_a_creation_takes_no_control) {
    light_output(g_refused_while_reserving_display, 0, false);
    light_output(g_refused_while_reserving_display, 1, false);
    gfxfb_test_node test = publish_driver_node(&g_refused_while_reserving_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* presenter = fs::open(test.path, fs::O_RDWR);
    fs::file* rival = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(presenter);
    ASSERT_NOT_NULL(rival);

    g_refused_presenter = presenter;
    g_refusals_rival = rival;
    g_refusals_page = &page;
    g_unheld_handle_result = fs::OK;
    g_rival_after_unheld_handle_result = fs::OK;
    g_empty_region_result = fs::OK;
    g_rival_after_empty_region_result = fs::OK;
    g_display_failure_result = fs::OK;
    g_rival_after_display_failure_result = fs::OK;
    g_failed_display_result = fs::OK;
    g_rival_after_failed_display_result = fs::OK;
    g_refusals_rival_base_result = fs::OK;
    g_rival_after_its_base_result = fs::OK;
    g_refusals_rival_unheld_handle_result = fs::OK;
    g_rival_after_its_unheld_handle_result = fs::OK;
    g_refusals_rival_empty_region_result = fs::OK;
    g_rival_after_its_empty_region_result = fs::OK;
    g_refused_while_reserving_display.while_creating = refuse_presents_while_reserving;

    uint32_t format = gfxfb::GFXFB_FORMAT_XRGB8888;
    EXPECT_EQ(request_buffer(presenter, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, format), fs::ERR_NOMEM);
    EXPECT_EQ(g_unheld_handle_result, fs::ERR_INVAL);
    EXPECT_EQ(g_rival_after_unheld_handle_result, fs::ERR_BUSY);
    EXPECT_EQ(g_empty_region_result, fs::ERR_INVAL);
    EXPECT_EQ(g_rival_after_empty_region_result, fs::ERR_BUSY);
    EXPECT_EQ(g_display_failure_result, fs::ERR_IO);
    EXPECT_EQ(g_rival_after_display_failure_result, fs::ERR_BUSY);
    EXPECT_EQ(g_failed_display_result, fs::ERR_IO);
    EXPECT_EQ(g_rival_after_failed_display_result, fs::ERR_BUSY);
    EXPECT_EQ(g_refusals_rival_base_result, fs::ERR_BUSY);
    EXPECT_EQ(g_rival_after_its_base_result, fs::ERR_BUSY);
    EXPECT_EQ(g_refusals_rival_unheld_handle_result, fs::ERR_INVAL);
    EXPECT_EQ(g_rival_after_its_unheld_handle_result, fs::ERR_BUSY);
    EXPECT_EQ(g_refusals_rival_empty_region_result, fs::ERR_INVAL);
    EXPECT_EQ(g_rival_after_its_empty_region_result, fs::ERR_BUSY);
    EXPECT_EQ(g_refused_while_reserving_display.present_calls, static_cast<uint32_t>(1));
    EXPECT_EQ(g_refused_while_reserving_display.create_calls, static_cast<uint32_t>(1));

    // The presenter took no control, so the rival creates, on a fake still creating once failed, which no display
    // keeping the contract would do
    g_refused_while_reserving_display.create_result = gfxfb::OK;
    EXPECT_NE(request_small_buffer(rival, page).handle, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(rival));
    ASSERT_TRUE(closes_before_deadline(presenter));
}

static recording_display g_serialized_display;

// The first present's node, file and page, the second's task, whether that task blocked before its present reached the
// display, the presents the display had taken by then, the second present's result and the flag set once it returned
static gfxfb_test_node g_serialized_node;
static fs::file* g_serialized_file;
static user_page* g_serialized_page;
static sched::task* g_second_presenter;
static bool g_second_presenter_blocked;
static uint32_t g_present_calls_while_blocked;
static int32_t g_second_present_result;
static sync::atomic<uint32_t> g_second_present_done;

// Sends the first present's request again from the page's start, on a task of its own
static void present_from_a_second_task(void*) {
    {
        user_space_scope scope(g_serialized_page->ctx);
        g_second_present_result = fs::ioctl(g_serialized_file, gfxfb::GFXFB_PRESENT, g_serialized_page->addr);
    }

    g_second_present_done.store_release(1);
    sched::exit(0);
}

// Runs inside the first present's display call: ends that present latched and reads its event, then starts the second
// present's task and records whether it blocks and how many presents the display has taken meanwhile
static void present_again_while_presenting() {
    end_present(g_serialized_node, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    read_event(g_serialized_file);
    g_second_presenter = test_helpers::start_pinned_task(present_from_a_second_task, nullptr, "gfxfb_present");
    g_second_presenter_blocked = g_second_presenter && test_helpers::blocks_before_deadline(g_second_presenter);
    g_present_calls_while_blocked = g_serialized_display.present_calls;
}

// A second present for an output waits for the display call of the first, even once the output holds no present, and
// reaches the display after it, so the display never runs two presents for one output at once
TEST(gfxfb, presents_for_one_output_reach_the_display_one_at_a_time) {
    light_output(g_serialized_display, 0, false);
    g_serialized_node = publish_driver_node(&g_serialized_display);
    ASSERT_NOT_NULL(g_serialized_node.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(g_serialized_node.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    g_serialized_file = file;
    g_serialized_page = &page;
    g_second_presenter = nullptr;
    g_second_presenter_blocked = false;
    g_present_calls_while_blocked = 0;
    g_second_present_result = fs::ERR_IO;
    g_second_present_done.store_relaxed(0);
    g_serialized_display.while_presenting = present_again_while_presenting;

    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    ASSERT_NOT_NULL(g_second_presenter);
    EXPECT_TRUE(g_second_presenter_blocked);
    EXPECT_EQ(g_present_calls_while_blocked, static_cast<uint32_t>(1));

    ASSERT_TRUE(spin_wait(g_second_present_done));
    ASSERT_EQ(g_second_present_result, fs::OK);
    EXPECT_EQ(g_serialized_display.present_calls, static_cast<uint32_t>(2));
    EXPECT_EQ(g_serialized_display.presented, &g_serialized_display.bases[0]);

    unpin(g_second_presenter);
    end_present(g_serialized_node, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_event_display;

// The frame count and time the test's ends report, so an event that drops or swaps them fails its check
static constexpr uint64_t LATCH_FRAME = 120;
static constexpr uint64_t LATCH_TIME_NS = 5000000000;

// Each end of a present the display accepted reaches the presenting file alone as one event naming the output, the end
// and the handle, with the reported frame and time only for a latch, while a present the display refuses leaves none
TEST(gfxfb, an_ended_present_reaches_its_file_as_an_event) {
    light_output(g_event_display, 0, false);
    light_output(g_event_display, 1, false);
    gfxfb_test_node test = publish_driver_node(&g_event_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    fs::file* other = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);
    ASSERT_NOT_NULL(other);

    uint32_t latched = create_output_0_buffer(file, page);
    ASSERT_NE(latched, static_cast<uint32_t>(0));
    g_event_display.present_answer = gfxfb::ERR_REFUSED;
    EXPECT_EQ(present_buffer(file, page, 0, latched), fs::ERR_IO);
    g_event_display.present_answer = gfxfb::OK;
    ASSERT_EQ(present_buffer(file, page, 0, latched), fs::OK);
    EXPECT_EQ(poll_file(file), static_cast<uint32_t>(0));

    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED, LATCH_FRAME, LATCH_TIME_NS);
    EXPECT_EQ(poll_file(file), sync::POLL_IN);
    EXPECT_EQ(poll_file(other), static_cast<uint32_t>(0));
    gfxfb::gfxfb_event event = read_event(file);
    EXPECT_EQ(event.type, gfxfb::GFXFB_EVENT_PRESENT);
    EXPECT_EQ(event.output, static_cast<uint32_t>(0));
    EXPECT_EQ(event.status, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(event.handle, latched);
    EXPECT_EQ(event.frame, LATCH_FRAME);
    EXPECT_EQ(event.timestamp_ns, LATCH_TIME_NS);
    EXPECT_EQ(poll_file(file), static_cast<uint32_t>(0));

    // A dark or failed end carries no frame or time, even when the display reports them
    ASSERT_EQ(present_buffer(file, page, 1, 0), fs::OK);
    end_present(test, 1, gfxfb::GFXFB_PRESENT_DARK, LATCH_FRAME, LATCH_TIME_NS);
    event = read_event(file);
    EXPECT_EQ(event.output, static_cast<uint32_t>(1));
    EXPECT_EQ(event.status, gfxfb::GFXFB_PRESENT_DARK);
    EXPECT_EQ(event.handle, static_cast<uint32_t>(0));
    EXPECT_EQ(event.frame, static_cast<uint64_t>(0));
    EXPECT_EQ(event.timestamp_ns, static_cast<uint64_t>(0));

    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_FAILED, LATCH_FRAME, LATCH_TIME_NS);
    event = read_event(file);
    EXPECT_EQ(event.status, gfxfb::GFXFB_PRESENT_FAILED);
    EXPECT_EQ(event.frame, static_cast<uint64_t>(0));
    EXPECT_EQ(event.timestamp_ns, static_cast<uint64_t>(0));
    ASSERT_TRUE(reads_before_deadline(file, sizeof(gfxfb::gfxfb_event), fs::O_NONBLOCK, fs::ERR_AGAIN));

    ASSERT_TRUE(closes_before_deadline(other));
    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_whole_events_display;

// A read returns as many whole events as fit, oldest first, across the wrap of the client's event slots, and fails, at
// once and taking no event, with ERR_INVAL below one event's size and with ERR_AGAIN when non-blocking and empty
TEST(gfxfb, a_read_returns_whole_events_oldest_first) {
    light_output(g_whole_events_display, 0, false);
    light_output(g_whole_events_display, 1, false);
    gfxfb_test_node test = publish_driver_node(&g_whole_events_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    gfxfb::gfxfb_event events[3] = {};
    size_t event_size = sizeof(gfxfb::gfxfb_event);
    ASSERT_TRUE(reads_before_deadline(file, sizeof(events), fs::O_NONBLOCK, fs::ERR_AGAIN));
    ASSERT_TRUE(reads_before_deadline(file, event_size - 1, 0, fs::ERR_INVAL));

    // Reading each event before the next present leaves `first_event` at the last slot, so the next two events wrap
    for (uint32_t i = 1; i < gfxfb::CLIENT_EVENTS_MAX; i++) {
        ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
        end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
        ASSERT_EQ(read_event(file).output, static_cast<uint32_t>(0));
    }

    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    ASSERT_EQ(present_buffer(file, page, 1, 0), fs::OK);
    end_present(test, 1, gfxfb::GFXFB_PRESENT_LATCHED);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_DARK);
    EXPECT_EQ(present_buffer(file, page, 0, 0), fs::ERR_BUSY);
    EXPECT_EQ(fs::read(file, events, event_size - 1, fs::O_NONBLOCK), static_cast<ssize_t>(fs::ERR_INVAL));
    EXPECT_EQ(fs::read(file, events, sizeof(events), fs::O_NONBLOCK), static_cast<ssize_t>(2 * event_size));
    EXPECT_EQ(events[0].output, static_cast<uint32_t>(1));
    EXPECT_EQ(events[1].output, static_cast<uint32_t>(0));
    EXPECT_EQ(events[1].status, gfxfb::GFXFB_PRESENT_DARK);
    EXPECT_EQ(events[2].type, static_cast<uint32_t>(0));

    // A size between one event and two takes the oldest event alone and leaves the next
    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    ASSERT_EQ(present_buffer(file, page, 1, 0), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    end_present(test, 1, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(fs::read(file, events, event_size + 1, fs::O_NONBLOCK), static_cast<ssize_t>(event_size));
    EXPECT_EQ(events[0].output, static_cast<uint32_t>(0));
    EXPECT_EQ(read_event(file).output, static_cast<uint32_t>(1));

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_unread_event_display;

// A file's present fails with ERR_BUSY, reaching no display and keeping the file's control, while the file holds the
// unread event of its last present on that output, which reading alone clears, and a refused present adds no event
TEST(gfxfb, an_unread_event_fails_the_next_present_on_its_output) {
    light_output(g_unread_event_display, 0, false);
    light_output(g_unread_event_display, 1, false);
    gfxfb_test_node test = publish_driver_node(&g_unread_event_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    fs::file* rival = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);
    ASSERT_NOT_NULL(rival);

    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(present_buffer(file, page, 0, 0), fs::ERR_BUSY);
    EXPECT_EQ(g_unread_event_display.present_calls, static_cast<uint32_t>(1));
    EXPECT_EQ(request_small_buffer(rival, page).handle, static_cast<uint32_t>(0));

    // The file holds no event of output 1, and reading output 0's event, the oldest, ends only output 0's refusal
    ASSERT_EQ(present_buffer(file, page, 1, 0), fs::OK);
    end_present(test, 1, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).output, static_cast<uint32_t>(0));
    EXPECT_EQ(present_buffer(file, page, 1, 0), fs::ERR_BUSY);
    EXPECT_EQ(g_unread_event_display.present_calls, static_cast<uint32_t>(2));

    g_unread_event_display.present_answer = gfxfb::ERR_REFUSED;
    EXPECT_EQ(present_buffer(file, page, 0, 0), fs::ERR_IO);
    g_unread_event_display.present_answer = gfxfb::OK;
    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    EXPECT_EQ(g_unread_event_display.present_calls, static_cast<uint32_t>(4));
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).output, static_cast<uint32_t>(1));
    EXPECT_EQ(read_event(file).output, static_cast<uint32_t>(0));
    ASSERT_TRUE(reads_before_deadline(file, sizeof(gfxfb::gfxfb_event), fs::O_NONBLOCK, fs::ERR_AGAIN));

    ASSERT_TRUE(closes_before_deadline(file));
    EXPECT_NE(request_small_buffer(rival, page).handle, static_cast<uint32_t>(0));
    ASSERT_TRUE(closes_before_deadline(rival));
}

static recording_display g_waiting_reader_display;

// The file the readers' tasks read, what their reads returned and the flags set once each returned
static fs::file* g_waiting_reader_file;
static gfxfb::gfxfb_event g_waited_event;
static ssize_t g_waited_read_result;
static ssize_t g_interrupted_read_result;
static sync::atomic<uint32_t> g_waited_read_done;
static sync::atomic<uint32_t> g_interrupted_read_done;

// Reads one event from the readers' file, waiting for it
static void read_waiting_for_an_event(void*) {
    g_waited_read_result = fs::read(g_waiting_reader_file, &g_waited_event, sizeof(g_waited_event));
    g_waited_read_done.store_release(1);
    sched::exit(0);
}

// Reads one event from the readers' file, which holds none and has no present pending, so only a kill ends the wait
static void read_until_interrupted(void*) {
    gfxfb::gfxfb_event event = {};
    g_interrupted_read_result = fs::read(g_waiting_reader_file, &event, sizeof(event));
    g_interrupted_read_done.store_release(1);
    sched::exit(0);
}

// A blocking read waits for the file's next event and a subscribed poll is triggered by it, while a kill ends a
// blocking read that has no event with ERR_INTR
TEST(gfxfb, the_next_event_wakes_a_blocking_read_and_a_subscribed_poll) {
    light_output(g_waiting_reader_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_waiting_reader_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    g_waiting_reader_file = file;
    g_waited_event = {};
    g_waited_read_result = fs::ERR_IO;
    g_interrupted_read_result = fs::OK;
    g_waited_read_done.store_relaxed(0);
    g_interrupted_read_done.store_relaxed(0);

    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    sched::task* reader = test_helpers::start_pinned_task(read_waiting_for_an_event, nullptr, "gfxfb_read");
    ASSERT_NOT_NULL(reader);
    EXPECT_TRUE(test_helpers::blocks_before_deadline(reader));

    uint32_t ready_before = sync::POLL_IN;
    bool triggered_before = true;
    bool triggered_after = false;
    RUN_ELEVATED({
        sync::poll_table table;
        table.init(sched::current());
        ready_before = file->get_node()->poll(file, &table);
        triggered_before = table.triggered.load_acquire() != 0;
        test.node->present_done(0, {.status = gfxfb::GFXFB_PRESENT_LATCHED, .frame = 0, .timestamp_ns = 0});
        triggered_after = table.triggered.load_acquire() != 0;
        sync::poll_cleanup(table);
    });

    EXPECT_EQ(ready_before, static_cast<uint32_t>(0));
    EXPECT_FALSE(triggered_before);
    EXPECT_TRUE(triggered_after);
    ASSERT_TRUE(spin_wait(g_waited_read_done));
    EXPECT_EQ(g_waited_read_result, static_cast<ssize_t>(sizeof(gfxfb::gfxfb_event)));
    EXPECT_EQ(g_waited_event.status, gfxfb::GFXFB_PRESENT_LATCHED);
    unpin(reader);

    sched::task* interrupted = test_helpers::start_pinned_task(read_until_interrupted, nullptr, "gfxfb_intr");
    ASSERT_NOT_NULL(interrupted);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(interrupted));
    RUN_ELEVATED(sched::force_wake_for_kill(interrupted));
    ASSERT_TRUE(spin_wait(g_interrupted_read_done));
    EXPECT_EQ(g_interrupted_read_result, static_cast<ssize_t>(fs::ERR_INTR));
    unpin(interrupted);

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_unended_close_display;

// The closes that wait for unended presents, the file's and then the rival's
static task_close g_waiting_close;

// A closing file waits, through a kill and keeping control, until each of its unended presents ends, a file that never
// presented closes without waiting, and no end's event reaches another file
TEST(gfxfb, a_closing_file_waits_for_its_unended_presents) {
    light_output(g_unended_close_display, 0, false);
    light_output(g_unended_close_display, 1, false);
    gfxfb_test_node test = publish_driver_node(&g_unended_close_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    fs::file* rival = fs::open(test.path, fs::O_RDWR);
    fs::file* bystander = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);
    ASSERT_NOT_NULL(rival);
    ASSERT_NOT_NULL(bystander);

    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    ASSERT_EQ(present_buffer(file, page, 1, 0), fs::OK);
    sched::task* closer = start_close(g_waiting_close, file);
    ASSERT_NOT_NULL(closer);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(closer));
    EXPECT_EQ(request_small_buffer(rival, page).handle, static_cast<uint32_t>(0));
    ASSERT_TRUE(closes_before_deadline(bystander));
    EXPECT_EQ(g_waiting_close.done.load_acquire(), static_cast<uint32_t>(0));

    // Output 0's end leaves output 1's present unended, so the woken close waits again, as it does after a kill
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(closer));
    RUN_ELEVATED(sched::force_wake_for_kill(closer));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(closer));
    EXPECT_EQ(g_waiting_close.done.load_acquire(), static_cast<uint32_t>(0));
    EXPECT_EQ(request_small_buffer(rival, page).handle, static_cast<uint32_t>(0));

    end_present(test, 1, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_TRUE(spin_wait(g_waiting_close.done));
    unpin(closer);
    EXPECT_NE(request_small_buffer(rival, page).handle, static_cast<uint32_t>(0));
    EXPECT_EQ(poll_file(rival), static_cast<uint32_t>(0));

    // The rival's close waits for its only unended present, on output 0
    ASSERT_EQ(present_buffer(rival, page, 0, 0), fs::OK);
    sched::task* rival_closer = start_close(g_waiting_close, rival);
    ASSERT_NOT_NULL(rival_closer);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(rival_closer));
    EXPECT_EQ(g_waiting_close.done.load_acquire(), static_cast<uint32_t>(0));

    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_TRUE(spin_wait(g_waiting_close.done));
    unpin(rival_closer);
}

static recording_display g_ended_close_display;

// A closing file waits for no present that latched, failed or was refused, even with an event unread, and the next file
// reads none of its events
TEST(gfxfb, a_closing_file_waits_for_no_ended_present) {
    light_output(g_ended_close_display, 0, false);
    light_output(g_ended_close_display, 1, false);
    gfxfb_test_node test = publish_driver_node(&g_ended_close_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    // Output 0's last present is the refused one and output 1's failed one leaves its event unread
    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).output, static_cast<uint32_t>(0));
    g_ended_close_display.present_answer = gfxfb::ERR_REFUSED;
    EXPECT_EQ(present_buffer(file, page, 0, 0), fs::ERR_IO);
    g_ended_close_display.present_answer = gfxfb::OK;
    ASSERT_EQ(present_buffer(file, page, 1, 0), fs::OK);
    end_present(test, 1, gfxfb::GFXFB_PRESENT_FAILED);
    ASSERT_TRUE(closes_before_deadline(file));

    // The failed end left the display failed, so the next file's present fails without reaching it
    fs::file* next = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(next);
    ASSERT_TRUE(reads_before_deadline(next, sizeof(gfxfb::gfxfb_event), fs::O_NONBLOCK, fs::ERR_AGAIN));
    EXPECT_EQ(present_buffer(next, page, 0, 0), fs::ERR_IO);
    EXPECT_EQ(g_ended_close_display.present_calls, static_cast<uint32_t>(3));

    ASSERT_TRUE(closes_before_deadline(next));
}

static recording_display g_given_back_display;

// The controlling files' closes, run on tasks of their own so a test can end their presents
static task_close g_controller_close;

// A closing controller presents the base buffer of each output showing another buffer, naming no region, and waits
// through a kill, keeping control, until those presents end, while a closing bystander presents nothing
TEST(gfxfb, a_closing_controller_presents_each_output_its_base_buffer) {
    light_output(g_given_back_display, 0, false);
    light_output(g_given_back_display, 1, false);
    gfxfb_test_node test = publish_driver_node(&g_given_back_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    fs::file* rival = fs::open(test.path, fs::O_RDWR);
    fs::file* bystander = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);
    ASSERT_NOT_NULL(rival);
    ASSERT_NOT_NULL(bystander);

    // Output 0 shows a buffer whose handle remains and output 1 one whose handle is gone, their events unread
    uint32_t zero = create_output_0_buffer(file, page);
    ASSERT_NE(zero, static_cast<uint32_t>(0));
    uint32_t width = static_cast<uint32_t>(OUTPUT_1_WIDTH);
    uint32_t height = static_cast<uint32_t>(OUTPUT_1_HEIGHT);
    ASSERT_EQ(request_buffer(file, page, width, height, gfxfb::GFXFB_FORMAT_XRGB8888), fs::OK);
    uint32_t one = page.at<gfxfb::gfxfb_buffer_args>(0)->handle;
    ASSERT_EQ(present_one_region(file, page, 0, zero, {0, 0, 1, 1}), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_EQ(present_one_region(file, page, 1, one, {0, 0, 1, 1}), fs::OK);
    end_present(test, 1, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_EQ(destroy_buffer(file, page, one), fs::OK);

    ASSERT_TRUE(closes_before_deadline(bystander));
    EXPECT_EQ(g_given_back_display.present_calls, static_cast<uint32_t>(2));

    sched::task* closer = start_close(g_controller_close, file);
    ASSERT_NOT_NULL(closer);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(closer));
    EXPECT_EQ(g_given_back_display.present_calls, static_cast<uint32_t>(4));
    ASSERT_EQ(g_given_back_display.presented_on[0], &g_given_back_display.bases[0]);
    ASSERT_EQ(g_given_back_display.presented_on[1], &g_given_back_display.bases[1]);
    EXPECT_TRUE(g_given_back_display.named_no_region_on[0]);
    EXPECT_TRUE(g_given_back_display.named_no_region_on[1]);
    ASSERT_TRUE(creates_before_deadline(rival, page, fs::ERR_BUSY));

    // Output 0's end leaves output 1's present unended, so the woken close waits again, as it does after a kill
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(closer));
    RUN_ELEVATED(sched::force_wake_for_kill(closer));
    ASSERT_TRUE(test_helpers::blocks_before_deadline(closer));
    EXPECT_EQ(g_controller_close.done.load_acquire(), static_cast<uint32_t>(0));
    ASSERT_TRUE(creates_before_deadline(rival, page, fs::ERR_BUSY));

    // The ends reach no file, and the buffers they replace are released once the file's handles drop
    end_present(test, 1, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_TRUE(spin_wait(g_controller_close.done));
    unpin(closer);
    ASSERT_TRUE(spin_wait_ge(g_given_back_display.released_count, 2));
    EXPECT_EQ(g_given_back_display.released_count.load_acquire(), static_cast<uint32_t>(2));
    EXPECT_EQ(poll_file(rival), static_cast<uint32_t>(0));
    EXPECT_NE(request_small_buffer(rival, page).handle, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(rival));
}

static recording_display g_queued_close_display;

// A closing controller presents an output's base buffer only once its own present there has ended latched or dark, and
// waits for no such present the display refuses, as it does on a dark output
TEST(gfxfb, a_closing_controller_presents_the_base_buffer_once_its_present_ends) {
    light_output(g_queued_close_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_queued_close_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t shown = create_output_0_buffer(file, page);
    uint32_t queued = create_output_0_buffer(file, page);
    ASSERT_NE(queued, static_cast<uint32_t>(0));
    ASSERT_EQ(present_buffer(file, page, 0, shown), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_EQ(read_event(file).handle, shown);
    ASSERT_EQ(present_buffer(file, page, 0, queued), fs::OK);

    sched::task* closer = start_close(g_controller_close, file);
    ASSERT_NOT_NULL(closer);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(closer));
    EXPECT_EQ(g_queued_close_display.present_calls, static_cast<uint32_t>(2));

    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(closer));
    EXPECT_EQ(g_queued_close_display.present_calls, static_cast<uint32_t>(3));
    ASSERT_EQ(g_queued_close_display.presented, &g_queued_close_display.bases[0]);
    EXPECT_EQ(g_controller_close.done.load_acquire(), static_cast<uint32_t>(0));
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_TRUE(spin_wait(g_controller_close.done));
    unpin(closer);

    // A dark end leaves output 0 showing the next file's `shown`, so the close presents the base buffer there, and the
    // display refuses it, since the output is dark by then
    fs::file* next = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(next);
    shown = create_output_0_buffer(next, page);
    queued = create_output_0_buffer(next, page);
    ASSERT_NE(queued, static_cast<uint32_t>(0));
    ASSERT_EQ(present_buffer(next, page, 0, shown), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_EQ(read_event(next).handle, shown);
    ASSERT_EQ(present_buffer(next, page, 0, queued), fs::OK);

    closer = start_close(g_controller_close, next);
    ASSERT_NOT_NULL(closer);
    ASSERT_TRUE(test_helpers::blocks_before_deadline(closer));
    g_queued_close_display.states[0].lit = false;
    end_present(test, 0, gfxfb::GFXFB_PRESENT_DARK);
    ASSERT_TRUE(spin_wait(g_controller_close.done));
    unpin(closer);
    EXPECT_EQ(g_queued_close_display.present_calls, static_cast<uint32_t>(6));
}

static recording_display g_lit_later_display;

// An output lit only after its node started has no base buffer recorded, so a closing controller presents nothing there
TEST(gfxfb, a_closing_controller_presents_nothing_on_an_output_without_a_base_buffer) {
    gfxfb_test_node test = publish_driver_node(&g_lit_later_display);
    ASSERT_NOT_NULL(test.node);
    light_output(g_lit_later_display, 0, false);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t handle = create_output_0_buffer(file, page);
    ASSERT_NE(handle, static_cast<uint32_t>(0));
    ASSERT_EQ(present_buffer(file, page, 0, handle), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);

    ASSERT_TRUE(closes_before_deadline(file));
    EXPECT_EQ(g_lit_later_display.present_calls, static_cast<uint32_t>(1));
}

static recording_display g_regions_display;

// A present hands the display its regions complete and in order, each checked against the presented buffer up to its
// far edges, and a present naming no region hands none, succeeding though its regions' address cannot be read
TEST(gfxfb, a_present_hands_its_regions_to_the_display) {
    light_output(g_regions_display, 0, false);
    light_output(g_regions_display, 1, false);
    gfxfb_test_node test = publish_driver_node(&g_regions_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t handle = create_output_0_buffer(file, page);
    ASSERT_NE(handle, static_cast<uint32_t>(0));

    // The buffer is 64 by 32, so the second region is its last pixel, the third all of it and the fourth, past the
    // count, outside it
    auto* regions = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    regions[0] = {1, 2, 3, 4};
    regions[1] = {63, 31, 1, 1};
    regions[2] = {0, 0, 64, 32};
    regions[3] = {64, 0, 1, 1};
    ASSERT_EQ(present_regions(file, page, 0, handle, page.addr + RECTS_OFFSET, 3), fs::OK);
    ASSERT_EQ(g_regions_display.presented_region_count, static_cast<uint32_t>(3));
    EXPECT_TRUE(rect_equals(g_regions_display.presented_regions[0], 1, 2, 3, 4));
    EXPECT_TRUE(rect_equals(g_regions_display.presented_regions[1], 63, 31, 1, 1));
    EXPECT_TRUE(rect_equals(g_regions_display.presented_regions[2], 0, 0, 64, 32));
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, handle);

    // Handle 0 on output 1 names its 32 by 8 base buffer, whose last pixel passes
    ASSERT_EQ(present_one_region(file, page, 1, 0, {31, 7, 1, 1}), fs::OK);
    ASSERT_EQ(g_regions_display.presented_region_count, static_cast<uint32_t>(1));
    EXPECT_TRUE(rect_equals(g_regions_display.presented_regions[0], 31, 7, 1, 1));
    end_present(test, 1, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).output, static_cast<uint32_t>(1));

    ASSERT_EQ(present_regions(file, page, 0, handle, page.addr + pmm::PAGE_SIZE, 0), fs::OK);
    EXPECT_TRUE(g_regions_display.got_null_regions);
    EXPECT_EQ(g_regions_display.presented_region_count, static_cast<uint32_t>(0));
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(read_event(file).handle, handle);

    ASSERT_TRUE(closes_giving_output_back(test, g_regions_display, file, 0));
}

static recording_display g_many_regions_display;

// A present of GFXFB_MAX_FLUSH_RECTS regions hands the display its count, its first MAX_RECORDED regions and its last,
// while one more, all readable, or a count whose low 16 bits alone would pass fails with ERR_INVAL before the display
TEST(gfxfb, a_present_names_at_most_gfxfb_max_flush_rects_regions) {
    light_output(g_many_regions_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_many_regions_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    uintptr_t regions = map_many_regions(page);
    ASSERT_NE(regions, static_cast<uintptr_t>(0));
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t handle = create_output_0_buffer(file, page);
    ASSERT_NE(handle, static_cast<uint32_t>(0));

    EXPECT_EQ(present_regions(file, page, 0, handle, regions, MANY_REGION_COUNT), fs::ERR_INVAL);
    EXPECT_EQ(present_regions(file, page, 0, handle, regions, WIDE_REGION_COUNT), fs::ERR_INVAL);
    EXPECT_EQ(g_many_regions_display.present_calls, static_cast<uint32_t>(0));

    // One region first, so regions freed before the display reads them fail this check rather than fault on the 256
    // below, whose copy takes pages of its own
    ASSERT_EQ(present_regions(file, page, 0, handle, regions + sizeof(gfxfb::gfxfb_rect), 1), fs::OK);
    ASSERT_TRUE(rect_equals(g_many_regions_display.last_presented_region, 0, 1, 1, 1));
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);
    ASSERT_EQ(read_event(file).handle, handle);

    ASSERT_EQ(present_regions(file, page, 0, handle, regions, gfxfb::GFXFB_MAX_FLUSH_RECTS), fs::OK);
    ASSERT_EQ(g_many_regions_display.presented_region_count, gfxfb::GFXFB_MAX_FLUSH_RECTS);
    for (uint32_t i = 0; i < recording_display::MAX_RECORDED; i++) {
        gfxfb::gfxfb_rect expected = many_regions_entry(i);
        EXPECT_TRUE(rect_equals(g_many_regions_display.presented_regions[i], expected.x, expected.y, 1, 1));
    }

    gfxfb::gfxfb_rect last = many_regions_entry(gfxfb::GFXFB_MAX_FLUSH_RECTS - 1);
    EXPECT_TRUE(rect_equals(g_many_regions_display.last_presented_region, last.x, last.y, 1, 1));
    end_present(test, 0, gfxfb::GFXFB_PRESENT_LATCHED);

    ASSERT_TRUE(closes_giving_output_back(test, g_many_regions_display, file, 0));
}

static recording_display g_refused_regions_display;

// Regions each breaking one bound of output 0's 64 by 32 buffer: a zero side, a coordinate at an edge, one pixel past a
// far edge, a sum wrapping to a small value from either term, and a field whose low 16 bits alone would fit
static constexpr gfxfb::gfxfb_rect REFUSED_OUTPUT_0_REGIONS[] = {
    {0, 0, 0, 1},
    {0, 0, 1, 0},
    {64, 0, 1, 1},
    {0, 32, 1, 1},
    {60, 0, 5, 1},
    {0, 28, 1, 5},
    {0xFFFFFFF0, 0, 0x20, 1},
    {4, 0, 0xFFFFFFFE, 1},
    {0, 0xFFFFFFF8, 1, 0x10},
    {0, 4, 1, 0xFFFFFFFE},
    {0x10001, 0, 1, 1},
    {0, 0x10001, 1, 1},
    {0, 0, 0x10001, 1},
    {0, 0, 1, 0x10001},
};

// gfxfb refuses with ERR_INVAL before the display a set reserved field, regions it cannot read and a region with a zero
// width or height or not wholly inside the presented buffer, whatever the output's mode, first or last in a list
TEST(gfxfb, present_refuses_empty_outside_and_unreadable_regions) {
    light_output(g_refused_regions_display, 0, false);
    light_output(g_refused_regions_display, 1, false);
    gfxfb_test_node test = publish_driver_node(&g_refused_regions_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    uint32_t handle = create_output_0_buffer(file, page);
    uint32_t small = request_small_buffer(file, page).handle;
    ASSERT_NE(handle, static_cast<uint32_t>(0));
    ASSERT_NE(small, static_cast<uint32_t>(0));

    for (const gfxfb::gfxfb_rect& region : REFUSED_OUTPUT_0_REGIONS) {
        EXPECT_EQ(present_one_region(file, page, 0, handle, region), fs::ERR_INVAL);
    }

    // A list whose last region lies right of the buffer, then one whose first does
    auto* regions = page.at<gfxfb::gfxfb_rect>(RECTS_OFFSET);
    regions[0] = {0, 0, 1, 1};
    regions[1] = {64, 0, 1, 1};
    EXPECT_EQ(present_regions(file, page, 0, handle, page.addr + RECTS_OFFSET, 2), fs::ERR_INVAL);
    regions[0] = {64, 0, 1, 1};
    regions[1] = {0, 0, 1, 1};
    EXPECT_EQ(present_regions(file, page, 0, handle, page.addr + RECTS_OFFSET, 2), fs::ERR_INVAL);

    // A reserved field with only its low or only its high bit set
    const uint32_t reserved_values[] = {0x1, 0x80000000};
    for (uint32_t reserved : reserved_values) {
        *page.at<gfxfb::gfxfb_present_args>(ARGS_OFFSET) = {.output = 0, .handle = handle, .reserved = reserved};
        EXPECT_EQ(file_ioctl(file, page, gfxfb::GFXFB_PRESENT, page.addr + ARGS_OFFSET), fs::ERR_INVAL);
    }

    // Inside output 0's mode but below the 72 by 8 small buffer, and inside the primary's buffer but right of output
    // 1's 32 by 8 base buffer
    EXPECT_EQ(present_one_region(file, page, 0, small, {0, 10, 1, 1}), fs::ERR_INVAL);
    EXPECT_EQ(present_one_region(file, page, 1, 0, {40, 0, 1, 1}), fs::ERR_INVAL);
    EXPECT_EQ(g_refused_regions_display.present_calls, static_cast<uint32_t>(0));

    // A region inside the small buffer but right of output 0's mode reaches the display, which refuses the size, and
    // `file` keeps control through its refusals, so another file's present fails with ERR_BUSY
    EXPECT_EQ(present_one_region(file, page, 0, small, {70, 0, 2, 8}), fs::ERR_INVAL);
    EXPECT_EQ(g_refused_regions_display.present_calls, static_cast<uint32_t>(1));
    fs::file* rival = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(rival);
    ASSERT_EQ(present_buffer(rival, page, 0, 0), fs::ERR_BUSY);
    ASSERT_TRUE(closes_before_deadline(rival));

    // While output 1 holds a present, presents there of regions the kernel cannot read fail with ERR_INVAL rather than
    // ERR_BUSY: at a null address, past the page, and a list whose second region lies past the page's end
    ASSERT_EQ(present_one_region(file, page, 1, 0, {0, 0, 1, 1}), fs::OK);
    size_t last_region_offset = pmm::PAGE_SIZE - sizeof(gfxfb::gfxfb_rect);
    *page.at<gfxfb::gfxfb_rect>(last_region_offset) = {0, 0, 1, 1};
    EXPECT_EQ(present_regions(file, page, 1, 0, 0, 1), fs::ERR_INVAL);
    EXPECT_EQ(present_regions(file, page, 1, 0, page.addr + pmm::PAGE_SIZE, 1), fs::ERR_INVAL);
    EXPECT_EQ(present_regions(file, page, 1, 0, page.addr + last_region_offset, 2), fs::ERR_INVAL);
    EXPECT_EQ(g_refused_regions_display.present_calls, static_cast<uint32_t>(2));
    end_present(test, 1, gfxfb::GFXFB_PRESENT_LATCHED);

    // Once `file` closes, a creation the display refuses takes no control, so another file's then succeeds
    ASSERT_TRUE(closes_before_deadline(file));
    fs::file* refused = fs::open(test.path, fs::O_RDWR);
    fs::file* next = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(refused);
    ASSERT_NOT_NULL(next);
    EXPECT_EQ(request_buffer(refused, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);
    EXPECT_NE(request_small_buffer(next, page).handle, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(next));
    ASSERT_TRUE(closes_before_deadline(refused));
}

static recording_display g_regions_control_display;

// A present's reserved field, count and regions are refused before control, the first two before handle 0's ERR_AGAIN:
// the refusals take no control and, while another file controls the buffers, fail with ERR_INVAL, not ERR_BUSY
TEST(gfxfb, present_checks_its_regions_before_control) {
    light_output(g_regions_control_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_regions_control_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    uintptr_t many_regions = map_many_regions(page);
    ASSERT_NE(many_regions, static_cast<uintptr_t>(0));
    fs::file* controller = fs::open(test.path, fs::O_RDWR);
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(controller);
    ASSERT_NOT_NULL(file);

    // Output 1 is dark, so a present of its handle 0 fails with ERR_AGAIN, but first with ERR_INVAL for a set reserved
    // field or too many regions
    *page.at<gfxfb::gfxfb_present_args>(ARGS_OFFSET) = {.output = 1, .handle = 0, .reserved = 1};
    EXPECT_EQ(file_ioctl(file, page, gfxfb::GFXFB_PRESENT, page.addr + ARGS_OFFSET), fs::ERR_INVAL);
    EXPECT_EQ(present_regions(file, page, 1, 0, many_regions, MANY_REGION_COUNT), fs::ERR_INVAL);
    EXPECT_EQ(present_buffer(file, page, 1, 0), fs::ERR_AGAIN);

    // With no file in control, `file`'s refusals neither reserve nor take control, so `controller`'s creation succeeds
    EXPECT_EQ(present_one_region(file, page, 0, 0, {0, 0, 0, 1}), fs::ERR_INVAL);
    EXPECT_EQ(present_regions(file, page, 0, 0, 0, 1), fs::ERR_INVAL);
    EXPECT_EQ(present_regions(file, page, 0, 0, many_regions, MANY_REGION_COUNT), fs::ERR_INVAL);
    uint32_t small = request_small_buffer(controller, page).handle;
    ASSERT_NE(small, static_cast<uint32_t>(0));

    // While `controller` controls the buffers, `file`'s refusals fail with ERR_INVAL and leave `controller` in control
    // through its present the display refuses, so `file`'s present of one fitting region fails with ERR_BUSY
    EXPECT_EQ(present_one_region(file, page, 0, 0, {64, 0, 1, 1}), fs::ERR_INVAL);
    EXPECT_EQ(present_regions(file, page, 0, 0, mm::USER_STACK_TOP, 1), fs::ERR_INVAL);
    EXPECT_EQ(present_regions(file, page, 0, 0, many_regions, MANY_REGION_COUNT), fs::ERR_INVAL);
    *page.at<gfxfb::gfxfb_present_args>(ARGS_OFFSET) = {.output = 0, .handle = 0, .reserved = 1};
    EXPECT_EQ(file_ioctl(file, page, gfxfb::GFXFB_PRESENT, page.addr + ARGS_OFFSET), fs::ERR_INVAL);
    EXPECT_EQ(present_buffer(controller, page, 0, small), fs::ERR_INVAL);
    EXPECT_EQ(present_one_region(file, page, 0, 0, {0, 0, 1, 1}), fs::ERR_BUSY);
    EXPECT_EQ(g_regions_control_display.present_calls, static_cast<uint32_t>(1));

    // Once both close, a creation the display refuses takes no control, so another file's then succeeds
    ASSERT_TRUE(closes_before_deadline(file));
    ASSERT_TRUE(closes_before_deadline(controller));
    fs::file* refused = fs::open(test.path, fs::O_RDWR);
    fs::file* next = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(refused);
    ASSERT_NOT_NULL(next);
    EXPECT_EQ(request_buffer(refused, page, SMALL_BUFFER_WIDTH, SMALL_BUFFER_HEIGHT, WIDE_FORMAT), fs::ERR_INVAL);
    EXPECT_NE(request_small_buffer(next, page).handle, static_cast<uint32_t>(0));

    ASSERT_TRUE(closes_before_deadline(next));
    ASSERT_TRUE(closes_before_deadline(refused));
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

// `gfxfb::present_done` ends a present only for /dev/gfxfb's display on the output holding it, so it drops an end of
// another display, of an output at or past the display's count or GFXFB_OUTPUTS_MAX, and a second end
TEST(gfxfb, present_done_ends_only_held_presents_of_the_shown_display) {
    light_output(g_registered_display, 1, false);

    user_page page;
    ASSERT_TRUE(page.ready());

    fs::file* file = fs::open("/dev/gfxfb", fs::O_RDWR);
    ASSERT_NOT_NULL(file);
    uint32_t width = static_cast<uint32_t>(OUTPUT_1_WIDTH);
    uint32_t height = static_cast<uint32_t>(OUTPUT_1_HEIGHT);
    ASSERT_EQ(request_buffer(file, page, width, height, gfxfb::GFXFB_FORMAT_XRGB8888), fs::OK);
    uint32_t handle = page.at<gfxfb::gfxfb_buffer_args>(0)->handle;
    ASSERT_EQ(present_buffer(file, page, 1, handle), fs::OK);

    gfxfb::present_result latched = {.status = gfxfb::GFXFB_PRESENT_LATCHED, .frame = 7, .timestamp_ns = 1234};

    RUN_ELEVATED(gfxfb::present_done(&g_refused_display, 1, latched));
    ASSERT_TRUE(reads_before_deadline(file, sizeof(gfxfb::gfxfb_event), fs::O_NONBLOCK, fs::ERR_AGAIN));
    RUN_ELEVATED(gfxfb::present_done(&g_registered_display, recording_display::OUTPUTS, latched));
    ASSERT_TRUE(reads_before_deadline(file, sizeof(gfxfb::gfxfb_event), fs::O_NONBLOCK, fs::ERR_AGAIN));
    RUN_ELEVATED(gfxfb::present_done(&g_registered_display, gfxfb::GFXFB_OUTPUTS_MAX, latched));
    ASSERT_TRUE(reads_before_deadline(file, sizeof(gfxfb::gfxfb_event), fs::O_NONBLOCK, fs::ERR_AGAIN));

    RUN_ELEVATED(gfxfb::present_done(&g_registered_display, 1, latched));
    gfxfb::gfxfb_event event = read_event(file);
    EXPECT_EQ(event.type, gfxfb::GFXFB_EVENT_PRESENT);
    EXPECT_EQ(event.output, static_cast<uint32_t>(1));
    EXPECT_EQ(event.status, gfxfb::GFXFB_PRESENT_LATCHED);
    EXPECT_EQ(event.frame, static_cast<uint64_t>(7));
    EXPECT_EQ(event.timestamp_ns, static_cast<uint64_t>(1234));

    RUN_ELEVATED(gfxfb::present_done(&g_registered_display, 1, latched));
    EXPECT_TRUE(reads_before_deadline(file, sizeof(gfxfb::gfxfb_event), fs::O_NONBLOCK, fs::ERR_AGAIN));

    ASSERT_TRUE(closes_before_deadline(file));
}

static recording_display g_failed_end_display;

// The node drops a second end on an output whose present ended failed, though the output keeps that present for good
TEST(gfxfb, present_done_drops_a_second_end_of_a_failed_present) {
    light_output(g_failed_end_display, 0, false);
    gfxfb_test_node test = publish_driver_node(&g_failed_end_display);
    ASSERT_NOT_NULL(test.node);

    user_page page;
    ASSERT_TRUE(page.ready());
    fs::file* file = fs::open(test.path, fs::O_RDWR);
    ASSERT_NOT_NULL(file);

    ASSERT_EQ(present_buffer(file, page, 0, 0), fs::OK);
    end_present(test, 0, gfxfb::GFXFB_PRESENT_FAILED);
    EXPECT_EQ(read_event(file).status, gfxfb::GFXFB_PRESENT_FAILED);

    end_present(test, 0, gfxfb::GFXFB_PRESENT_FAILED);
    EXPECT_TRUE(reads_before_deadline(file, sizeof(gfxfb::gfxfb_event), fs::O_NONBLOCK, fs::ERR_AGAIN));

    ASSERT_TRUE(closes_before_deadline(file));
}
