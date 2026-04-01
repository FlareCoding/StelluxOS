#ifndef STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_GSP_BOOT_H
#define STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_GSP_BOOT_H

#include "common/types.h"
#include "dma/dma.h"
#include "drivers/graphics/nvidia/nv_types.h"
#include "drivers/graphics/nvidia/nv_firmware.h"
#include "drivers/graphics/nvidia/nv_falcon.h"

namespace nv {

class nv_gpu;

// ============================================================================
// Constants
// ============================================================================

constexpr uint64_t GSP_FW_WPR_META_MAGIC    = 0xdc3aae21371a60b3ULL;
constexpr uint64_t GSP_FW_WPR_META_REVISION = 1;
constexpr uint64_t GSP_FW_WPR_META_VERIFIED = 0xa0a0a0a0a0a0a0a0ULL;

// GSP heap sizing parameters
// LIBOS2 (Turing, GA100): carveout=0, min=64MB, max=256MB
// LIBOS3 (GA102+, baremetal): carveout=22MB, min=88MB, max=280MB
constexpr uint32_t GSP_FW_HEAP_OS_CARVEOUT_LIBOS3 = (22 << 20); // 22 MB
constexpr uint32_t GSP_FW_HEAP_OS_CARVEOUT_LIBOS2 = 0;           // 0 MB
constexpr uint32_t GSP_FW_HEAP_BASE_SIZE        = (8 << 20);  // 8 MB (Turing-Ada)
constexpr uint32_t GSP_FW_HEAP_PER_GB_SIZE      = (96 << 10); // 96 KB per GB VRAM
constexpr uint32_t GSP_FW_HEAP_CLIENT_ALLOC     = ((48 << 10) * 2048); // 96 MB
constexpr uint32_t GSP_FW_HEAP_MIN_LIBOS3       = (88 << 20); // 88 MB (GA102+)
constexpr uint32_t GSP_FW_HEAP_MIN_LIBOS2       = (64 << 20); // 64 MB (Turing)
constexpr uint32_t GSP_FW_HEAP_MAX_LIBOS3       = (280 << 20); // 280 MB (GA102+)
constexpr uint32_t GSP_FW_HEAP_MAX_LIBOS2       = (256 << 20); // 256 MB (Turing)

// Alignment constants
constexpr uint64_t ALIGN_128K = 0x20000;
constexpr uint64_t ALIGN_64K  = 0x10000;
constexpr uint64_t ALIGN_4K   = 0x1000;
constexpr uint64_t ALIGN_1M   = 0x100000;

// GSP page constants (GSP_PAGE_SHIFT and GSP_PAGE_SIZE already in nv_firmware.h)
constexpr uint32_t GSP_PAGE_SIZE_VAL = 0x1000; // 4KB
constexpr uint32_t RADIX3_ENTRIES_PER_PAGE_VAL = GSP_PAGE_SIZE_VAL / sizeof(uint64_t); // 512

// Message queue sizing
constexpr uint32_t CMDQ_SIZE = 0x40000; // 256KB command queue
constexpr uint32_t MSGQ_SIZE = 0x40000; // 256KB status/message queue
constexpr uint32_t QUEUE_ENTRY_SIZE = GSP_PAGE_SIZE_VAL; // 4KB per entry
constexpr uint32_t LOG_BUF_SIZE = 0x10000; // 64KB per log buffer

// RPC constants
constexpr uint32_t RPC_HDR_VERSION = 0x03000000;
constexpr uint32_t RPC_SIGNATURE   = 0x43505256; // 'VRPC' LE
constexpr uint32_t GSP_MSG_HDR_SIZE = 48; // sizeof(gsp_msg_element)

// GSP events
constexpr uint32_t NV_VGPU_MSG_EVENT_GSP_INIT_DONE       = 0x1001;
constexpr uint32_t NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQ     = 0x1002;
constexpr uint32_t NV_VGPU_MSG_EVENT_OS_ERROR_LOG         = 0x1006;

// GSP RPC functions used during boot
constexpr uint32_t NV_VGPU_MSG_FUNCTION_CONTINUATION     = 71;
constexpr uint32_t NV_VGPU_MSG_FUNCTION_SET_SYSTEM_INFO  = 72;
constexpr uint32_t NV_VGPU_MSG_FUNCTION_SET_REGISTRY     = 73;
constexpr uint32_t NV_VGPU_MSG_FUNCTION_GET_STATIC_INFO  = 65;
constexpr uint32_t NV_VGPU_MSG_FUNCTION_RM_ALLOC         = 103;

// LibOS memory region kinds and locations
constexpr uint8_t LIBOS_MEMORY_REGION_CONTIGUOUS = 1;
constexpr uint8_t LIBOS_MEMORY_REGION_RADIX3     = 2;
constexpr uint8_t LIBOS_MEMORY_REGION_LOC_NONE   = 0;
constexpr uint8_t LIBOS_MEMORY_REGION_LOC_SYSMEM = 1;
constexpr uint8_t LIBOS_MEMORY_REGION_LOC_FB     = 2;

// Doorbell register (falcon offset)
constexpr uint32_t FALCON_DOORBELL = 0xC00;

// RISC-V status register (falcon offset + 0x1000 block)
constexpr uint32_t FALCON_RISCV_STATUS = 0x1388;
constexpr uint32_t FALCON_RISCV_ACTIVE = (1 << 7);

// ============================================================================
// On-wire Structures (must be binary-compatible with GSP-RM firmware)
// ============================================================================

// GspFwWprMeta — 256 bytes, passed to SEC2 booter via DMA
struct __attribute__((packed)) gsp_fw_wpr_meta {
    uint64_t magic;                    // 0x00: 0xdc3aae21371a60b3
    uint64_t revision;                 // 0x08: 1
    uint64_t sysmem_addr_radix3_elf;   // 0x10: DMA addr of radix3 level-0
    uint64_t size_of_radix3_elf;       // 0x18: .fwimage size
    uint64_t sysmem_addr_bootloader;   // 0x20: DMA addr of bootloader ucode
    uint64_t size_of_bootloader;       // 0x28: bootloader payload size
    uint64_t bootloader_code_offset;   // 0x30: from RiscvUCodeDesc
    uint64_t bootloader_data_offset;   // 0x38: from RiscvUCodeDesc
    uint64_t bootloader_manifest_offset; // 0x40: from RiscvUCodeDesc
    uint64_t sysmem_addr_signature;    // 0x48: DMA addr of GSP FW sigs
    uint64_t size_of_signature;        // 0x50: signature section size
    uint64_t gsp_fw_rsvd_start;        // 0x58: = non-WPR heap start
    uint64_t non_wpr_heap_offset;      // 0x60
    uint64_t non_wpr_heap_size;        // 0x68: 1MB
    uint64_t gsp_fw_wpr_start;         // 0x70: 1MB-aligned
    uint64_t gsp_fw_heap_offset;       // 0x78
    uint64_t gsp_fw_heap_size;         // 0x80
    uint64_t gsp_fw_offset;            // 0x88: ELF location in FB
    uint64_t boot_bin_offset;          // 0x90: bootloader location in FB
    uint64_t frts_offset;              // 0x98
    uint64_t frts_size;                // 0xA0: 1MB
    uint64_t gsp_fw_wpr_end;           // 0xA8
    uint64_t fb_size;                  // 0xB0: total VRAM
    uint64_t vga_workspace_offset;     // 0xB8
    uint64_t vga_workspace_size;       // 0xC0
    uint64_t boot_count;               // 0xC8: 0
    uint64_t partition_rpc_addr;       // 0xD0: 0
    uint16_t partition_rpc_request_offset; // 0xD8: 0
    uint16_t partition_rpc_reply_offset;   // 0xDA: 0
    uint32_t elf_code_offset;          // 0xDC: 0
    uint32_t elf_data_offset;          // 0xE0: 0
    uint32_t elf_code_size;            // 0xE4: 0
    uint32_t elf_data_size;            // 0xE8: 0
    uint32_t ls_ucode_version;         // 0xEC: 0
    uint8_t  gsp_fw_heap_vf_count;     // 0xF0: 0
    uint8_t  flags;                    // 0xF1: 0
    uint16_t padding;                  // 0xF2: 0
    uint32_t pmu_reserved_size;        // 0xF4: 0
    uint64_t verified;                 // 0xF8: 0 (set to VERIFIED by booter)
};
static_assert(sizeof(gsp_fw_wpr_meta) == 256);

// LibOS memory region init argument — 32 bytes (8-byte aligned)
struct __attribute__((packed)) libos_mem_region_init_arg {
    uint64_t id8;     // ASCII name packed big-endian into u64
    uint64_t pa;      // DMA physical address
    uint64_t size;    // Size in bytes
    uint8_t  kind;    // 0=NONE, 1=CONTIGUOUS, 2=RADIX3
    uint8_t  loc;     // 0=NONE, 1=SYSMEM, 2=FB
    uint8_t  pad[6];  // Padding to 32 bytes
};
static_assert(sizeof(libos_mem_region_init_arg) == 32);

// Message queue TX header — 32 bytes
struct __attribute__((packed)) msgq_tx_header {
    uint32_t version;    // 0
    uint32_t size;       // Total queue size (0x40000)
    uint32_t msg_size;   // Entry size (0x1000)
    uint32_t msg_count;  // Number of entries (63)
    uint32_t write_ptr;  // 0 initially
    uint32_t flags;      // 1 for cmdq, 0 for msgq
    uint32_t rx_hdr_off; // Offset to RX header
    uint32_t entry_off;  // Offset to first entry (0x1000)
};
static_assert(sizeof(msgq_tx_header) == 32);

// Message queue RX header — 4 bytes
struct __attribute__((packed)) msgq_rx_header {
    uint32_t read_ptr;   // 0 initially
};
static_assert(sizeof(msgq_rx_header) == 4);

// Combined queue header (at start of each queue's backing store)
struct __attribute__((packed)) queue_header {
    msgq_tx_header tx;  // 32 bytes at offset 0x00
    msgq_rx_header rx;  // 4 bytes at offset 0x20
};
static_assert(sizeof(queue_header) == 36);

// GSP message element header (transport layer) — 48 bytes
struct __attribute__((packed)) gsp_msg_element {
    uint8_t  auth_tag[16];  // 0x00: zero for plaintext
    uint8_t  aad[16];       // 0x10: zero for plaintext
    uint32_t checksum;      // 0x20: XOR checksum
    uint32_t seq_num;       // 0x24: transport sequence number
    uint32_t elem_count;    // 0x28: number of pages this message spans
    uint32_t pad;           // 0x2C: zero
    // RPC header follows at offset 0x30
};
static_assert(sizeof(gsp_msg_element) == 48);

// RPC message header — 32 bytes
struct __attribute__((packed)) rpc_header {
    uint32_t header_version;     // 0x03000000
    uint32_t signature;          // 0x43505256 ('VRPC')
    uint32_t length;             // sizeof(rpc_header) + payload_size
    uint32_t function;           // RPC function number
    uint32_t rpc_result;         // 0xFFFFFFFF initially
    uint32_t rpc_result_private; // 0xFFFFFFFF initially
    uint32_t sequence;           // RPC sequence number
    uint32_t spare;              // 0
    // Payload follows at offset 0x20
};
static_assert(sizeof(rpc_header) == 32);

// MESSAGE_QUEUE_INIT_ARGUMENTS — passed in GSP_ARGUMENTS_CACHED
// NvLength = NvU64 on 64-bit platforms (including GSP RISC-V)
struct __attribute__((packed)) msg_queue_init_args {
    uint64_t shared_mem_phys_addr;         // 0x00: DMA addr of shared memory
    uint32_t page_table_entry_count;       // 0x08: PTE count
    uint32_t _pad0;                        // 0x0C: alignment padding
    uint64_t cmd_queue_offset;             // 0x10: Byte offset of cmdq in shared mem
    uint64_t stat_queue_offset;            // 0x18: Byte offset of msgq in shared mem
    uint64_t lockless_cmd_queue_offset;    // 0x20: 0 (unused in r535)
    uint64_t lockless_stat_queue_offset;   // 0x28: 0 (unused in r535)
};
static_assert(sizeof(msg_queue_init_args) == 48);

// GSP_SR_INIT_ARGUMENTS
struct __attribute__((packed)) gsp_sr_init_args {
    uint32_t old_level;          // 0 = fresh boot
    uint32_t flags;              // 0
    uint8_t  b_in_pm_transition; // NvBool = NvU8, 0 = not resuming
    uint8_t  _pad[3];            // padding to 12 bytes
};
static_assert(sizeof(gsp_sr_init_args) == 12);

// GSP_ARGUMENTS_CACHED — rmargs content (natural alignment, no packed needed)
struct gsp_arguments_cached {
    msg_queue_init_args mq_init;   // 48 bytes
    gsp_sr_init_args    sr_init;   // 12 bytes
    uint32_t            gpu_instance; // 0
    uint64_t            profiler_pa;  // 0
    uint64_t            profiler_size; // 0
};

// ============================================================================
// Framebuffer Layout
// ============================================================================

struct fb_region {
    uint64_t addr;
    uint64_t size;
};

struct fb_layout {
    uint64_t fb_size;        // Total VRAM size

