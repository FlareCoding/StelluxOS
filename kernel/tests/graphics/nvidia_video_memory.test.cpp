// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/bar1_page_tables.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/video_memory.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_video_memory);

using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_INVALID_ARG;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::ERR_TIMEOUT;
using drivers::nvidia::OK;
using drivers::nvidia::bar1_page_entry;
using drivers::nvidia::bar1_video_memory;
using drivers::nvidia::cpu_mapping;
using drivers::nvidia::registers;
using drivers::nvidia::static_configuration;
using drivers::nvidia::video_memory_region;
using drivers::nvidia::vram_range;
using drivers::nvidia::vram_use;

static constexpr uint64_t PAGE_BYTES = 0x1000;

// Where BAR0's window (NVIDIA's PRAMIN) starts in BAR0, showing video memory from the 64 KiB step its register names,
// and the bits of an offset inside one step
static constexpr uint32_t PRAMIN              = 0x700000;
static constexpr uint64_t OFFSET_IN_STEP_MASK = 0xFFFF;

// The invalidate's registers, one naming the root page directory and one starting it, and the value that starts an
// invalidate of BAR1's cached translations, its trigger bit 31 set
static constexpr uint32_t INVALIDATE_PDB  = 0xB830A0;
static constexpr uint32_t INVALIDATE      = 0xB830B0;
static constexpr uint32_t BAR1_INVALIDATE = 0x80010005;

// NVIDIA's sparse value, which a page table entry holds when it names nothing and an access through it does not fault
static constexpr uint64_t SPARSE_ENTRY = 0x8;

// 256 MiB of video memory whose first MiB is a reserved region holding BAR1's root entry, as `init` asks, and whose
// rest is usable
static constexpr uint64_t VIDEO_MEMORY_BYTES = 0x10000000;
static constexpr uint64_t ROOT_REGION_BYTES  = 0x100000;
static constexpr uint64_t ROOT_OFFSET        = 0x5000;

// The root's offset in 4 KiB units in bits 31:4 of the invalidate's first register, with video memory's aperture 0
static constexpr uint32_t ROOT_PDB = 0x50;

// Where the tests' tables lie, the three directories and then the page tables, and a 256 MiB BAR1 whose window is all
// of it but its last 4 KiB
static constexpr uint64_t TABLES_OFFSET   = 0x200000;
static constexpr uint64_t DIRECTORY_BYTES = 3 * PAGE_BYTES;
static constexpr uint64_t BAR1_PHYS       = 0xE0000000;
static constexpr uint64_t BAR1_BYTES      = 0x10000000;

// A kept console of two pages, so mappings start at BAR1 offset 0x2000
static constexpr uint64_t CONSOLE_BYTES = 2 * PAGE_BYTES;

// Zeroed memory where each window opens, standing in for BAR1's pages up to the first mapping's, which an unmap reads
// through. No test reaches BAR1 past them
static uint64_t bar1_pages[(CONSOLE_BYTES + PAGE_BYTES) / sizeof(uint64_t)];

// A mapping no call hands out, filling `out` before a call so the test sees whether the call wrote it
static constexpr cpu_mapping UNTOUCHED = {.virt = 1, .phys = 1};

// A zeroed fake of BAR0 whose registers and window into video memory are plain memory, so an invalidate's trigger bit
// never clears
struct mapping_bar0 {
    static constexpr size_t BYTES = 0x1000000;

    uint8_t*  bytes = static_cast<uint8_t*>(heap::uzalloc(BYTES));
    registers regs{reinterpret_cast<uintptr_t>(bytes)};

    ~mapping_bar0() { heap::ufree(bytes); }

    uint32_t read(uint32_t offset) const { return *reinterpret_cast<volatile uint32_t*>(bytes + offset); }

    void write(uint32_t offset, uint32_t value) { *reinterpret_cast<volatile uint32_t*>(bytes + offset) = value; }

    // The page table entry for BAR1's page at `bar1_offset` as BAR0's window last showed it, which holds every write of
    // the tests' mappings, since none spans two 64 KiB steps
    uint64_t entry(uint64_t bar1_offset) const {
        uint64_t entry_offset = TABLES_OFFSET + DIRECTORY_BYTES + bar1_offset / PAGE_BYTES * sizeof(uint64_t);
        uint32_t low_word = PRAMIN + static_cast<uint32_t>(entry_offset & OFFSET_IN_STEP_MASK);
        return static_cast<uint64_t>(read(low_word + sizeof(uint32_t))) << 32 | read(low_word);
    }
};

