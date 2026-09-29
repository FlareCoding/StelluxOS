#define STLX_TEST_TIER TIER_SCHED

#include "stlx_unit_test.h"
#include "exec/elf.h"
#include "exec/elf64.h"
#include "exec/elf_arch.h"
#include "fs/fs.h"
#include "mm/heap.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "common/string.h"

TEST_SUITE(elf_load);

static constexpr size_t PAGE = pmm::PAGE_SIZE;
static constexpr uint64_t SEG_OFFSET = PAGE;
static constexpr uint64_t SEG_VADDR = 0x400000;
static constexpr uint64_t SEG_FILESZ = PAGE + 100;
static constexpr uint64_t SEG_MEMSZ = 3 * PAGE;
static constexpr uint64_t NEAR_PHDR_OFFSET = sizeof(elf64::Ehdr);
static constexpr uint64_t FAR_PHDR_OFFSET = 128 * 1024; // past any fixed window at the head of a file
static constexpr uint64_t TRUNCATED_BYTES = 50;
static constexpr uint64_t OVERRUN_BYTES = 10;

struct test_image {
    uint8_t* bytes;
    size_t size;
};

static uint8_t pattern_byte(size_t i) {
    return static_cast<uint8_t>(i * 7 + 3);
}

// One segment spanning a full page, a partial page, and a page of zero fill
// past its file bytes, described by a program header at phdr_offset
static test_image build_image(uint64_t phdr_offset) {
    uint64_t seg_end = SEG_OFFSET + SEG_FILESZ;
    uint64_t phdr_end = phdr_offset + sizeof(elf64::Phdr);
    size_t size = seg_end > phdr_end ? seg_end : phdr_end;

    auto* bytes = static_cast<uint8_t*>(heap::uzalloc(size));
    if (!bytes) {
        return { nullptr, 0 };
    }

    auto* ehdr = reinterpret_cast<elf64::Ehdr*>(bytes);
    ehdr->e_ident[0] = elf64::ELFMAG0;
    ehdr->e_ident[1] = elf64::ELFMAG1;
    ehdr->e_ident[2] = elf64::ELFMAG2;
    ehdr->e_ident[3] = elf64::ELFMAG3;
    ehdr->e_ident[elf64::EI_CLASS] = elf64::ELFCLASS64;
    ehdr->e_ident[elf64::EI_DATA] = elf64::ELFDATA2LSB;
    ehdr->e_ident[elf64::EI_VERSION] = elf64::EV_CURRENT;
    ehdr->e_type = elf64::ET_EXEC;
    ehdr->e_machine = exec::ELF_EXPECTED_MACHINE;
    ehdr->e_entry = SEG_VADDR;
    ehdr->e_phoff = phdr_offset;
    ehdr->e_phentsize = sizeof(elf64::Phdr);
    ehdr->e_phnum = 1;

    auto* phdr = reinterpret_cast<elf64::Phdr*>(bytes + phdr_offset);
    phdr->p_type = elf64::PT_LOAD;
    phdr->p_flags = elf64::PF_R;
    phdr->p_offset = SEG_OFFSET;
    phdr->p_vaddr = SEG_VADDR;
    phdr->p_filesz = SEG_FILESZ;
    phdr->p_memsz = SEG_MEMSZ;
    phdr->p_align = PAGE;

    for (size_t i = 0; i < SEG_FILESZ; i++) {
        bytes[SEG_OFFSET + i] = pattern_byte(i);
    }

    return { bytes, size };
}

static bool write_file(const char* path, const uint8_t* bytes, size_t len) {
    fs::file* f = fs::open(path, fs::O_CREAT | fs::O_RDWR | fs::O_TRUNC);
    if (!f) {
        return false;
    }

    bool written = fs::write(f, bytes, len) == static_cast<ssize_t>(len);
    fs::close(f);

    return written;
}

static bool write_image(const char* path, uint64_t phdr_offset, uint64_t trailing_bytes_dropped) {
    test_image image = build_image(phdr_offset);
    if (!image.bytes) {
        return false;
    }

    bool written = write_file(path, image.bytes, image.size - trailing_bytes_dropped);
    heap::ufree(image.bytes);

    return written;
}

static uint8_t* page_bytes(const exec::loaded_image& img, uintptr_t addr) {
    pmm::phys_addr_t phys = paging::get_physical(addr, img.pt_root);
    return phys ? static_cast<uint8_t*>(paging::phys_to_virt(phys)) : nullptr;
}