    fb_region vga_workspace; // VGA workspace at top of FB
    fb_region bios;          // BIOS region (= VGA workspace)

    // WPR2 sub-regions (computed top-down from FRTS)
    fb_region frts;          // FRTS region (1MB, 128KB-aligned below VGA)
    fb_region boot;          // GSP bootloader (4KB-aligned below FRTS)
    fb_region elf;           // GSP FW ELF (64KB-aligned below boot)
    fb_region heap;          // GSP FW heap (1MB-aligned below ELF)
    fb_region wpr2;          // WPR2 region (encompasses heap+elf+boot+frts)

    fb_region non_wpr_heap;  // Non-WPR heap (1MB below WPR2 start)
};

// ============================================================================
// Radix-3 Page Table
// ============================================================================

struct radix3 {
    dma::buffer lvl[3];      // [0]=root, [1]=middle, [2]=leaf
    dma::buffer fw_data_dma; // DMA buffer for firmware data (radix3 entries point into this)
    bool valid;
};

// ============================================================================
// GSP Boot State
// ============================================================================

struct gsp_boot_state {
    // Computed layout
    fb_layout layout;

    // DMA allocations for boot
    dma::buffer wpr_meta_dma;    // GspFwWprMeta (4KB)
    dma::buffer bootloader_dma;  // Bootloader ucode payload
    dma::buffer sig_dma;         // GSP FW signatures (.fwsignature)

