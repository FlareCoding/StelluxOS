#include "cpu/features.h"
#include "common/types.h"

namespace cpu {

__PRIVILEGED_DATA features g_features = {};

// Read MIDR_EL1: Main ID Register
__PRIVILEGED_CODE static inline uint64_t read_midr_el1() {
    uint64_t val;
    asm volatile("mrs %0, midr_el1" : "=r"(val));
    return val;
}

// Read ID_AA64PFR0_EL1: Processor Feature Register 0
__PRIVILEGED_CODE static inline uint64_t read_id_aa64pfr0_el1() {
    uint64_t val;
    asm volatile("mrs %0, id_aa64pfr0_el1" : "=r"(val));
    return val;
}

// Read ID_AA64ISAR0_EL1: Instruction Set Attribute Register 0
__PRIVILEGED_CODE static inline uint64_t read_id_aa64isar0_el1() {
    uint64_t val;
    asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(val));
    return val;
}

// Read ID_AA64PFR1_EL1: Processor Feature Register 1
__PRIVILEGED_CODE static inline uint64_t read_id_aa64pfr1_el1() {
    uint64_t val;
    asm volatile("mrs %0, id_aa64pfr1_el1" : "=r"(val));
    return val;
}

// Detect CPU features via ID registers and populate g_features
__PRIVILEGED_CODE static void detect() {
    g_features.flags = 0;

    // MIDR_EL1: CPU identification
    uint64_t midr = read_midr_el1();
    g_features.revision    = static_cast<uint8_t>(midr & 0x0F);
    g_features.part_num    = static_cast<uint16_t>((midr >> 4) & 0x0FFF);
    g_features.variant     = static_cast<uint8_t>((midr >> 20) & 0x0F);
    g_features.implementer = static_cast<uint8_t>((midr >> 24) & 0xFF);

    // ID_AA64PFR0_EL1: Processor features
    uint64_t pfr0 = read_id_aa64pfr0_el1();

    // FP field [19:16]: 0b0000 = FP implemented, 0b1111 = not implemented
    uint8_t fp_field = (pfr0 >> 16) & 0x0F;
    if (fp_field != 0x0F) {
        g_features.flags |= FP;
    }

    // AdvSIMD field [23:20]: 0b0000 = ASIMD implemented, 0b1111 = not implemented
    uint8_t asimd_field = (pfr0 >> 20) & 0x0F;
    if (asimd_field != 0x0F) {
        g_features.flags |= ASIMD;
    }

    // SVE field [35:32]: 0b0000 = not implemented, 0b0001 = SVE implemented
    uint8_t sve_field = (pfr0 >> 32) & 0x0F;
    if (sve_field >= 1) {
        g_features.flags |= SVE;
    }

    // ID_AA64ISAR0_EL1: Instruction set attributes
    uint64_t isar0 = read_id_aa64isar0_el1();

    // AES field [7:4]: 0b0001 = AES, 0b0010 = AES+PMULL
    uint8_t aes_field = (isar0 >> 4) & 0x0F;
    if (aes_field >= 1) {
        g_features.flags |= AES;
    }
    if (aes_field >= 2) {
        g_features.flags |= PMULL;
    }

    // SHA1 field [11:8]
    uint8_t sha1_field = (isar0 >> 8) & 0x0F;
    if (sha1_field >= 1) {
        g_features.flags |= SHA1;
    }

    // SHA2 field [15:12]
    uint8_t sha2_field = (isar0 >> 12) & 0x0F;
    if (sha2_field >= 1) {
        g_features.flags |= SHA256;
    }

    // CRC32 field [19:16]
    uint8_t crc32_field = (isar0 >> 16) & 0x0F;
    if (crc32_field >= 1) {
        g_features.flags |= CRC32;
    }

    // Atomic field [23:20]: LSE atomics
    uint8_t atomic_field = (isar0 >> 20) & 0x0F;
    if (atomic_field >= 2) {
        g_features.flags |= ATOMICS;
    }

    // RNDR field [63:60]: Random number
    uint8_t rndr_field = (isar0 >> 60) & 0x0F;
    if (rndr_field >= 1) {
        g_features.flags |= RNG;
    }

    // ID_AA64PFR1_EL1 for BTI and MTE
    uint64_t pfr1 = read_id_aa64pfr1_el1();

    // BT field [3:0]: Branch Target Identification
    // 0b0000 = not implemented, 0b0001 = implemented
    uint8_t bti_field = pfr1 & 0x0F;
    if (bti_field >= 1) {
        g_features.flags |= BTI;
    }

    // MTE field [11:8]: Memory Tagging Extension
    // 0b0000 = not implemented, 0b0001 = instruction-only, 0b0010 = full MTE
    uint8_t mte_field = (pfr1 >> 8) & 0x0F;
    if (mte_field >= 1) {
        g_features.flags |= MTE;
    }
}

__PRIVILEGED_CODE int32_t init() {
    detect();
    return OK;
}

} // namespace cpu
