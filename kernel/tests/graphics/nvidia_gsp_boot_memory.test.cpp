// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/firmware_files.h"
#include "drivers/graphics/nvidia/gsp_boot_memory.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "fs/fs.h"
#include "fw/fw.h"
#include "mm/heap.h"
#include "mm/page_quarantine.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "tests/memory/dma_test_access.h"

TEST_SUITE(nvidia_gsp_boot_memory);

using drivers::nvidia::boot_file_header;
using drivers::nvidia::check_boot_copies;
using drivers::nvidia::ERR_BAD_FIRMWARE;
using drivers::nvidia::ERR_BAD_STATE;
using drivers::nvidia::ERR_NO_MEMORY;
using drivers::nvidia::firmware_files;
using drivers::nvidia::gsp_boot_memory;
using drivers::nvidia::gsp_image_layout;
using drivers::nvidia::OK;
using drivers::nvidia::radix3_table_pages;
using drivers::nvidia::write_radix3_table;

// A radix3 page table's page size and the addresses one of its pages holds, restated here so the tests hold the
// table to NVIDIA's page and entry sizes rather than to the driver's constants
static constexpr uint64_t PAGE_BYTES   = 0x1000;
static constexpr uint64_t PAGE_ENTRIES = 512;

// The most data pages a table maps, the entries of a full level 2 under a level 0 of one page
static constexpr uint64_t MAX_DATA_PAGES = PAGE_ENTRIES * PAGE_ENTRIES * PAGE_ENTRIES;

// A page count that a cut to 32 bits makes 1, so only a count kept in 64 bits refuses it
static constexpr uint64_t PAST_32_BITS_PAGES = (1ULL << 32) + 1;

// The directories down to the suite's own, in creation order. The suite creates only the missing ones and removes
// exactly those, so an installed package's files are never touched.
static const char* const FIXTURE_DIRECTORIES[] = {
    "/usr",
    "/usr/lib",
    "/usr/lib/firmware",
    "/usr/lib/firmware/nvidia_boot_memory_test",
};

static constexpr size_t FIXTURE_DIRECTORY_COUNT = sizeof(FIXTURE_DIRECTORIES) / sizeof(FIXTURE_DIRECTORIES[0]);

static bool g_created_directories[FIXTURE_DIRECTORY_COUNT];

// The two files the copies are read from, by their names for `fw::file` and by their paths for `fs::`
static constexpr const char BOOTLOADER_NAME[] = "nvidia_boot_memory_test/bootloader.bin";
static constexpr const char BOOTLOADER_PATH[] = "/usr/lib/firmware/nvidia_boot_memory_test/bootloader.bin";
static constexpr const char GSP_IMAGE_NAME[]  = "nvidia_boot_memory_test/gsp.bin";
static constexpr const char GSP_IMAGE_PATH[]  = "/usr/lib/firmware/nvidia_boot_memory_test/gsp.bin";

static int32_t remove_fixtures() {
    fs::unlink(BOOTLOADER_PATH);
    fs::unlink(GSP_IMAGE_PATH);

    for (size_t i = FIXTURE_DIRECTORY_COUNT; i > 0; i--) {
        if (g_created_directories[i - 1]) {
            fs::rmdir(FIXTURE_DIRECTORIES[i - 1]);
            g_created_directories[i - 1] = false;
        }
    }

    return 0;
}

static int32_t create_fixtures() {
    for (size_t i = 0; i < FIXTURE_DIRECTORY_COUNT; i++) {
        int32_t rc = fs::mkdir(FIXTURE_DIRECTORIES[i], fs::DEFAULT_DIRECTORY_MODE);
        if (rc != fs::OK && rc != fs::ERR_EXIST) {
            remove_fixtures();
            return rc;
        }

        g_created_directories[i] = rc == fs::OK;
    }

    return 0;
}

BEFORE_ALL(nvidia_gsp_boot_memory, create_fixtures);
AFTER_ALL(nvidia_gsp_boot_memory, remove_fixtures);