namespace drivers::nvidia {

// Gives the tests the window `set_up_bar1` records, with no tables written or linked, and the records of mappings whose
// invalidate finished, which the fake cannot give
struct video_memory_test_access {
    // Records the window, still closed, with an invalidate timeout of 0
    static void record_window(bar1_video_memory& memory, registers regs, uint64_t bar1_bytes, uint64_t console_bytes) {
        memory.record_window(regs, BAR1_PHYS, bar1_bytes, ROOT_OFFSET, TABLES_OFFSET, console_bytes);
        memory.m_invalidate_timeout_ns = 0;
    }

    // Records the window and opens it
    static void record_and_open_window(
        bar1_video_memory& memory,
        registers regs,
        uint64_t bar1_bytes,
        uint64_t console_bytes
    ) {
        record_window(memory, regs, bar1_bytes, console_bytes);
        memory.open_window(reinterpret_cast<uintptr_t>(bar1_pages));
    }

    // The window's size, 0 while it is closed
    static uint64_t window_bytes(const bar1_video_memory& memory) { return memory.m_window_bytes; }

    // Records a mapping of `range` at `bar1_offset` as one whose invalidate finished
    static bool hold(bar1_video_memory& memory, uint64_t bar1_offset, const vram_range& range) {
        bar1_video_memory::mapping* held = heap::ualloc_new<bar1_video_memory::mapping>();
        if (held == nullptr) {
            return false;
        }

        held->bar1_offset = bar1_offset;
        held->range = range;
        memory.m_mappings.insert_sorted(
            held,
            [](const bar1_video_memory::mapping* a, const bar1_video_memory::mapping* b) {
                return a->bar1_offset < b->bar1_offset;
            }
        );
        return true;
    }

    // Points the entries of the held mapping of `range` at its pages, as a mapping writes them before its invalidate
    static int32_t write_entries(bar1_video_memory& memory, const vram_range& range) {
        bar1_video_memory::mapping* held = memory.find_mapping(range);
        if (held == nullptr) {
            return ERR_INVALID_ARG;
        }

        return memory.write_run_entries(*held, true);
    }
};

} // namespace drivers::nvidia

using drivers::nvidia::video_memory_test_access;

// A usable region of `size` bytes at `offset`
static video_memory_region usable_region(uint64_t offset, uint64_t size) {
    video_memory_region region = {};
    region.offset = offset;
    region.size = size;
    return region;
}

// A reserved region of `size` bytes at `offset`, all of them reserved
static video_memory_region reserved_region(uint64_t offset, uint64_t size) {
    video_memory_region region = usable_region(offset, size);
    region.reserved_bytes = size;
    region.is_reserved = true;
    return region;
}

// The tests' region table: the reserved region holding BAR1's root entry, then the rest of video memory usable
static static_configuration two_region_configuration() {
    static_configuration configuration = {};
    configuration.regions[0] = reserved_region(0, ROOT_REGION_BYTES);
    configuration.regions[1] = usable_region(ROOT_REGION_BYTES, VIDEO_MEMORY_BYTES - ROOT_REGION_BYTES);
    configuration.region_count = 2;
    configuration.bar1_page_directory_offset = ROOT_OFFSET;
    return configuration;
}

// How many of the `pages` entries from BAR1's page at `bar1_offset` hold NVIDIA's sparse value
static uint64_t sparse_entries(const mapping_bar0& bar0, uint64_t bar1_offset, uint64_t pages) {
    uint64_t sparse = 0;
    for (uint64_t i = 0; i < pages; i++) {
        sparse += bar0.entry(bar1_offset + i * PAGE_BYTES) == SPARSE_ENTRY;
    }

    return sparse;
}

TEST(nvidia_video_memory, a_mapping_before_bar1s_window_is_opened_is_refused) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);

    vram_range page = {};
    ASSERT_EQ(memory.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, page), OK);

    // Refused before the window is set up, and once it is set up but not opened
    cpu_mapping out = UNTOUCHED;
    EXPECT_EQ(memory.map_for_cpu(page, out), ERR_NO_MEMORY);
    video_memory_test_access::record_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);
    EXPECT_EQ(memory.map_for_cpu(page, out), ERR_NO_MEMORY);
    EXPECT_EQ(out.virt, UNTOUCHED.virt);
    EXPECT_EQ(out.phys, UNTOUCHED.phys);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES), 0u);
    EXPECT_EQ(bar0.read(INVALIDATE), 0u);
}

