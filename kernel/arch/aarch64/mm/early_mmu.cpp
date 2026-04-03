#include "mm/early_mmu.h"
#include "boot/boot_services.h"
#include "hw/barrier.h"
#include "common/types.h"

namespace early_mmu {

// Page table descriptor bits
constexpr uint64_t DESC_VALID = 1ULL << 0;
constexpr uint64_t DESC_TABLE = 1ULL << 1;   // Table descriptor (points to next level)
constexpr uint64_t DESC_BLOCK = 0ULL << 1;   // Block descriptor (2MB/1GB mapping)
constexpr uint64_t DESC_AF = 1ULL << 10;     // Access flag (must be set)
constexpr uint64_t DESC_SH_OSH = 2ULL << 8;  // Outer Shareable
constexpr uint64_t DESC_AP_RW = 0ULL << 6;   // EL1 R/W, EL0 no access
constexpr uint64_t DESC_UXN = 1ULL << 54;    // User execute never
constexpr uint64_t DESC_PXN = 1ULL << 53;    // Privileged execute never

// AttrIndx for MAIR - Limine uses index 0 for normal memory (0xFF), not device
// We use index 3 for device memory to avoid conflicts
// Note: This early_mmu code uses identity mapping in TTBR0, so it's separate
// from Limine's TTBR1 mappings. We can use index 3 for device here.
constexpr uint64_t DESC_ATTR_DEVICE = 3ULL << 2;  // Index 3 = Device-nGnRnE

// Page/block sizes
constexpr uint64_t PAGE_SIZE = 4096;
constexpr uint64_t BLOCK_SIZE_2MB = 2ULL * 1024 * 1024;

// Static page tables in .bss (4KB aligned)
alignas(PAGE_SIZE) __PRIVILEGED_BSS static uint64_t l0_table[512];
alignas(PAGE_SIZE) __PRIVILEGED_BSS static uint64_t l1_table[512];
alignas(PAGE_SIZE) __PRIVILEGED_BSS static uint64_t l2_table[512];

// State
__PRIVILEGED_DATA static bool initialized = false;

// Convert kernel virtual address to physical using boot info
__PRIVILEGED_CODE static uintptr_t virt_to_phys(void* vaddr) {
    return reinterpret_cast<uintptr_t>(vaddr) 
         - g_boot_info.kernel_virt_base 
         + g_boot_info.kernel_phys_base;
}

// Read system register
__PRIVILEGED_CODE static inline uint64_t read_mair_el1() {
    uint64_t val;
    asm volatile("mrs %0, mair_el1" : "=r"(val));
    return val;
}

// Write system register
__PRIVILEGED_CODE static inline void write_ttbr0_el1(uint64_t val) {
    asm volatile("msr ttbr0_el1, %0" :: "r"(val));
}

__PRIVILEGED_CODE static inline void write_mair_el1(uint64_t val) {
    asm volatile("msr mair_el1, %0" :: "r"(val));
}

// TLB invalidation sequence
__PRIVILEGED_CODE static void invalidate_tlb() {
    asm volatile(
        "dsb ishst\n"       // Ensure page table writes are visible
        "tlbi vmalle1\n"    // Invalidate all EL1 TLB entries
        "dsb ish\n"         // Wait for invalidation to complete
        "isb"               // Synchronize instruction stream
        ::: "memory"
    );
}

__PRIVILEGED_CODE int32_t init() {
    if (initialized) {
        return OK;
    }

    // Verify boot info has been initialized
    if (g_boot_info.kernel_virt_base == 0) {
        return ERR_NOT_INITIALIZED;
    }

    uint64_t mair = read_mair_el1();
    uint8_t mair_idx3 = (mair >> 24) & 0xFF;
    constexpr uint8_t DEVICE_nGnRnE = 0x00;
    if (mair_idx3 != DEVICE_nGnRnE) {
        mair = (mair & ~(0xFFULL << 24)) | (DEVICE_nGnRnE << 24);
        write_mair_el1(mair);
        barrier::instruction();
    }

    // Clear page tables (BSS should be zeroed, but be explicit)
    for (int i = 0; i < 512; i++) {
        l0_table[i] = 0;
        l1_table[i] = 0;
        l2_table[i] = 0;
    }

    initialized = true;
    return OK;
}

__PRIVILEGED_CODE uintptr_t map_device(uintptr_t phys_addr, [[maybe_unused]] size_t size) {
    if (!initialized) {
        return 0;
    }

    // We use identity mapping: VA = PA
    // This keeps the address in TTBR0 range (lower half)
    uintptr_t virt_addr = phys_addr;

    // Calculate page table indices for 4KB granule, 48-bit VA
    // VA bits: [47:39] = L0, [38:30] = L1, [29:21] = L2, [20:12] = L3, [11:0] = offset
    uint64_t l0_idx = (virt_addr >> 39) & 0x1FF;
    uint64_t l1_idx = (virt_addr >> 30) & 0x1FF;
    uint64_t l2_idx = (virt_addr >> 21) & 0x1FF;

    // Get physical addresses of our page tables
    uint64_t l0_phys = virt_to_phys(l0_table);
    uint64_t l1_phys = virt_to_phys(l1_table);
    uint64_t l2_phys = virt_to_phys(l2_table);

    // L0 entry -> L1 table
    if ((l0_table[l0_idx] & DESC_VALID) == 0) {
        l0_table[l0_idx] = l1_phys | DESC_VALID | DESC_TABLE;
    }

    // L1 entry -> L2 table
    if ((l1_table[l1_idx] & DESC_VALID) == 0) {
        l1_table[l1_idx] = l2_phys | DESC_VALID | DESC_TABLE;
    }

    // L2 entry -> 2MB block (device memory)
    // Align physical address to 2MB boundary
    uint64_t block_phys = phys_addr & ~(BLOCK_SIZE_2MB - 1);
    
    if ((l2_table[l2_idx] & DESC_VALID) == 0) {
        l2_table[l2_idx] = block_phys 
                         | DESC_VALID 
                         | DESC_BLOCK
                         | DESC_AF 
                         | DESC_ATTR_DEVICE
                         | DESC_SH_OSH
                         | DESC_AP_RW
                         | DESC_UXN 
                         | DESC_PXN;
    }

    // Ensure page table writes are visible before loading TTBR0
    barrier::io_full();

    // Load our page table into TTBR0
    write_ttbr0_el1(l0_phys);

    // Invalidate TLB
    invalidate_tlb();

    // Return the virtual address (same as physical for identity mapping)
    return virt_addr;
}

} // namespace early_mmu
