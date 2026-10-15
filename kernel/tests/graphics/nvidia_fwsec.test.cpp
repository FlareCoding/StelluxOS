// Holds definitions re-declared from NVIDIA's open GPU kernel modules, under drivers/graphics/nvidia/LICENSE-NVIDIA

#define STLX_TEST_TIER TIER_MM_ALLOC

#include "stlx_unit_test.h"
#include "drivers/graphics/nvidia/fwsec.h"
#include "drivers/graphics/nvidia/nvidia.h"
#include "drivers/graphics/nvidia/registers.h"
#include "drivers/graphics/nvidia/vbios.h"
#include "mm/heap.h"

TEST_SUITE(nvidia_fwsec);

using drivers::nvidia::ERR_BAD_VBIOS;
using drivers::nvidia::fwsec_layout;
using drivers::nvidia::fwsec_signature;
using drivers::nvidia::OK;
using drivers::nvidia::registers;
using drivers::nvidia::select_fwsec_signature;

// The first fuse register recording a program's version and the bytes to the next, restated here so the tests hold
// the choice to NVIDIA's register map rather than to its own constants
static constexpr uint32_t UCODE_VERSION_FUSES       = 0x8241C0;
static constexpr uint32_t UCODE_VERSION_FUSE_STRIDE = 4;

// A zeroed fake of BAR0, all 16 MiB of it
struct fuse_bar0 {
    static constexpr size_t BYTES = 0x1000000;

    uint8_t* bytes = static_cast<uint8_t*>(heap::uzalloc(BYTES));
    registers regs{reinterpret_cast<uintptr_t>(bytes)};

    ~fuse_bar0() { heap::ufree(bytes); }

    void write(uint32_t offset, uint32_t value) { *reinterpret_cast<uint32_t*>(bytes + offset) = value; }
};

// Writes the fuse register of the program with `ucode_id`, which may name a register just outside the 16
static void put_fuses(fuse_bar0& bar0, uint32_t ucode_id, uint32_t fuses) {
    bar0.write(UCODE_VERSION_FUSES + UCODE_VERSION_FUSE_STRIDE * ucode_id - UCODE_VERSION_FUSE_STRIDE, fuses);
}

// FWSEC as `find_fwsec` returns it for the RTX 3080's VBIOS
static constexpr fwsec_layout RTX_3080_FWSEC = {
    .offset = 0x43864,
    .size = 59904,
    .code_size = 57856,
    .data_size = 2048,
    .signature_slot = 0x5A4,
    .signatures_offset = 0x433E4,
    .signature_count = 3,
    .ucode_id = 9,
    .signature_versions = 0x7,
};

// What the RTX 3080's fuse register for FWSEC holds, recording fuse version 2
static constexpr uint32_t RTX_3080_FWSEC_FUSES = 0x3;

static int32_t select_for_fuses(
    fuse_bar0& bar0,
    const fwsec_layout& fwsec,
    uint32_t fuses,
    fwsec_signature& signature
) {
    put_fuses(bar0, fwsec.ucode_id, fuses);
    return select_fwsec_signature(bar0.regs, fwsec, signature);
}

// The fuse registers next to FWSEC's record other versions, so reading the wrong one picks another signature
TEST(nvidia_fwsec, rtx_3080_signature_is_its_third) {
    fuse_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_fuses(bar0, 8, 0x1);
    put_fuses(bar0, 9, RTX_3080_FWSEC_FUSES);
    put_fuses(bar0, 10, 0x0);

    fwsec_signature signature = {};
    EXPECT_EQ(select_fwsec_signature(bar0.regs, RTX_3080_FWSEC, signature), OK);
    EXPECT_EQ(signature.fuse_version, 2u);
    EXPECT_EQ(signature.index, 2u);
}