TEST(nvidia_video_memory, bar1_tables_ending_past_128_gib_are_refused_and_leave_the_window_closed) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    // A usable page, too small for the four pages of tables a BAR1 of 17 pages takes, and four usable pages from two
    // pages below 128 GiB, the reach of an entry's address field, so the tables end past it
    static constexpr uint64_t REACH = 1ULL << 37;
    static_configuration configuration = two_region_configuration();
    configuration.regions[1] = usable_region(ROOT_REGION_BYTES, PAGE_BYTES);
    configuration.regions[2] = usable_region(REACH - 2 * PAGE_BYTES, 4 * PAGE_BYTES);
    configuration.region_count = 3;

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(configuration, REACH + 2 * PAGE_BYTES), OK);
    EXPECT_EQ(memory.set_up_bar1(bar0.regs, BAR1_PHYS, 17 * PAGE_BYTES, ROOT_OFFSET, CONSOLE_BYTES), ERR_BAD_STATE);
    memory.open_window(reinterpret_cast<uintptr_t>(bar1_pages));
    EXPECT_EQ(video_memory_test_access::window_bytes(memory), 0u);

    // The tables' memory stays handed out, so only the page below them is left, and the closed window maps nothing
    vram_range page = {};
    vram_range none_left = {};
    ASSERT_EQ(memory.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, page), OK);
    EXPECT_EQ(memory.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, none_left), ERR_NO_MEMORY);
    cpu_mapping out = UNTOUCHED;
    EXPECT_EQ(memory.map_for_cpu(page, out), ERR_NO_MEMORY);
    EXPECT_EQ(out.virt, UNTOUCHED.virt);

    // Neither the root entry nor the invalidate was written
    EXPECT_EQ(bar0.read(PRAMIN + static_cast<uint32_t>(ROOT_OFFSET)), 0u);
    EXPECT_EQ(bar0.read(INVALIDATE), 0u);
}

TEST(nvidia_video_memory, a_range_not_handed_out_or_part_of_one_is_refused_writing_nothing) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);

    vram_range allocation = {};
    ASSERT_EQ(memory.allocate(2 * PAGE_BYTES, PAGE_BYTES, vram_use::general, allocation), OK);

    vram_range never_handed_out = {.offset = allocation.offset + 4 * PAGE_BYTES, .size = PAGE_BYTES};
    vram_range first_half = {.offset = allocation.offset, .size = PAGE_BYTES};

    cpu_mapping out = UNTOUCHED;
    EXPECT_EQ(memory.map_for_cpu(never_handed_out, out), ERR_INVALID_ARG);
    EXPECT_EQ(memory.map_for_cpu(first_half, out), ERR_INVALID_ARG);
    EXPECT_EQ(out.virt, UNTOUCHED.virt);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES), 0u);
    EXPECT_EQ(bar0.read(INVALIDATE), 0u);
}

TEST(nvidia_video_memory, a_range_mapped_already_is_refused_writing_nothing) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);

    vram_range page = {};
    ASSERT_EQ(memory.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, page), OK);
    ASSERT_TRUE(video_memory_test_access::hold(memory, CONSOLE_BYTES, page));

    cpu_mapping out = UNTOUCHED;
    EXPECT_EQ(memory.map_for_cpu(page, out), ERR_INVALID_ARG);
    EXPECT_EQ(out.virt, UNTOUCHED.virt);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES + PAGE_BYTES), 0u);
    EXPECT_EQ(bar0.read(INVALIDATE), 0u);
}

TEST(nvidia_video_memory, a_mapping_whose_invalidate_times_out_leaves_its_entries_sparse_and_out_unchanged) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);

    vram_range range = {};
    ASSERT_EQ(memory.allocate(3 * PAGE_BYTES, PAGE_BYTES, vram_use::general, range), OK);

    cpu_mapping out = UNTOUCHED;
    EXPECT_EQ(memory.map_for_cpu(range, out), ERR_TIMEOUT);
    EXPECT_EQ(out.virt, UNTOUCHED.virt);
    EXPECT_EQ(out.phys, UNTOUCHED.phys);

    // The invalidate was asked for under BAR1's root, and the run's entries alone were left sparse
    EXPECT_EQ(bar0.read(INVALIDATE_PDB), ROOT_PDB);
    EXPECT_EQ(bar0.read(INVALIDATE), BAR1_INVALIDATE);
    EXPECT_EQ(sparse_entries(bar0, CONSOLE_BYTES, 3), 3u);
    EXPECT_EQ(bar0.entry(0), 0u);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES + 3 * PAGE_BYTES), 0u);
}