static bool segment_loaded_with_zero_fill(const exec::loaded_image& img) {
    uint8_t* first = page_bytes(img, SEG_VADDR);
    uint8_t* second = page_bytes(img, SEG_VADDR + PAGE);
    uint8_t* third = page_bytes(img, SEG_VADDR + 2 * PAGE);
    if (!first || !second || !third) {
        return false;
    }

    for (size_t i = 0; i < SEG_FILESZ; i++) {
        uint8_t loaded = i < PAGE ? first[i] : second[i - PAGE];
        if (loaded != pattern_byte(i)) {
            return false;
        }
    }

    for (size_t i = SEG_FILESZ - PAGE; i < PAGE; i++) {
        if (second[i] != 0) {
            return false;
        }
    }

    for (size_t i = 0; i < PAGE; i++) {
        if (third[i] != 0) {
            return false;
        }
    }

    return true;
}

TEST(elf_load, a_file_loads_its_segment_bytes_and_zero_fill) {
    ASSERT_TRUE(write_image("/elf_load_near", NEAR_PHDR_OFFSET, 0));

    exec::loaded_image img;
    ASSERT_EQ(exec::load_elf("/elf_load_near", &img), exec::OK);

    EXPECT_EQ(img.entry_point, SEG_VADDR);
    EXPECT_TRUE(segment_loaded_with_zero_fill(img));

    exec::unload_elf(&img);
    fs::unlink("/elf_load_near");
}

TEST(elf_load, program_headers_far_into_the_file_are_read) {
    ASSERT_TRUE(write_image("/elf_load_far", FAR_PHDR_OFFSET, 0));

    exec::loaded_image img;
    ASSERT_EQ(exec::load_elf("/elf_load_far", &img), exec::OK);

    EXPECT_TRUE(segment_loaded_with_zero_fill(img));

    exec::unload_elf(&img);
    fs::unlink("/elf_load_far");
}

static bool segment_is_zero_filled(const exec::loaded_image& img) {
    for (uint64_t offset = 0; offset < SEG_MEMSZ; offset += PAGE) {
        uint8_t* bytes = page_bytes(img, SEG_VADDR + offset);
        if (!bytes) {
            return false;
        }

        for (size_t i = 0; i < PAGE; i++) {
            if (bytes[i] != 0) {
                return false;
            }
        }
    }

    return true;
}

TEST(elf_load, a_segment_without_file_bytes_loads_as_zero_fill) {
    test_image image = build_image(NEAR_PHDR_OFFSET);
    ASSERT_NOT_NULL(image.bytes);

    // No bytes come from the file, so an offset past its end is still valid
    auto* phdr = reinterpret_cast<elf64::Phdr*>(image.bytes + NEAR_PHDR_OFFSET);
    phdr->p_filesz = 0;
    phdr->p_offset = image.size + PAGE;

    bool written = write_file("/elf_load_bss", image.bytes, image.size);
    heap::ufree(image.bytes);
    ASSERT_TRUE(written);

    exec::loaded_image img;
    ASSERT_EQ(exec::load_elf("/elf_load_bss", &img), exec::OK);

    EXPECT_TRUE(segment_is_zero_filled(img));

    exec::unload_elf(&img);
    fs::unlink("/elf_load_bss");
}

TEST(elf_load, a_file_ending_inside_a_segment_is_refused) {
    ASSERT_TRUE(write_image("/elf_load_short", NEAR_PHDR_OFFSET, TRUNCATED_BYTES));

    exec::loaded_image img;
    EXPECT_EQ(exec::load_elf("/elf_load_short", &img), exec::ERR_INVALID_PHDR);
    EXPECT_NULL(img.mm_ctx);

    fs::unlink("/elf_load_short");
}

TEST(elf_load, a_program_header_table_past_the_end_is_refused) {
    ASSERT_TRUE(write_image("/elf_load_overrun", FAR_PHDR_OFFSET, OVERRUN_BYTES));

    exec::loaded_image img;
    EXPECT_EQ(exec::load_elf("/elf_load_overrun", &img), exec::ERR_INVALID_PHDR);
    EXPECT_NULL(img.mm_ctx);

    fs::unlink("/elf_load_overrun");
}