    // Radix-3 page table (points to firmware pages)
    radix3 r3;

    // LibOS arguments
    dma::buffer libos_dma;       // 4KB page with LibosMemoryRegionInitArgument[4]

    // Log buffers
    dma::buffer loginit_dma;     // 64KB
    dma::buffer logintr_dma;     // 64KB
    dma::buffer logrm_dma;       // 64KB

    // RM arguments
    dma::buffer rmargs_dma;      // 4KB

    // Shared memory (PTEs + cmdq + msgq)
    dma::buffer shm_dma;
    uint32_t pte_count;
    uint32_t pte_size;
    uint32_t cmdq_offset;
    uint32_t msgq_offset;

    // Message queue state
    uint32_t cmdq_seq;    // Transport sequence counter
    uint32_t rpc_seq;     // RPC sequence counter

    // Boot firmware info (from parsed bootloader)
    uint32_t boot_code_offset;
    uint32_t boot_data_offset;
    uint32_t boot_manifest_offset;
    uint32_t boot_app_version;

    bool initialized;
};

// ============================================================================
// GSP Boot API
// ============================================================================

/**
 * Execute the complete GSP boot sequence:
 * 1. Compute FB layout
 * 2. Allocate DMA buffers (bootloader, sigs, radix3, WPR meta, queues, libos)
 * 3. Build radix-3 page tables for GSP firmware
 * 4. Populate WPR metadata
 * 5. Initialize LibOS arguments and message queues
 * 6. Patch and load booter_load onto SEC2
 * 7. Execute booter → boots GSP-RM
 * 8. Wait for INIT_DONE
 */
int32_t gsp_boot(nv_gpu* gpu, gsp_firmware& fw, gsp_boot_state& state);

/**
 * Free all GSP boot state (DMA buffers, radix3, etc.)
 */
void gsp_boot_free(gsp_boot_state& state);

// Sub-steps (called by gsp_boot, also available individually for testing)
int32_t gsp_compute_fb_layout(nv_gpu* gpu, const gsp_firmware& fw, fb_layout& layout);
int32_t gsp_build_radix3(nv_gpu* gpu, const gsp_fw& fw_data, radix3& r3);
int32_t gsp_init_wpr_meta(nv_gpu* gpu, const gsp_firmware& fw,
                           const fb_layout& layout, gsp_boot_state& state);
int32_t gsp_init_libos(nv_gpu* gpu, gsp_boot_state& state);
int32_t gsp_init_shared_mem(nv_gpu* gpu, gsp_boot_state& state);
int32_t gsp_run_booter(nv_gpu* gpu, gsp_firmware& fw, gsp_boot_state& state);
int32_t gsp_wait_init_done(nv_gpu* gpu, gsp_boot_state& state);

// Helper: encode libos ID8
uint64_t libos_id8(const char* name);

} // namespace nv

#endif // STELLUX_DRIVERS_GRAPHICS_NVIDIA_NV_GSP_BOOT_H