TEST(nvidia_video_memory, a_mapping_longer_than_one_write_of_entries_writes_every_entry) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);

    // 65 pages take two writes of at most 64 entries each
    vram_range range = {};
    ASSERT_EQ(memory.allocate(65 * PAGE_BYTES, PAGE_BYTES, vram_use::general, range), OK);

    cpu_mapping out = UNTOUCHED;
    EXPECT_EQ(memory.map_for_cpu(range, out), ERR_TIMEOUT);
    EXPECT_EQ(sparse_entries(bar0, CONSOLE_BYTES, 65), 65u);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES + 65 * PAGE_BYTES), 0u);
}

TEST(nvidia_video_memory, the_entries_of_a_run_name_its_ranges_pages_in_order) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);

    // 65 pages, so the last entry comes from the second write of at most 64
    vram_range range = {.offset = 0x8000000, .size = 65 * PAGE_BYTES};
    ASSERT_TRUE(video_memory_test_access::hold(memory, CONSOLE_BYTES, range));
    ASSERT_EQ(video_memory_test_access::write_entries(memory, range), OK);

    static constexpr uint64_t CHECKED_PAGES[] = {0, 63, 64};
    for (uint64_t page : CHECKED_PAGES) {
        uint64_t page_offset = page * PAGE_BYTES;
        EXPECT_EQ(bar0.entry(CONSOLE_BYTES + page_offset), bar1_page_entry(range.offset + page_offset));
    }

    EXPECT_EQ(bar0.entry(CONSOLE_BYTES + 65 * PAGE_BYTES), 0u);
}

TEST(nvidia_video_memory, a_failed_mappings_run_is_free_for_the_next_mapping) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);

    vram_range failed = {};
    vram_range next = {};
    ASSERT_EQ(memory.allocate(3 * PAGE_BYTES, PAGE_BYTES, vram_use::general, failed), OK);
    ASSERT_EQ(memory.allocate(3 * PAGE_BYTES, PAGE_BYTES, vram_use::general, next), OK);

    cpu_mapping out = UNTOUCHED;
    ASSERT_EQ(memory.map_for_cpu(failed, out), ERR_TIMEOUT);

    // The next mapping takes the same run, so the entry past it stays unwritten
    bar0.write(INVALIDATE, 0);
    EXPECT_EQ(memory.map_for_cpu(next, out), ERR_TIMEOUT);
    EXPECT_EQ(bar0.read(INVALIDATE), BAR1_INVALIDATE);
    EXPECT_EQ(sparse_entries(bar0, CONSOLE_BYTES, 3), 3u);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES + 3 * PAGE_BYTES), 0u);
}

TEST(nvidia_video_memory, an_unmap_whose_invalidate_times_out_leaves_its_entries_sparse_and_its_run_free) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);

    vram_range mapped = {};
    vram_range next = {};
    ASSERT_EQ(memory.allocate(3 * PAGE_BYTES, PAGE_BYTES, vram_use::general, mapped), OK);
    ASSERT_EQ(memory.allocate(3 * PAGE_BYTES, PAGE_BYTES, vram_use::general, next), OK);
    ASSERT_TRUE(video_memory_test_access::hold(memory, CONSOLE_BYTES, mapped));

    memory.unmap_for_cpu(mapped);
    EXPECT_EQ(bar0.read(INVALIDATE_PDB), ROOT_PDB);
    EXPECT_EQ(bar0.read(INVALIDATE), BAR1_INVALIDATE);
    EXPECT_EQ(sparse_entries(bar0, CONSOLE_BYTES, 3), 3u);

    // The next mapping takes the unmapped run, so the entry past it stays unwritten
    bar0.write(INVALIDATE, 0);
    cpu_mapping out = UNTOUCHED;
    EXPECT_EQ(memory.map_for_cpu(next, out), ERR_TIMEOUT);
    EXPECT_EQ(bar0.read(INVALIDATE), BAR1_INVALIDATE);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES + 3 * PAGE_BYTES), 0u);
}