TEST(nvidia_gsp_boot_memory, radix3_table_takes_a_page_per_512_entries_of_each_level) {
    // gsp_ga10x.bin of 535.183.01 has a .fwimage section of 0x2460000 bytes, 9312 pages under 19 pages of level 2
    uint64_t ga10x_firmware_pages = 0x2460000 / PAGE_BYTES;

    EXPECT_EQ(radix3_table_pages(0), static_cast<uint64_t>(0));
    EXPECT_EQ(radix3_table_pages(1), static_cast<uint64_t>(1 + 1 + 1));
    EXPECT_EQ(radix3_table_pages(PAGE_ENTRIES), static_cast<uint64_t>(1 + 1 + 1));
    EXPECT_EQ(radix3_table_pages(PAGE_ENTRIES + 1), static_cast<uint64_t>(1 + 1 + 2));
    EXPECT_EQ(radix3_table_pages(ga10x_firmware_pages), static_cast<uint64_t>(1 + 1 + 19));
    EXPECT_EQ(radix3_table_pages(PAGE_ENTRIES * PAGE_ENTRIES), 1 + 1 + PAGE_ENTRIES);
    EXPECT_EQ(radix3_table_pages(PAGE_ENTRIES * PAGE_ENTRIES + 1), 1 + 2 + (PAGE_ENTRIES + 1));
    EXPECT_EQ(radix3_table_pages(MAX_DATA_PAGES), 1 + PAGE_ENTRIES + PAGE_ENTRIES * PAGE_ENTRIES);
    EXPECT_EQ(radix3_table_pages(MAX_DATA_PAGES + 1), static_cast<uint64_t>(0));
    EXPECT_EQ(radix3_table_pages(PAST_32_BITS_PAGES), static_cast<uint64_t>(0));
}

// Where the card reaches the tables the tests write, above 32 bits
static constexpr uint64_t TABLE_ADDRESS = 0x7654321000;

// Three runs out of address order, two of them above 32 bits, 530 pages together, so level 2 fills one page and
// 18 entries of a second
static constexpr uint64_t SPLIT_DATA_PAGES  = 3 + 510 + 17;
static constexpr uint64_t SPLIT_TABLE_PAGES = 1 + 1 + 2;

TEST(nvidia_gsp_boot_memory, radix3_table_lists_each_level_then_every_page_of_the_runs_in_order) {
    dma::buffer runs[] = {
        {.virt = 0, .phys = 0x12345670000, .size = 3 * PAGE_BYTES, .cached = false},
        {.virt = 0, .phys = 0x9876540000, .size = 510 * PAGE_BYTES, .cached = false},
        {.virt = 0, .phys = 0x10000, .size = 17 * PAGE_BYTES, .cached = false},
    };

    uint64_t* table = static_cast<uint64_t*>(heap::uzalloc(SPLIT_TABLE_PAGES * PAGE_BYTES));
    ASSERT_NOT_NULL(table);

    write_radix3_table(table, TABLE_ADDRESS, runs, 3);

    uint64_t* level_1 = table + PAGE_ENTRIES;
    uint64_t* level_2 = level_1 + PAGE_ENTRIES;

    EXPECT_EQ(table[0], TABLE_ADDRESS + PAGE_BYTES);
    EXPECT_EQ(table[1], static_cast<uint64_t>(0));
    EXPECT_EQ(level_1[0], TABLE_ADDRESS + 2 * PAGE_BYTES);
    EXPECT_EQ(level_1[1], TABLE_ADDRESS + 3 * PAGE_BYTES);
    EXPECT_EQ(level_1[2], static_cast<uint64_t>(0));

    uint64_t wrong_entries = 0;
    uint64_t entry = 0;
    for (const dma::buffer& run : runs) {
        for (uint64_t page = 0; page < run.size / PAGE_BYTES; page++) {
            wrong_entries += level_2[entry] != run.phys + page * PAGE_BYTES;
            entry++;
        }
    }

    EXPECT_EQ(entry, SPLIT_DATA_PAGES);
    EXPECT_EQ(wrong_entries, static_cast<uint64_t>(0));
    EXPECT_EQ(level_2[SPLIT_DATA_PAGES], static_cast<uint64_t>(0));

    heap::ufree(table);
}

// One data page more than 512 pages of level 2 map, so level 2 takes 513 pages and level 1 two
static constexpr uint64_t SPANNING_DATA_PAGES  = PAGE_ENTRIES * PAGE_ENTRIES + 1;
static constexpr uint64_t SPANNING_TABLE_PAGES = 1 + 2 + (PAGE_ENTRIES + 1);

