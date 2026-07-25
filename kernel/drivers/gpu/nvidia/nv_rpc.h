#ifndef STELLUX_DRIVERS_GPU_NVIDIA_NV_RPC_H
#define STELLUX_DRIVERS_GPU_NVIDIA_NV_RPC_H

#include "common/types.h"
#include "drivers/gpu/nvidia/nv_mem.h"
#include "drivers/gpu/nvidia/nv_msgq.h"
#include "drivers/gpu/nvidia/nv_seq.h"

// GSP RPC plane (Stage 8): the shared sysmem command/status rings the GSP-RM
// uses to talk to us, plus the GSP boot args (GSP_ARGUMENTS_CACHED) and libos
// init args (memory regions incl. the logging buffers). Structures are ABI with
// the GSP firmware. Source: message_queue_cpu.c, gsp_init_args.h, libos_init_args.h,
// kgspPopulateGspRmInitArgs / kgspSetupLibosInitArgs (kernel_gsp.c).
namespace nvidia {

constexpr int32_t RPC_OK      = 0;
constexpr int32_t ERR_RPC_MEM = -100;
constexpr int32_t ERR_RPC_MSGQ = -101;
constexpr int32_t ERR_RPC_STATUSQ        = -102; // GSP never brought up its status ring
constexpr int32_t ERR_RPC_INITDONE_TIMEOUT = -103; // never saw GSP_INIT_DONE

// MESSAGE_QUEUE_INIT_ARGUMENTS (gsp_init_args.h), 48 bytes.
struct MessageQueueInitArguments {
    uint64_t sharedMemPhysAddr;
    uint32_t pageTableEntryCount;
    uint32_t _pad0;
    uint64_t cmdQueueOffset;
    uint64_t statQueueOffset;
    uint64_t locklessCmdQueueOffset;
    uint64_t locklessStatQueueOffset;
};

// GSP_SR_INIT_ARGUMENTS (gsp_init_args.h).
struct GspSrInitArguments {
    uint32_t oldLevel;
    uint32_t flags;
    uint8_t  bInPMTransition;
};

// GSP_ARGUMENTS_CACHED (gsp_init_args.h), 80 bytes. Referenced by the libos
// "RMARGS" region; the GSP reads it to find the message queues.
struct GspArgumentsCached {
    MessageQueueInitArguments messageQueueInitArguments;
    GspSrInitArguments        srInitArguments;
    uint32_t                  gpuInstance;
    struct { uint64_t pa; uint64_t size; } profilerArgs;
};
static_assert(sizeof(GspArgumentsCached) == 80, "GSP_ARGUMENTS_CACHED must be 80 bytes");

// LibosMemoryRegionInitArgument (libos_init_args.h), 32 bytes.
struct LibosMemoryRegionInitArgument {
    uint64_t id8;
    uint64_t pa;
    uint64_t size;
    uint8_t  kind;
    uint8_t  loc;
};
static_assert(sizeof(LibosMemoryRegionInitArgument) == 32, "LibosMemoryRegionInitArgument must be 32 bytes");

constexpr uint8_t  LIBOS_MEMORY_REGION_CONTIGUOUS = 1;
constexpr uint8_t  LIBOS_MEMORY_REGION_LOC_SYSMEM = 1;
constexpr uint32_t LIBOS_INIT_ARGUMENTS_SIZE = 0x1000; // 4 KB
constexpr uint32_t GSP_LOG_COUNT = 3;       // LOGINIT, LOGINTR, LOGRM (LOGIDX_SIZE)
constexpr uint32_t GSP_LOG_SIZE  = 0x10000; // 64 KB each (release build)

struct gsp_rpc {
    dma_buffer shared;                    // page table + cmd ring + status ring
    dma_buffer gsp_args;                  // GSP_ARGUMENTS_CACHED
    dma_buffer log_mem[GSP_LOG_COUNT];    // libos per-task log buffers
    dma_buffer libos_args;                // LibosMemoryRegionInitArgument[]
    dma_buffer rx_staging;                // 64KB working copy for received messages
    msgq       cmd_q;                     // our TX (cmd ring) + RX (status ring, linked in Stage 10)
    uint64_t   shared_mem_pa;            // == page table page phys (pt[0])
    uint64_t   cmd_queue_offset;
    uint64_t   stat_queue_offset;
    uint32_t   page_table_entry_count;
    uint32_t   rx_seq_num;               // next expected status-ring sequence number
    uint32_t   tx_seq_num;               // next command-ring sequence number to send
};

/**
 * @brief Allocate + initialize the GSP RPC shared rings, GSP args, log buffers,
 * and libos init args (Stages 8a+8b). Host-side; no GPU writes (the doorbell and
 * MAILBOX programming happen at the Booter/reset stage). Heavily logged.
 * @return RPC_OK on success; negative ERR_RPC_* otherwise.
 */
int32_t gsp_rpc_init(gsp_rpc& out);

/** @brief Release all buffers held by a gsp_rpc. */
void gsp_rpc_free(gsp_rpc& out);

/**
 * @brief Send the GSP_SET_SYSTEM_INFO (fn 72) async init RPC. Must be stuffed
 * into the command ring after reset-into-RISC-V and before the Booter, so the
 * booting GSP-RM consumes it (kgspBootstrapRiscvOSEarly_GA102). @return RPC_OK.
 */
int32_t gsp_rpc_send_set_system_info(gsp_rpc& rpc, uintptr_t bar0_va,
                                     uint64_t bar0_phys, uint64_t bar1_phys,
                                     uint64_t bar3_phys, uint64_t dbdf);

/**
 * @brief Send the SET_REGISTRY (fn 73) async init RPC with an empty packed
 * registry table. Like SET_SYSTEM_INFO, must be sent before the Booter.
 */
int32_t gsp_rpc_send_set_registry(gsp_rpc& rpc, uintptr_t bar0_va);

/**
 * @brief Stage 10: link the GSP's status ring, then drain GSP->host events and
 * service the CPU sequencer until GSP_INIT_DONE (or timeout). This is what
 * brings GSP-RM the rest of the way up. Heavily logged.
 * @return RPC_OK if INIT_DONE was received; negative ERR_RPC_* otherwise.
 */
int32_t gsp_rpc_wait_for_init_done(gsp_rpc& rpc, const gsp_seq_ctx& ctx);

// Key fields extracted from the GSP's GspStaticConfigInfo reply (fn 65).
struct gsp_static_info {
    char     gpu_name[64];          // gpuNameString, e.g. "NVIDIA GeForce RTX 3080"
    uint64_t fb_length;            // framebuffer size in bytes
    uint32_t fbio_mask;
    uint32_t fb_bus_width;
    uint32_t fb_ram_type;
    uint32_t fbp_mask;
    uint32_t l2_cache_size;        // bytes
    uint8_t  poison_fuse_enabled;  // ECC poison fuse
    uint8_t  vbios_valid;
    uint32_t vbios_sub_vendor;
    uint32_t vbios_sub_device;
    uint32_t h_internal_client;    // handles for internal RMAPI control (used later)
    uint32_t h_internal_device;
    uint32_t h_internal_subdevice;
};

/**
 * @brief Stage 11a: SET_GUEST_SYSTEM_INFO (fn 1, sync) -- the RPC version
 * handshake (driver version strings + VGX version). Must precede other control
 * RPCs. @return RPC_OK if the GSP accepted it (rpc_result==0).
 */
int32_t gsp_rpc_set_guest_system_info(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va);

/**
 * @brief Stage 11b: GET_GSP_STATIC_INFO (fn 65, sync) -- fetch the GSP's static
 * GPU config and extract/print the highlights. Also yields the internal RMAPI
 * handles needed for the object tree. @return RPC_OK on success.
 */
int32_t gsp_rpc_get_static_info(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va,
                                gsp_static_info& out);

/**
 * @brief GSP_RM_ALLOC (fn 103): allocate an RM object on GSP-RM. `params` is the
 * class alloc-params struct (or nullptr/paramsSize=0 for NULL-param classes).
 * @return RPC_OK if transport + alloc handler (*out_status) succeeded.
 */
int32_t gsp_rpc_alloc(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va,
                      uint32_t hClient, uint32_t hParent, uint32_t hObject, uint32_t hClass,
                      const void* params, uint32_t paramsSize, uint32_t* out_status);

/**
 * @brief GSP_RM_ALLOC (fn 103) with reply-param readback. Like gsp_rpc_alloc but
 * copies the alloc reply's params back into `params` (rpcRmApiAlloc_GSP echoes the
 * updated alloc-params, e.g. the GSP-assigned VRAM `offset` for NV01_MEMORY_LOCAL_USER).
 */
int32_t gsp_rpc_alloc_inout(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va,
                            uint32_t hClient, uint32_t hParent, uint32_t hObject, uint32_t hClass,
                            void* params, uint32_t paramsSize, uint32_t* out_status);

/**
 * @brief GSP_RM_CONTROL (fn 76): invoke an RM control on an object. `params` is
 * in/out (the reply's params are copied back into it). @return RPC_OK on success.
 */
int32_t gsp_rpc_control(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va,
                        uint32_t hClient, uint32_t hObject, uint32_t cmd,
                        void* params, uint32_t paramsSize, uint32_t* out_status);

} // namespace nvidia

#endif // STELLUX_DRIVERS_GPU_NVIDIA_NV_RPC_H