TEST(nvidia_video_memory, releasing_a_mapped_range_unmaps_it_and_returns_its_memory) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);

    vram_range mapped = {};
    ASSERT_EQ(memory.allocate(2 * PAGE_BYTES, PAGE_BYTES, vram_use::general, mapped), OK);
    ASSERT_TRUE(video_memory_test_access::hold(memory, CONSOLE_BYTES, mapped));

    memory.release(mapped);
    EXPECT_EQ(bar0.read(INVALIDATE), BAR1_INVALIDATE);
    EXPECT_EQ(sparse_entries(bar0, CONSOLE_BYTES, 2), 2u);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES + 2 * PAGE_BYTES), 0u);

    // The memory is handed out again, and unmapping the range changes nothing
    vram_range again = {};
    ASSERT_EQ(memory.allocate(2 * PAGE_BYTES, PAGE_BYTES, vram_use::general, again), OK);
    EXPECT_EQ(again.offset, mapped.offset);
    bar0.write(INVALIDATE, 0);
    memory.unmap_for_cpu(again);
    EXPECT_EQ(bar0.read(INVALIDATE), 0u);
}

TEST(nvidia_video_memory, a_range_larger_than_the_windows_free_run_is_refused_writing_nothing) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    // A BAR1 of 17 pages, whose window of 16 pages leaves 14 past the console
    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, 17 * PAGE_BYTES, CONSOLE_BYTES);

    vram_range too_large = {};
    vram_range fits = {};
    ASSERT_EQ(memory.allocate(15 * PAGE_BYTES, PAGE_BYTES, vram_use::general, too_large), OK);
    ASSERT_EQ(memory.allocate(14 * PAGE_BYTES, PAGE_BYTES, vram_use::general, fits), OK);

    cpu_mapping out = UNTOUCHED;
    EXPECT_EQ(memory.map_for_cpu(too_large, out), ERR_NO_MEMORY);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES), 0u);
    EXPECT_EQ(bar0.read(INVALIDATE), 0u);

    EXPECT_EQ(memory.map_for_cpu(fits, out), ERR_TIMEOUT);
    EXPECT_EQ(sparse_entries(bar0, CONSOLE_BYTES, 14), 14u);

    // With the 14 pages mapped, one page more has no room
    ASSERT_TRUE(video_memory_test_access::hold(memory, CONSOLE_BYTES, fits));
    vram_range page = {};
    ASSERT_EQ(memory.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, page), OK);
    EXPECT_EQ(memory.map_for_cpu(page, out), ERR_NO_MEMORY);
}

TEST(nvidia_video_memory, a_window_ends_at_512_mib_on_a_larger_bar1) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    // A 1 GiB BAR1 with live mappings up to the last page below 512 MiB
    static constexpr uint64_t WINDOW_BYTES = 0x20000000;
    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, 2 * WINDOW_BYTES, CONSOLE_BYTES);

    uint64_t last_page = WINDOW_BYTES - PAGE_BYTES;
    vram_range below_last_page = {.offset = 0x8000000, .size = last_page - CONSOLE_BYTES};
    ASSERT_TRUE(video_memory_test_access::hold(memory, CONSOLE_BYTES, below_last_page));

    vram_range page = {};
    ASSERT_EQ(memory.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, page), OK);

    cpu_mapping out = UNTOUCHED;
    EXPECT_EQ(memory.map_for_cpu(page, out), ERR_TIMEOUT);
    EXPECT_EQ(bar0.entry(last_page), SPARSE_ENTRY);

    // With the last page mapped too, nothing past 512 MiB is used
    vram_range last = {.offset = 0x9000000, .size = PAGE_BYTES};
    ASSERT_TRUE(video_memory_test_access::hold(memory, last_page, last));
    bar0.write(INVALIDATE, 0);
    EXPECT_EQ(memory.map_for_cpu(page, out), ERR_NO_MEMORY);
    EXPECT_EQ(bar0.entry(WINDOW_BYTES), 0u);
    EXPECT_EQ(bar0.read(INVALIDATE), 0u);
}

TEST(nvidia_video_memory, without_a_console_the_first_mapping_starts_at_bar1s_start) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, 0);

    vram_range page = {};
    ASSERT_EQ(memory.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, page), OK);

    cpu_mapping out = UNTOUCHED;
    EXPECT_EQ(memory.map_for_cpu(page, out), ERR_TIMEOUT);
    EXPECT_EQ(bar0.entry(0), SPARSE_ENTRY);
    EXPECT_EQ(bar0.entry(PAGE_BYTES), 0u);
}