// With a signature for every version up to 15, the index is the fuse version, and versions from 16 have none
TEST(nvidia_fwsec, fuse_version_is_the_highest_set_bit_plus_one) {
    fuse_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    fwsec_layout every_version = RTX_3080_FWSEC;
    every_version.signature_versions = 0xFFFF;
    every_version.signature_count = 16;

    fwsec_signature signature = {};
    EXPECT_EQ(select_for_fuses(bar0, every_version, 0x0, signature), OK);
    EXPECT_EQ(signature.index, 0u);
    EXPECT_EQ(select_for_fuses(bar0, every_version, 0x1, signature), OK);
    EXPECT_EQ(signature.index, 1u);
    EXPECT_EQ(select_for_fuses(bar0, every_version, 0x5, signature), OK);
    EXPECT_EQ(signature.index, 3u);
    EXPECT_EQ(select_for_fuses(bar0, every_version, 0x7FFF, signature), OK);
    EXPECT_EQ(signature.fuse_version, 15u);
    EXPECT_EQ(signature.index, 15u);

    EXPECT_EQ(select_for_fuses(bar0, every_version, 0x8000, signature), ERR_BAD_VBIOS);
    EXPECT_EQ(select_for_fuses(bar0, every_version, 0x80000000, signature), ERR_BAD_VBIOS);

    // Version 32 has no signature. x86_64 and aarch64 shift a 32-bit value by 32 as by 0, so an unbounded bit test
    // would see version 0's bit, and a count of 2 lets the index 1 through.
    fwsec_layout first_version = RTX_3080_FWSEC;
    first_version.signature_versions = 0x1;
    first_version.signature_count = 2;
    EXPECT_EQ(select_for_fuses(bar0, first_version, 0x80000000, signature), ERR_BAD_VBIOS);
}

// Ucode ids 0 and 17 name no fuse register, so the registers right outside the 16 are never read
TEST(nvidia_fwsec, only_ucode_ids_1_to_16_read_a_fuse_register) {
    fuse_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);
    put_fuses(bar0, 0, 0x3);
    put_fuses(bar0, 1, 0x1);
    put_fuses(bar0, 16, 0x3);
    put_fuses(bar0, 17, 0x1);

    fwsec_layout fwsec = RTX_3080_FWSEC;
    fwsec_signature signature = {};
    fwsec.ucode_id = 0;
    EXPECT_EQ(select_fwsec_signature(bar0.regs, fwsec, signature), OK);
    EXPECT_EQ(signature.fuse_version, 0u);

    fwsec.ucode_id = 1;
    EXPECT_EQ(select_fwsec_signature(bar0.regs, fwsec, signature), OK);
    EXPECT_EQ(signature.fuse_version, 1u);

    fwsec.ucode_id = 16;
    EXPECT_EQ(select_fwsec_signature(bar0.regs, fwsec, signature), OK);
    EXPECT_EQ(signature.fuse_version, 2u);

    fwsec.ucode_id = 17;
    EXPECT_EQ(select_fwsec_signature(bar0.regs, fwsec, signature), OK);
    EXPECT_EQ(signature.fuse_version, 0u);
}

// A signature's place counts only lower versions FWSEC has signatures for, so with 0x8001 version 15 takes the
// second, and signatures for all the other versions do not stand in for the fuse version's
TEST(nvidia_fwsec, signature_versions_must_include_the_fuse_version) {
    fuse_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    fwsec_layout fwsec = RTX_3080_FWSEC;
    fwsec_signature signature = {};
    fwsec.signature_versions = 0x3;
    EXPECT_EQ(select_for_fuses(bar0, fwsec, RTX_3080_FWSEC_FUSES, signature), ERR_BAD_VBIOS);

    fwsec.signature_versions = 0xFFFB;
    EXPECT_EQ(select_for_fuses(bar0, fwsec, RTX_3080_FWSEC_FUSES, signature), ERR_BAD_VBIOS);

    fwsec.signature_versions = 0x4;
    EXPECT_EQ(select_for_fuses(bar0, fwsec, RTX_3080_FWSEC_FUSES, signature), OK);
    EXPECT_EQ(signature.fuse_version, 2u);
    EXPECT_EQ(signature.index, 0u);

    fwsec.signature_versions = 0x8001;
    EXPECT_EQ(select_for_fuses(bar0, fwsec, 0x7FFF, signature), OK);
    EXPECT_EQ(signature.fuse_version, 15u);
    EXPECT_EQ(signature.index, 1u);
}

// The RTX 3080's FWSEC leaves room for 3 signatures before it, so a descriptor sized for them but counting 2 passes
// NVIDIA's bound and fails this one
TEST(nvidia_fwsec, signature_must_be_among_the_signatures) {
    fuse_bar0 bar0;
    ASSERT_NOT_NULL(bar0.bytes);

    fwsec_layout fwsec = RTX_3080_FWSEC;
    fwsec_signature signature = {};
    fwsec.signature_count = 2;
    EXPECT_EQ(select_for_fuses(bar0, fwsec, RTX_3080_FWSEC_FUSES, signature), ERR_BAD_VBIOS);

    fwsec.signature_count = 3;
    EXPECT_EQ(select_for_fuses(bar0, fwsec, RTX_3080_FWSEC_FUSES, signature), OK);
}