TEST(nvidia_gsp_boot_memory, radix3_level_1_continues_on_its_second_page_past_512_level_2_pages) {
    dma::buffer run = {.virt = 0, .phys = 0x400000000, .size = SPANNING_DATA_PAGES * PAGE_BYTES, .cached = false};

    uint64_t* table = static_cast<uint64_t*>(heap::uzalloc(SPANNING_TABLE_PAGES * PAGE_BYTES));
    ASSERT_NOT_NULL(table);

    write_radix3_table(table, TABLE_ADDRESS, &run, 1);

    uint64_t* level_1 = table + PAGE_ENTRIES;
    uint64_t* level_2 = level_1 + 2 * PAGE_ENTRIES;
    uint64_t level_2_address = TABLE_ADDRESS + 3 * PAGE_BYTES;

    EXPECT_EQ(table[0], TABLE_ADDRESS + PAGE_BYTES);
    EXPECT_EQ(table[1], TABLE_ADDRESS + 2 * PAGE_BYTES);
    EXPECT_EQ(table[2], static_cast<uint64_t>(0));
    EXPECT_EQ(level_1[PAGE_ENTRIES - 1], level_2_address + (PAGE_ENTRIES - 1) * PAGE_BYTES);
    EXPECT_EQ(level_1[PAGE_ENTRIES], level_2_address + PAGE_ENTRIES * PAGE_BYTES);
    EXPECT_EQ(level_1[PAGE_ENTRIES + 1], static_cast<uint64_t>(0));
    EXPECT_EQ(level_2[0], run.phys);
    EXPECT_EQ(level_2[SPANNING_DATA_PAGES - 1], run.phys + (SPANNING_DATA_PAGES - 1) * PAGE_BYTES);
    EXPECT_EQ(level_2[SPANNING_DATA_PAGES], static_cast<uint64_t>(0));

    heap::ufree(table);
}

// Each copied range lies at an offset and has a size above 0xFFFF, ending inside a page, so a cut to 16 bits or a
// copy of whole pages fails. The firmware takes two runs, the second of 18 pages, its last page partly filled.
static constexpr uint64_t BOOTLOADER_PROGRAM_OFFSET = 0x10050;
static constexpr uint32_t BOOTLOADER_PROGRAM_BYTES  = 0x10F21;
static constexpr uint64_t BOOTLOADER_FILE_BYTES     = 0x21000;
static constexpr uint64_t SIGNATURE_FILE_OFFSET     = 0x10123;
static constexpr uint64_t COPIED_SIGNATURE_BYTES    = 0x10ABC;
static constexpr uint64_t FIRMWARE_FILE_OFFSET      = 0x21340;
static constexpr uint64_t COPIED_FIRMWARE_BYTES     = 0x211234;
static constexpr uint64_t COPIED_FIRMWARE_PAGES     = 512 + 18;
static constexpr uint64_t GSP_IMAGE_FILE_BYTES      = 0x233000;

// Different salts make the two files differ at most offsets, so a copy from the wrong file fails a test
static constexpr uint32_t BOOTLOADER_SALT = 0x5A17C3E9;
static constexpr uint32_t GSP_IMAGE_SALT  = 0x0B7E4D21;

// The byte at `offset` in a fixture, mixed from the offset's low 32 bits and the file's salt, so a range read from
// another offset in the file, such as one 64 KiB or 2 MiB away, differs in most of its bytes
static uint8_t fixture_byte(uint64_t offset, uint32_t salt) {
    uint32_t mixed = (static_cast<uint32_t>(offset) ^ salt) * 0x9E3779B1;
    mixed ^= mixed >> 15;
    mixed *= 0x85EBCA77;
    mixed ^= mixed >> 13;

    return static_cast<uint8_t>(mixed);
}

// Writes `size` fixture bytes salted with `salt` at `path`, truncating any file there rather than replacing it, so
// a handle already open on it reads only the new bytes
static bool write_fixture(const char* path, uint64_t size, uint32_t salt) {
    uint8_t* bytes = static_cast<uint8_t*>(heap::ualloc(size));
    if (!bytes) {
        return false;
    }

    for (uint64_t i = 0; i < size; i++) {
        bytes[i] = fixture_byte(i, salt);
    }

    fs::file* f = fs::open(path, fs::O_CREAT | fs::O_TRUNC | fs::O_WRONLY);
    ssize_t written = f ? fs::write(f, bytes, size) : -1;
    if (f) {
        fs::close(f);
    }

    heap::ufree(bytes);

    return written == static_cast<ssize_t>(size);
}

// Writes the fixture at `path` and opens it at `name` into `file_out`
static bool install(const char* path, const char* name, uint64_t size, uint32_t salt, fw::file& file_out) {
    return write_fixture(path, size, salt) && file_out.open(name) == fw::OK;
}