TEST(nvidia_video_memory, a_range_ending_past_128_gib_is_refused_and_one_ending_there_gets_its_entry) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    // A usable region straddling 128 GiB, the reach of an entry's address field
    static constexpr uint64_t REACH = 1ULL << 37;
    static_configuration configuration = two_region_configuration();
    configuration.regions[1] = usable_region(REACH - PAGE_BYTES, 2 * PAGE_BYTES);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(configuration, REACH + PAGE_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);

    vram_range straddling = {};
    ASSERT_EQ(memory.allocate(2 * PAGE_BYTES, PAGE_BYTES, vram_use::general, straddling), OK);

    cpu_mapping out = UNTOUCHED;
    EXPECT_EQ(memory.map_for_cpu(straddling, out), ERR_INVALID_ARG);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES), 0u);

    // The region's first page ends at 128 GiB, so its entry is written and then rewritten sparse
    memory.release(straddling);
    vram_range below_reach = {};
    ASSERT_EQ(memory.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, below_reach), OK);
    EXPECT_EQ(below_reach.offset, REACH - PAGE_BYTES);
    EXPECT_EQ(memory.map_for_cpu(below_reach, out), ERR_TIMEOUT);
    EXPECT_EQ(bar0.read(INVALIDATE), BAR1_INVALIDATE);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES), SPARSE_ENTRY);
}

TEST(nvidia_video_memory, unmapping_a_range_not_mapped_changes_nothing) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);

    vram_range page = {};
    ASSERT_EQ(memory.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, page), OK);

    memory.unmap_for_cpu(page);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES), 0u);
    EXPECT_EQ(bar0.read(INVALIDATE), 0u);
}

TEST(nvidia_video_memory, a_mapping_takes_the_lowest_free_run_that_fits_between_live_mappings) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);

    // Live mappings of one page right after the console and of two pages one page later leave a one-page gap
    vram_range first_live = {.offset = 0x8000000, .size = PAGE_BYTES};
    vram_range second_live = {.offset = 0x9000000, .size = 2 * PAGE_BYTES};
    ASSERT_TRUE(video_memory_test_access::hold(memory, CONSOLE_BYTES, first_live));
    ASSERT_TRUE(video_memory_test_access::hold(memory, CONSOLE_BYTES + 2 * PAGE_BYTES, second_live));

    vram_range two_pages = {};
    vram_range one_page = {};
    ASSERT_EQ(memory.allocate(2 * PAGE_BYTES, PAGE_BYTES, vram_use::general, two_pages), OK);
    ASSERT_EQ(memory.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, one_page), OK);

    cpu_mapping out = UNTOUCHED;
    EXPECT_EQ(memory.map_for_cpu(two_pages, out), ERR_TIMEOUT);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES + PAGE_BYTES), 0u);
    EXPECT_EQ(sparse_entries(bar0, CONSOLE_BYTES + 4 * PAGE_BYTES, 2), 2u);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES + 6 * PAGE_BYTES), 0u);

    bar0.write(INVALIDATE, 0);
    EXPECT_EQ(memory.map_for_cpu(one_page, out), ERR_TIMEOUT);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES + PAGE_BYTES), SPARSE_ENTRY);
}

TEST(nvidia_video_memory, a_dropped_window_maps_nothing_and_forgets_its_mappings) {
    mapping_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    bar1_video_memory memory;
    ASSERT_EQ(memory.init(two_region_configuration(), VIDEO_MEMORY_BYTES), OK);
    video_memory_test_access::record_and_open_window(memory, bar0.regs, BAR1_BYTES, CONSOLE_BYTES);

    vram_range live = {};
    vram_range page = {};
    ASSERT_EQ(memory.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, live), OK);
    ASSERT_EQ(memory.allocate(PAGE_BYTES, PAGE_BYTES, vram_use::general, page), OK);
    ASSERT_TRUE(video_memory_test_access::hold(memory, CONSOLE_BYTES, live));

    memory.drop_window();
    ASSERT_EQ(video_memory_test_access::window_bytes(memory), 0u);

    // A forgotten mapping is no longer refused as mapped already, before any call can reach BAR0
    cpu_mapping out = UNTOUCHED;
    ASSERT_EQ(memory.map_for_cpu(live, out), ERR_NO_MEMORY);
    EXPECT_EQ(memory.map_for_cpu(page, out), ERR_NO_MEMORY);
    memory.unmap_for_cpu(live);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES), 0u);
    EXPECT_EQ(bar0.entry(CONSOLE_BYTES + PAGE_BYTES), 0u);
    EXPECT_EQ(bar0.read(INVALIDATE), 0u);
}