static bool install_boot_files(firmware_files& files) {
    bool has_bootloader = install(
        BOOTLOADER_PATH,
        BOOTLOADER_NAME,
        BOOTLOADER_FILE_BYTES,
        BOOTLOADER_SALT,
        files.bootloader
    );
    bool has_gsp_image = install(GSP_IMAGE_PATH, GSP_IMAGE_NAME, GSP_IMAGE_FILE_BYTES, GSP_IMAGE_SALT, files.gsp_image);

    return has_bootloader && has_gsp_image;
}

static constexpr boot_file_header BOOTLOADER_HEADER = {
    .magic             = 0x10DE,
    .header_version    = 1,
    .rounded_size      = static_cast<uint32_t>(BOOTLOADER_FILE_BYTES),
    .descriptor_offset = 24,
    .program_offset    = static_cast<uint32_t>(BOOTLOADER_PROGRAM_OFFSET),
    .program_size      = BOOTLOADER_PROGRAM_BYTES,
};

static constexpr gsp_image_layout GSP_IMAGE = {
    .firmware_file_offset  = FIRMWARE_FILE_OFFSET,
    .firmware_size         = COPIED_FIRMWARE_BYTES,
    .signature_file_offset = SIGNATURE_FILE_OFFSET,
    .signature_size        = COPIED_SIGNATURE_BYTES,
};

// The tests read copies through the kernel's cached mapping of all physical memory, which other mappings must
// match, so the card is coherent
static dma::device_profile coherent_card() {
    return dma::device_profile_test_access::build(64, true);
}

// How many of the copy's `bytes` bytes differ from the fixture's at `file_offset`, plus how many bytes of the rest
// of its last page are not zero
static uint64_t count_wrong_bytes(const uint8_t* copy, uint64_t file_offset, uint64_t bytes, uint32_t salt) {
    uint64_t wrong = 0;
    for (uint64_t i = 0; i < bytes; i++) {
        wrong += copy[i] != fixture_byte(file_offset + i, salt);
    }

    uint64_t padded_bytes = (bytes + PAGE_BYTES - 1) / PAGE_BYTES * PAGE_BYTES;
    for (uint64_t i = bytes; i < padded_bytes; i++) {
        wrong += copy[i] != 0;
    }

    return wrong;
}

// The table page at `address`, read through the kernel's mapping of all physical memory
static const uint64_t* table_page(uint64_t address) {
    return static_cast<const uint64_t*>(paging::phys_to_virt(address));
}

TEST(nvidia_gsp_boot_memory, boot_file_parts_are_copied_whole_into_pages_ending_in_zeros) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    gsp_boot_memory memory;
    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), OK);

    const uint8_t* bootloader = static_cast<const uint8_t*>(paging::phys_to_virt(memory.bootloader_address()));
    const uint8_t* signature = static_cast<const uint8_t*>(paging::phys_to_virt(memory.signature_address()));
    uint64_t wrong_bootloader_bytes = count_wrong_bytes(
        bootloader,
        BOOTLOADER_PROGRAM_OFFSET,
        BOOTLOADER_PROGRAM_BYTES,
        BOOTLOADER_SALT
    );

    uint64_t wrong_signature_bytes = count_wrong_bytes(
        signature,
        SIGNATURE_FILE_OFFSET,
        COPIED_SIGNATURE_BYTES,
        GSP_IMAGE_SALT
    );

    EXPECT_EQ(wrong_bootloader_bytes, static_cast<uint64_t>(0));
    EXPECT_EQ(wrong_signature_bytes, static_cast<uint64_t>(0));

    // The firmware is reached as the card reaches it, through the table's levels
    const uint64_t* level_0 = table_page(memory.radix3_address());
    const uint64_t* level_1 = table_page(level_0[0]);
    const uint64_t* level_2[] = {table_page(level_1[0]), table_page(level_1[1])};

    EXPECT_EQ(level_0[1], static_cast<uint64_t>(0));
    EXPECT_EQ(level_1[2], static_cast<uint64_t>(0));
    EXPECT_EQ(level_2[1][COPIED_FIRMWARE_PAGES - PAGE_ENTRIES], static_cast<uint64_t>(0));

    uint64_t wrong_firmware_bytes = 0;
    for (uint64_t page = 0; page < COPIED_FIRMWARE_PAGES; page++) {
        uint64_t copied_bytes = page * PAGE_BYTES;
        uint64_t left_bytes = COPIED_FIRMWARE_BYTES - copied_bytes;
        uint64_t page_bytes = left_bytes < PAGE_BYTES ? left_bytes : PAGE_BYTES;
        uint64_t file_offset = FIRMWARE_FILE_OFFSET + copied_bytes;
        uint64_t page_address = level_2[page / PAGE_ENTRIES][page % PAGE_ENTRIES];
        const uint8_t* copy = static_cast<const uint8_t*>(paging::phys_to_virt(page_address));

        wrong_firmware_bytes += count_wrong_bytes(copy, file_offset, page_bytes, GSP_IMAGE_SALT);
    }

    EXPECT_EQ(wrong_firmware_bytes, static_cast<uint64_t>(0));

    memory.release();

    EXPECT_EQ(memory.radix3_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.bootloader_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.signature_address(), static_cast<uint64_t>(0));
}

static uint64_t drained_free_pages() {
    page_quarantine::drain();
    return pmm::free_page_count();
}

TEST(nvidia_gsp_boot_memory, bootloader_program_of_no_byte_is_refused_leaving_free_memory_unchanged) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    boot_file_header empty_program = BOOTLOADER_HEADER;
    empty_program.program_size = 0;

    uint64_t free_pages_before = drained_free_pages();
    gsp_boot_memory memory;

    EXPECT_EQ(memory.copy_boot_files(coherent_card(), files, empty_program, GSP_IMAGE), ERR_BAD_FIRMWARE);
    EXPECT_EQ(drained_free_pages(), free_pages_before);
    EXPECT_EQ(memory.bootloader_address(), static_cast<uint64_t>(0));
}

TEST(nvidia_gsp_boot_memory, firmware_of_more_pages_than_a_radix3_table_maps_is_refused_leaving_free_memory_unchanged) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    gsp_image_layout unmappable = GSP_IMAGE;
    unmappable.firmware_size = MAX_DATA_PAGES * PAGE_BYTES + 1;

    uint64_t free_pages_before = drained_free_pages();
    gsp_boot_memory memory;

    int32_t rc = memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, unmappable);

    EXPECT_EQ(rc, ERR_BAD_FIRMWARE);
    EXPECT_EQ(drained_free_pages(), free_pages_before);
    EXPECT_EQ(memory.radix3_address(), static_cast<uint64_t>(0));

    unmappable.firmware_size = PAST_32_BITS_PAGES * PAGE_BYTES;
    EXPECT_EQ(check_boot_copies(BOOTLOADER_HEADER, unmappable), ERR_BAD_FIRMWARE);

    // These differ by one byte, so a page count rounding a partial last page down or a whole one up fails one of them
    gsp_image_layout largest_mappable = GSP_IMAGE;
    largest_mappable.firmware_size = MAX_DATA_PAGES * PAGE_BYTES;
    gsp_image_layout one_byte_more = largest_mappable;
    one_byte_more.firmware_size++;

    EXPECT_EQ(check_boot_copies(BOOTLOADER_HEADER, largest_mappable), OK);
    EXPECT_EQ(check_boot_copies(BOOTLOADER_HEADER, one_byte_more), ERR_BAD_FIRMWARE);
}

TEST(nvidia_gsp_boot_memory, copy_into_an_object_holding_memory_is_refused_keeping_that_memory) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    gsp_boot_memory memory;
    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), OK);
    uint64_t radix3_address = memory.radix3_address();
    uint64_t bootloader_address = memory.bootloader_address();
    uint64_t signature_address = memory.signature_address();

    EXPECT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), ERR_BAD_STATE);
    EXPECT_EQ(memory.radix3_address(), radix3_address);
    EXPECT_EQ(memory.bootloader_address(), bootloader_address);
    EXPECT_EQ(memory.signature_address(), signature_address);

    memory.release();
}

// A limit below 32 bits is one the DMA layer refuses, so the first allocation fails
static constexpr uint8_t REFUSED_ADDRESS_BITS = 31;

TEST(nvidia_gsp_boot_memory, failed_allocation_returns_no_memory_and_takes_no_page) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    dma::device_profile refused_card = dma::device_profile_test_access::build(REFUSED_ADDRESS_BITS, true);
    uint64_t free_pages_before = drained_free_pages();
    gsp_boot_memory memory;

    EXPECT_EQ(memory.copy_boot_files(refused_card, files, BOOTLOADER_HEADER, GSP_IMAGE), ERR_NO_MEMORY);
    EXPECT_EQ(drained_free_pages(), free_pages_before);
    EXPECT_EQ(memory.bootloader_address(), static_cast<uint64_t>(0));
}

// Pages the kernel may keep for page tables that mapped the memory, fewer than the smallest part, the table's 4
static constexpr uint64_t KERNEL_PAGE_TABLE_PAGES = 3;

// Rewrites the file at `path` to its first `cut_bytes` bytes, shrinking the file `files` holds open, so a read past
// the cut fails, then expects the copy to fail with `ERR_BAD_FIRMWARE`, giving back every page it took
static void expect_cut_file_to_free_every_part(const char* path, uint32_t salt, uint64_t cut_bytes) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    // A first copy maps memory the way the failing one does, so its page tables already exist
    gsp_boot_memory memory;
    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), OK);
    memory.release();

    ASSERT_TRUE(write_fixture(path, cut_bytes, salt));

    uint64_t free_pages_before = drained_free_pages();

    EXPECT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), ERR_BAD_FIRMWARE);
    EXPECT_GE(drained_free_pages() + KERNEL_PAGE_TABLE_PAGES, free_pages_before);
    EXPECT_EQ(memory.radix3_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.bootloader_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.signature_address(), static_cast<uint64_t>(0));
}

TEST(nvidia_gsp_boot_memory, failed_read_of_the_bootloader_program_frees_every_part_allocated) {
    uint64_t cut_bytes = BOOTLOADER_PROGRAM_OFFSET + 0x100;
    expect_cut_file_to_free_every_part(BOOTLOADER_PATH, BOOTLOADER_SALT, cut_bytes);
}

TEST(nvidia_gsp_boot_memory, failed_read_of_the_signature_frees_every_part_allocated) {
    uint64_t cut_bytes = SIGNATURE_FILE_OFFSET + 0x100;
    expect_cut_file_to_free_every_part(GSP_IMAGE_PATH, GSP_IMAGE_SALT, cut_bytes);
}

// A cut inside the first run fails its read before the second run is allocated, so `release` meets an empty entry
// in the list of runs
TEST(nvidia_gsp_boot_memory, failed_read_of_the_first_firmware_run_frees_every_part_allocated) {
    uint64_t cut_bytes = FIRMWARE_FILE_OFFSET + 0x100;
    expect_cut_file_to_free_every_part(GSP_IMAGE_PATH, GSP_IMAGE_SALT, cut_bytes);
}

// The driver copies the firmware in runs of 2 MiB, so a GSP image cut just past the first run fails the second
// run's read after every part, the second run included, was allocated
static constexpr uint64_t FIRST_RUN_BYTES = 0x200000;

TEST(nvidia_gsp_boot_memory, failed_read_of_the_second_firmware_run_frees_every_part_allocated) {
    uint64_t cut_bytes = FIRMWARE_FILE_OFFSET + FIRST_RUN_BYTES + 0x100;
    expect_cut_file_to_free_every_part(GSP_IMAGE_PATH, GSP_IMAGE_SALT, cut_bytes);
}

// Firmware of the most pages a table maps needs a table of 1 + 512 + 512 x 512 pages, more than a single DMA
// allocation holds, so the table's allocation fails after the bootloader's program and the signature were copied
TEST(nvidia_gsp_boot_memory, failed_allocation_of_the_radix3_table_frees_every_part_allocated) {
    firmware_files files;
    ASSERT_TRUE(install_boot_files(files));

    gsp_image_layout largest_mappable = GSP_IMAGE;
    largest_mappable.firmware_size = MAX_DATA_PAGES * PAGE_BYTES;

    // A first copy maps memory the way the failing one does, so its page tables already exist
    gsp_boot_memory memory;
    ASSERT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, GSP_IMAGE), OK);
    memory.release();

    uint64_t free_pages_before = drained_free_pages();

    EXPECT_EQ(memory.copy_boot_files(coherent_card(), files, BOOTLOADER_HEADER, largest_mappable), ERR_NO_MEMORY);
    EXPECT_GE(drained_free_pages() + KERNEL_PAGE_TABLE_PAGES, free_pages_before);
    EXPECT_EQ(memory.radix3_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.bootloader_address(), static_cast<uint64_t>(0));
    EXPECT_EQ(memory.signature_address(), static_cast<uint64_t>(0));
}
