#include "drivers/gpu/nvidia/nv_rpc.h"
#include "drivers/gpu/nvidia/nv_regs.h"
#include "hw/barrier.h"
#include "hw/mmio.h"
#include "hw/delay.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "common/string.h"
#include "dynpriv/dynpriv.h"

namespace nvidia {

constexpr uint32_t PAGE            = 4096;
constexpr uint32_t CMD_QUEUE_SIZE  = 0x40000; // 256 KB (silicon default)
constexpr uint32_t STAT_QUEUE_SIZE = 0x40000; // 256 KB
constexpr uint32_t RX_STAGING_SIZE = 0x10000; // GSP_MSG_QUEUE_ELEMENT_SIZE_MAX (16 slots)

// GSP event codes (rpc_global_enums.h) + RPC framing constants.
constexpr uint32_t EVENT_GSP_INIT_DONE         = 0x1001;
constexpr uint32_t EVENT_GSP_RUN_CPU_SEQUENCER = 0x1002;

// Host->GSP init RPC function codes (rpc_global_enums.h: 72/73).
constexpr uint32_t FUNC_GSP_SET_SYSTEM_INFO   = 72;
constexpr uint32_t FUNC_SET_REGISTRY          = 73;
// Post-INIT_DONE sync control RPCs.
constexpr uint32_t FUNC_SET_GUEST_SYSTEM_INFO = 1;
constexpr uint32_t FUNC_GET_GSP_STATIC_INFO   = 65;
constexpr uint32_t FUNC_GSP_RM_CONTROL        = 76;
constexpr uint32_t FUNC_GSP_RM_ALLOC          = 103;

// SET_GUEST_SYSTEM_INFO version handshake constants (535.183.01 open driver:
// vgpu_version.h, nvBldVer.h, nvUnixVersion.h).
constexpr uint32_t VGX_MAJOR_VERSION = 0x23;
constexpr uint32_t VGX_MINOR_VERSION = 0x05;
constexpr uint32_t NV_BUILD_CL_NUM   = 34280977;
constexpr uint32_t VGX_INFO_BUF_SIZE = 256; // NV0000_CTRL_CMD_SYSTEM_GET_VGX_SYSTEM_INFO_BUFFER_SIZE

// GET_GSP_STATIC_INFO: GspStaticConfigInfo is 2168 bytes (golden trace fn=65
// len=2200 = 32 hdr + 2168). Field offsets within the struct were validated by
// an offsetof probe against the 535.183.01 SDK headers (sizeof==2168).
constexpr uint32_t SCI_SIZE                 = 2168;
constexpr uint32_t SCI_OFF_POISON_FUSE      = 1744;
constexpr uint32_t SCI_OFF_FB_LENGTH        = 1752; // NvU64
constexpr uint32_t SCI_OFF_FBIO_MASK        = 1760;
constexpr uint32_t SCI_OFF_FB_BUS_WIDTH     = 1764;
constexpr uint32_t SCI_OFF_FB_RAM_TYPE      = 1768;
constexpr uint32_t SCI_OFF_L2_CACHE_SIZE    = 1776;
constexpr uint32_t SCI_OFF_GPU_NAME         = 1820; // char[64]
constexpr uint32_t SCI_OFF_VBIOS_VALID      = 2104;
constexpr uint32_t SCI_OFF_VBIOS_SUBVENDOR  = 2108;
constexpr uint32_t SCI_OFF_VBIOS_SUBDEVICE  = 2112;
constexpr uint32_t SCI_OFF_H_CLIENT         = 2152;
constexpr uint32_t SCI_OFF_H_DEVICE         = 2156;
constexpr uint32_t SCI_OFF_H_SUBDEVICE      = 2160;

// rpc_message_header_v common-header constants (rpc_common.c rpcWriteCommonHeader).
constexpr uint32_t RPC_HEADER_VERSION  = 0x03000000; // MAJOR=3 (31:24), MINOR=0 (23:16)
constexpr uint32_t RPC_SIGNATURE_VALID = 0x43505256; // NV_VGPU_MSG_SIGNATURE_VALID
constexpr uint32_t RPC_RESULT_PENDING  = 0xFFFFFFFF; // NV_VGPU_MSG_RESULT_RPC_PENDING

// rpc_message_header_v03_00 (g_rpc-message-header.h), 32 bytes.
struct rpc_message_header_v {
    uint32_t header_version;
    uint32_t signature;
    uint32_t length;
    uint32_t function;
    uint32_t rpc_result;
    uint32_t rpc_result_private;
    uint32_t sequence;
    uint32_t u;
};
static_assert(sizeof(rpc_message_header_v) == 32, "rpc_message_header_v must be 32 bytes");

// GSP_MSG_QUEUE_ELEMENT (message_queue_priv.h): rpc is 8-aligned after elemCount.
struct gsp_msg_queue_element {
    uint8_t  authTag[16];
    uint8_t  aad[16];
    uint32_t checkSum;
    uint32_t seqNum;
    uint32_t elemCount;
    alignas(8) rpc_message_header_v rpc;
};

// rpc_run_cpu_sequencer_v17_00 (g_rpc-structures.h).
struct rpc_run_cpu_sequencer_v {
    uint32_t bufferSizeDWord;
    uint32_t cmdIndex;
    uint32_t regSaveArea[8];
    uint32_t commandBuffer[1]; // flexible (>=1)
};

// GspSystemInfo (gsp_static_config.h:151-181). The full struct embeds BUSINFO
// (x2) + ACPI_METHOD_DATA + GSP_VF_INFO; on bare-metal those are all zero, so we
// declare the leading critical fields exactly (same offsets as NVIDIA's, natural
// alignment) and pad the remainder. The golden trace sends fn=72 len=704, i.e.
// sizeof(GspSystemInfo)==672 -- the static_assert below is the ABI gate.
struct GspSystemInfo {
    uint64_t gpuPhysAddr;           // 0  -> BAR0 register aperture base
    uint64_t gpuPhysFbAddr;         // 8  -> BAR1 framebuffer aperture base
    uint64_t gpuPhysInstAddr;       // 16 -> BAR3 instance aperture base
    uint64_t nvDomainBusDeviceFunc; // 24 -> (domain<<32)|(bus<<8)|device
    uint64_t simAccessBufPhysAddr;  // 32
    uint64_t pcieAtomicsOpMask;     // 40
    uint64_t consoleMemSize;        // 48
    uint64_t maxUserVa;             // 56
    uint32_t pciConfigMirrorBase;   // 64 -> DEVICE_BASE(NV_PCFG)=0x88000
    uint32_t pciConfigMirrorSize;   // 68 -> 0x1000
    uint8_t  oorArch;               // 72 -> OOR_ARCH_X86_64 = 0
    uint8_t  _pad[599];             // clPdbProperties..bTdrEventSupported (all 0 here)
};
static_assert(sizeof(GspSystemInfo) == 672, "GspSystemInfo must be 672 bytes (trace fn=72 len=704)");

// PACKED_REGISTRY_TABLE (g_os_nvoc.h:232). Empty table = header only.
struct PackedRegistryTable {
    uint32_t size;       // total packed bytes (== 8 for an empty table)
    uint32_t numEntries; // 0
};
static_assert(sizeof(PackedRegistryTable) == 8, "empty registry table is 8 bytes");

// rpc_set_guest_system_info_v03_00 (g_rpc-structures.h:36-47).
struct rpc_set_guest_system_info_v {
    uint32_t vgxVersionMajorNum;
    uint32_t vgxVersionMinorNum;
    uint32_t guestDriverVersionBufferLength;
    uint32_t guestVersionBufferLength;
    uint32_t guestTitleBufferLength;
    uint32_t guestClNum;
    char     guestDriverVersion[0x100];
    char     guestVersion[0x100];
    char     guestTitle[0x100];
};
static_assert(sizeof(rpc_set_guest_system_info_v) == 792, "set_guest_system_info is 792 bytes");

// _checkSum32: XOR of u64s over the range, folded to u32 (reads up to the next
// 8-byte boundary; the buffer is padded, so over-read is safe).
static uint32_t checksum32(const void* p, uint32_t len) {
    const uint64_t* q   = static_cast<const uint64_t*>(p);
    const uint8_t*  end = static_cast<const uint8_t*>(p) + len;
    uint64_t cs = 0;
    while (reinterpret_cast<const uint8_t*>(q) < end) {
        cs ^= *q++;
    }
    return static_cast<uint32_t>(cs >> 32) ^ static_cast<uint32_t>(cs & 0xFFFFFFFFu);
}

// Internal receive result codes.
constexpr int32_t RX_GOT = 0;       // a full message is in rx_staging
constexpr int32_t RX_NONE = 1;      // nothing available (poll again)
constexpr int32_t RX_BADMSG = 2;    // checksum/seqNum bad (skipped)

// _kgspGenerateInitArgId: pack up to 8 chars big-endian into a u64.
static uint64_t init_arg_id(const char* name) {
    uint64_t id = 0;
    for (uint32_t i = 0; i < 8 && name[i] != '\0'; i++) {
        id = (id << 8) | static_cast<uint8_t>(name[i]);
    }
    return id;
}

// libos log buffer header: [0] = put pointer (0), [1..pages] = page table
// (phys of each page). Matches kgspInitLibosLoggingStructures (contiguous case).
static void fill_log_buffer(dma_buffer& buf) {
    uint64_t* w = reinterpret_cast<uint64_t*>(buf.cpu_va);
    const uint32_t pages = static_cast<uint32_t>(buf.size / PAGE);
    w[0] = 0;
    for (uint32_t i = 0; i < pages; i++) {
        w[1 + i] = static_cast<uint64_t>(buf.phys) + static_cast<uint64_t>(i) * PAGE;
    }
}

int32_t gsp_rpc_init(gsp_rpc& out) {
    string::memset(&out, 0, sizeof(out));

    // ---- 8a: shared block = [ page table | cmd ring | status ring ] ----
    const uint32_t pageTableSize = PAGE; // 129 PTEs * 8 B = 1032 -> 1 page
    const uint32_t sharedSize    = pageTableSize + CMD_QUEUE_SIZE + STAT_QUEUE_SIZE;
    const uint32_t entryCount    = sharedSize / PAGE; // 129
    if (dma_alloc(sharedSize, /*uncached=*/false, out.shared) != MEM_OK) {
        log::error("nvidia: rpc: shared mem alloc (%u B) failed", sharedSize);
        return ERR_RPC_MEM;
    }
    // Page table maps every page of the shared block (contiguous -> arithmetic).
    uint64_t* pt = reinterpret_cast<uint64_t*>(out.shared.cpu_va);
    for (uint32_t i = 0; i < entryCount; i++) {
        pt[i] = static_cast<uint64_t>(out.shared.phys) + static_cast<uint64_t>(i) * PAGE;
    }
    out.shared_mem_pa          = out.shared.phys; // == pt[0]
    out.cmd_queue_offset       = pageTableSize;
    out.stat_queue_offset      = pageTableSize + CMD_QUEUE_SIZE;
    out.page_table_entry_count = entryCount;

    // Create our TX ring (command queue). The GSP writes the status-queue header.
    void* cmdQueue = reinterpret_cast<void*>(out.shared.cpu_va + out.cmd_queue_offset);
    if (msgq_tx_create(out.cmd_q, cmdQueue, CMD_QUEUE_SIZE, PAGE,
                       /*hdrAlignShift=*/4, /*entryAlignShift=*/12,
                       MSGQ_FLAGS_SWAP_RX) != MSGQ_OK) {
        log::error("nvidia: rpc: msgq_tx_create failed");
        dma_free(out.shared);
        return ERR_RPC_MSGQ;
    }

    // ---- 8b: GSP args (GSP_ARGUMENTS_CACHED) - buffer comes zeroed ----
    if (dma_alloc(sizeof(GspArgumentsCached), /*uncached=*/false, out.gsp_args) != MEM_OK) {
        gsp_rpc_free(out);
        return ERR_RPC_MEM;
    }
    GspArgumentsCached* ga = reinterpret_cast<GspArgumentsCached*>(out.gsp_args.cpu_va);
    ga->messageQueueInitArguments.sharedMemPhysAddr   = out.shared_mem_pa;
    ga->messageQueueInitArguments.pageTableEntryCount = entryCount;
    ga->messageQueueInitArguments.cmdQueueOffset      = out.cmd_queue_offset;
    ga->messageQueueInitArguments.statQueueOffset     = out.stat_queue_offset;
    // lockless*Offset, srInitArguments, gpuInstance, profilerArgs = 0 (zeroed).

    // ---- 8b: libos per-task log buffers (LOGINIT must be first) ----
    static const char* const logNames[GSP_LOG_COUNT] = {"LOGINIT", "LOGINTR", "LOGRM"};
    for (uint32_t i = 0; i < GSP_LOG_COUNT; i++) {
        if (dma_alloc(GSP_LOG_SIZE, /*uncached=*/false, out.log_mem[i]) != MEM_OK) {
            gsp_rpc_free(out);
            return ERR_RPC_MEM;
        }
        fill_log_buffer(out.log_mem[i]);
    }

    // ---- 8b: libos init args (4 KB): 3 logs + RMARGS -> GSP args ----
    if (dma_alloc(LIBOS_INIT_ARGUMENTS_SIZE, /*uncached=*/false, out.libos_args) != MEM_OK) {
        gsp_rpc_free(out);
        return ERR_RPC_MEM;
    }
    LibosMemoryRegionInitArgument* la =
        reinterpret_cast<LibosMemoryRegionInitArgument*>(out.libos_args.cpu_va);
    for (uint32_t i = 0; i < GSP_LOG_COUNT; i++) {
        la[i].id8  = init_arg_id(logNames[i]);
        la[i].pa   = out.log_mem[i].phys;
        la[i].size = GSP_LOG_SIZE;
        la[i].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
        la[i].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;
    }
    la[GSP_LOG_COUNT].id8  = init_arg_id("RMARGS");
    la[GSP_LOG_COUNT].pa   = out.gsp_args.phys;
    la[GSP_LOG_COUNT].size = sizeof(GspArgumentsCached);
    la[GSP_LOG_COUNT].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
    la[GSP_LOG_COUNT].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;

    // Host-only working copy for received status-ring messages (Stage 10).
    if (dma_alloc(RX_STAGING_SIZE, /*uncached=*/false, out.rx_staging) != MEM_OK) {
        gsp_rpc_free(out);
        return ERR_RPC_MEM;
    }
    out.rx_seq_num = 0;
    out.tx_seq_num = 0;

    barrier::dma_full();

    // ---- logging (structural verification) ----
    log::info("nvidia: rpc: shared=%u B (%u pages) @ phys=0x%lx; sharedMemPA=0x%lx",
              sharedSize, entryCount, static_cast<unsigned long>(out.shared.phys),
              static_cast<unsigned long>(out.shared_mem_pa));
    log::info("nvidia: rpc: cmdQueueOff=0x%lx statQueueOff=0x%lx ptEntries=%u pt[1]=0x%lx",
              static_cast<unsigned long>(out.cmd_queue_offset),
              static_cast<unsigned long>(out.stat_queue_offset), entryCount,
              static_cast<unsigned long>(pt[1]));
    log::info("nvidia: rpc: cmd msgq: msgCount=%u msgSize=%u rxHdrOff=%u entryOff=%u flags=%u writePtr=%u",
              out.cmd_q.tx.msgCount, out.cmd_q.tx.msgSize, out.cmd_q.tx.rxHdrOff,
              out.cmd_q.tx.entryOff, out.cmd_q.tx.flags, out.cmd_q.tx.writePtr);
    log::info("nvidia: rpc: gspArgs @ phys=0x%lx (%lu B) | libosArgs @ phys=0x%lx",
              static_cast<unsigned long>(out.gsp_args.phys),
              static_cast<unsigned long>(sizeof(GspArgumentsCached)),
              static_cast<unsigned long>(out.libos_args.phys));
    log::info("nvidia: rpc: logs INIT=0x%lx INTR=0x%lx RM=0x%lx (64KB each)",
              static_cast<unsigned long>(out.log_mem[0].phys),
              static_cast<unsigned long>(out.log_mem[1].phys),
              static_cast<unsigned long>(out.log_mem[2].phys));
    log::info("nvidia: rpc: libosArg id8 LOGINIT=0x%lx RMARGS=0x%lx (pa->gspArgs=0x%lx)",
              static_cast<unsigned long>(la[0].id8),
              static_cast<unsigned long>(la[GSP_LOG_COUNT].id8),
              static_cast<unsigned long>(la[GSP_LOG_COUNT].pa));
    return RPC_OK;
}

void gsp_rpc_free(gsp_rpc& out) {
    dma_free(out.rx_staging);
    dma_free(out.libos_args);
    for (uint32_t i = 0; i < GSP_LOG_COUNT; i++) {
        dma_free(out.log_mem[i]);
    }
    dma_free(out.gsp_args);
    dma_free(out.shared);
    string::memset(&out, 0, sizeof(out));
}

// --- Stage 10b: host->GSP transmit (async init RPCs) -------------------------

// Build + send an RPC element in rx_staging (idle until a recv), copy it into the
// command ring, submit, ring the doorbell. The payload is (part1 ++ part2);
// `wire_payload_len` is the payload byte count placed in rpc.length + covered by
// the checksum. For controls wire==total; for allocs wire==part1 (the 32B alloc
// header) only -- the params (part2) ride physically in the slot beyond the
// length/checksum, exactly as rpcRmApiAlloc_GSP leaves length at 64. Faithful
// port of GspMsgQueueSendCommand + the _kgspRpcSendMessage doorbell.
static int32_t gsp_rpc_send2(gsp_rpc& rpc, uintptr_t bar0_va, uint32_t fn,
                             const void* part1, uint32_t part1_len,
                             const void* part2, uint32_t part2_len,
                             uint32_t wire_payload_len) {
    const uint32_t hdr        = static_cast<uint32_t>(__builtin_offsetof(gsp_msg_queue_element, rpc));
    const uint32_t rpc_hdr    = static_cast<uint32_t>(sizeof(rpc_message_header_v));
    const uint32_t rpc_len    = rpc_hdr + wire_payload_len;
    const uint32_t phys_total = hdr + rpc_hdr + part1_len + part2_len;
    const uint32_t elem_cnt   = (phys_total + PAGE - 1) / PAGE;
    if (elem_cnt * PAGE > RX_STAGING_SIZE) {
        log::error("nvidia: rpc: tx fn=%u too large (%u bytes)", fn, phys_total);
        return ERR_RPC_MSGQ;
    }

    uint8_t* staging = reinterpret_cast<uint8_t*>(rpc.rx_staging.cpu_va);
    string::memset(staging, 0, elem_cnt * PAGE);

    gsp_msg_queue_element* elem = reinterpret_cast<gsp_msg_queue_element*>(staging);
    elem->rpc.header_version     = RPC_HEADER_VERSION;
    elem->rpc.signature          = RPC_SIGNATURE_VALID;
    elem->rpc.length             = rpc_len;
    elem->rpc.function           = fn;
    elem->rpc.rpc_result         = RPC_RESULT_PENDING;
    elem->rpc.rpc_result_private = RPC_RESULT_PENDING;

    uint8_t* pay = staging + hdr + rpc_hdr;
    if (part1 != nullptr && part1_len != 0) {
        string::memcpy(pay, part1, part1_len);
    }
    if (part2 != nullptr && part2_len != 0) {
        string::memcpy(pay + part1_len, part2, part2_len);
    }

    elem->seqNum    = rpc.tx_seq_num;
    elem->elemCount = elem_cnt;
    elem->checkSum  = 0;
    elem->checkSum  = checksum32(staging, hdr + rpc_len); // checksum covers wire length only

    for (uint32_t i = 0; i < elem_cnt; i++) {
        void* slot = msgq_tx_get_write_buffer(rpc.cmd_q, i);
        if (slot == nullptr) {
            log::error("nvidia: rpc: tx fn=%u cmd ring full at slot %u", fn, i);
            return ERR_RPC_MSGQ;
        }
        string::memcpy(slot, staging + i * PAGE, PAGE);
    }

    barrier::dma_full();
    if (msgq_tx_submit_buffers(rpc.cmd_q, elem_cnt) != MSGQ_OK) {
        log::error("nvidia: rpc: tx fn=%u submit failed", fn);
        return ERR_RPC_MSGQ;
    }
    rpc.tx_seq_num++;

    // Doorbell: ring command queue 0 (NV_PGSP_QUEUE_HEAD(0) = 0).
    RUN_ELEVATED(mmio::write32(bar0_va + NV_PGSP_QUEUE_HEAD, 0));
    return RPC_OK;
}

static int32_t gsp_rpc_send(gsp_rpc& rpc, uintptr_t bar0_va, uint32_t fn,
                            const void* payload, uint32_t payload_len) {
    const int32_t rc = gsp_rpc_send2(rpc, bar0_va, fn, payload, payload_len, nullptr, 0, payload_len);
    if (rc == RPC_OK) {
        log::info("nvidia: rpc: sent fn=%u rpcLen=%u (seqNum=%u)", fn,
                  static_cast<uint32_t>(sizeof(rpc_message_header_v)) + payload_len, rpc.tx_seq_num - 1);
    }
    return rc;
}

int32_t gsp_rpc_send_set_system_info(gsp_rpc& rpc, uintptr_t bar0_va,
                                     uint64_t bar0_phys, uint64_t bar1_phys,
                                     uint64_t bar3_phys, uint64_t dbdf) {
    GspSystemInfo si;
    string::memset(&si, 0, sizeof(si));
    si.gpuPhysAddr           = bar0_phys;
    si.gpuPhysFbAddr         = bar1_phys;
    si.gpuPhysInstAddr       = bar3_phys;
    si.nvDomainBusDeviceFunc = dbdf;
    si.pciConfigMirrorBase   = 0x88000; // DEVICE_BASE(NV_PCFG)
    si.pciConfigMirrorSize   = 0x1000;  // DEVICE_EXTENT(NV_PCFG)-BASE+1
    si.oorArch               = 0;       // OOR_ARCH_X86_64
    log::info("nvidia: rpc: SET_SYSTEM_INFO bar0=0x%lx fb=0x%lx inst=0x%lx dbdf=0x%lx",
              static_cast<unsigned long>(bar0_phys), static_cast<unsigned long>(bar1_phys),
              static_cast<unsigned long>(bar3_phys), static_cast<unsigned long>(dbdf));
    return gsp_rpc_send(rpc, bar0_va, FUNC_GSP_SET_SYSTEM_INFO, &si, sizeof(si));
}

int32_t gsp_rpc_send_set_registry(gsp_rpc& rpc, uintptr_t bar0_va) {
    PackedRegistryTable reg;
    reg.size       = sizeof(PackedRegistryTable); // empty table, 8 bytes
    reg.numEntries = 0;
    log::info("nvidia: rpc: SET_REGISTRY (empty table, size=%u)", reg.size);
    return gsp_rpc_send(rpc, bar0_va, FUNC_SET_REGISTRY, &reg, sizeof(reg));
}

// --- Stage 10: receive framing + CPU-sequencer dispatch loop -----------------

// Pull one complete message off the status ring into rpc.rx_staging, validating
// the XOR checksum and the expected sequence number. Faithful port of the rx
// half of GspMsgQueueReceiveStatus / _kgspRpcRecvPoll.
static int32_t gsp_rpc_receive(gsp_rpc& rpc) {
    barrier::dma_full();

    uint8_t* staging = reinterpret_cast<uint8_t*>(rpc.rx_staging.cpu_va);

    // Slot 0 carries the element header (incl. elemCount).
    const void* slot0 = msgq_rx_get_read_buffer(rpc.cmd_q, 0);
    if (slot0 == nullptr) {
        return RX_NONE;
    }
    string::memcpy(staging, slot0, PAGE);

    gsp_msg_queue_element* elem = reinterpret_cast<gsp_msg_queue_element*>(staging);
    const uint32_t elemCount = elem->elemCount;
    if (elemCount == 0 || elemCount * PAGE > RX_STAGING_SIZE) {
        log::error("nvidia: rpc: bad elemCount=%u (seqNum=%u)", elemCount, elem->seqNum);
        msgq_rx_mark_consumed(rpc.cmd_q, 1);
        return RX_BADMSG;
    }

    // Remaining slots; if not yet all present, leave them and retry later.
    for (uint32_t i = 1; i < elemCount; i++) {
        const void* s = msgq_rx_get_read_buffer(rpc.cmd_q, i);
        if (s == nullptr) {
            return RX_NONE;
        }
        string::memcpy(staging + i * PAGE, s, PAGE);
    }

    const uint32_t hdr = static_cast<uint32_t>(__builtin_offsetof(gsp_msg_queue_element, rpc));
    const uint32_t cs = checksum32(staging, hdr + elem->rpc.length);
    if (cs != 0) {
        log::error("nvidia: rpc: checksum mismatch (got 0x%x, len=%u, seqNum=%u)",
                   cs, elem->rpc.length, elem->seqNum);
        msgq_rx_mark_consumed(rpc.cmd_q, elemCount);
        return RX_BADMSG;
    }
    if (elem->seqNum != rpc.rx_seq_num) {
        log::error("nvidia: rpc: seqNum mismatch (expected %u, got %u)",
                   rpc.rx_seq_num, elem->seqNum);
        msgq_rx_mark_consumed(rpc.cmd_q, elemCount);
        return RX_BADMSG;
    }

    msgq_rx_mark_consumed(rpc.cmd_q, elemCount);
    rpc.rx_seq_num++;
    return RX_GOT;
}

// Drain GSP->host status messages, dispatching async events (CPU sequencer +
// benign ignorable events) en route, until a message with function==expected_fn
// arrives (left in rx_staging for the caller) or timeout. Generalizes
// _kgspRpcRecvPoll / _kgspRpcDrainEvents.
static int32_t gsp_rpc_recv_poll(gsp_rpc& rpc, const gsp_seq_ctx& ctx,
                                 uint32_t expected_fn, uint64_t timeout_ns,
                                 uint32_t* out_seq_runs) {
    // GSP bring-up is a synchronous boot: busy-poll the status ring while it's empty, exactly like the
    // xhci HCD polls hardware during reset/port init (delay::us). The first sequencer opcode is a
    // time-critical MAILBOX0 handshake, so the poll must NOT yield/sleep (a scheduler round-trip misses
    // the GSP's window). Steady-state event waits belong on the framework's wait_for_event() path.
    constexpr uint64_t GSP_RPC_POLL_US = 50;
    uint32_t n_seq = 0;
    const uint64_t start = clock::now_ns();
    while (clock::now_ns() - start < timeout_ns) {
        const int32_t r = gsp_rpc_receive(rpc);
        if (r == RX_NONE) {
            delay::us(GSP_RPC_POLL_US);
            continue;
        }
        if (r == RX_BADMSG) {
            continue;
        }

        gsp_msg_queue_element* elem = reinterpret_cast<gsp_msg_queue_element*>(rpc.rx_staging.cpu_va);
        const uint32_t fn = elem->rpc.function;

        if (fn == expected_fn) {
            if (out_seq_runs) *out_seq_runs = n_seq;
            return RPC_OK; // event or sync reply is in rx_staging
        }

        if (fn == EVENT_GSP_RUN_CPU_SEQUENCER) {
            rpc_run_cpu_sequencer_v* p = reinterpret_cast<rpc_run_cpu_sequencer_v*>(
                reinterpret_cast<uint8_t*>(&elem->rpc) + sizeof(rpc_message_header_v));
            log::info("nvidia: rpc: RUN_CPU_SEQUENCER (cmdIndex=%u, bufSizeDw=%u)",
                      p->cmdIndex, p->bufferSizeDWord);
            const int32_t src = gsp_run_sequencer(ctx, p->commandBuffer, p->cmdIndex, p->regSaveArea);
            if (src != SEQ_OK) {
                log::error("nvidia: rpc: sequencer failed rc=%d", src);
                return src;
            }
            n_seq++;
            continue;
        }

        log::info("nvidia: rpc: GSP event fn=0x%x len=%u (ignored)", fn, elem->rpc.length);
    }

    log::error("nvidia: rpc: timed out waiting for fn=0x%x", expected_fn);
    return ERR_RPC_INITDONE_TIMEOUT;
}

int32_t gsp_rpc_wait_for_init_done(gsp_rpc& rpc, const gsp_seq_ctx& ctx) {
    // Link the GSP's status ring. The GSP brings up its TX header asynchronously
    // after the Booter starts it, so poll until the link succeeds.
    void* status_queue = reinterpret_cast<void*>(rpc.shared.cpu_va + rpc.stat_queue_offset);
    int32_t lrc = MSGQ_ERR;
    const uint64_t link_start = clock::now_ns();
    while (clock::now_ns() - link_start < 5000000000ull) { // 5 s
        barrier::dma_full();
        lrc = msgq_rx_link(rpc.cmd_q, status_queue, STAT_QUEUE_SIZE, PAGE);
        if (lrc == MSGQ_OK) {
            break;
        }
        delay::us(1000);
    }
    if (lrc != MSGQ_OK) {
        log::error("nvidia: rpc: status ring never came up (rc=%d) - GSP not producing", lrc);
        return ERR_RPC_STATUSQ;
    }
    log::info("nvidia: rpc: status ring linked; draining GSP events until INIT_DONE");
    rpc.rx_seq_num = 0;

    uint32_t n_seq = 0;
    const int32_t rc = gsp_rpc_recv_poll(rpc, ctx, EVENT_GSP_INIT_DONE, 15000000000ull, &n_seq);
    if (rc == RPC_OK) {
        log::info("nvidia: rpc: GSP_INIT_DONE received (sequencer runs=%u) -- GSP-RM IS UP", n_seq);
    }
    return rc;
}

// Issue a synchronous RPC: build+send the request (payload may be nullptr for a
// zero-filled body), then poll the status ring for the reply (function==fn, left
// in rx_staging). *out_result = the reply's rpc_result.
static int32_t gsp_rpc_call(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va,
                            uint32_t fn, const void* payload, uint32_t payload_len,
                            uint32_t* out_result) {
    int32_t rc = gsp_rpc_send(rpc, bar0_va, fn, payload, payload_len);
    if (rc != RPC_OK) {
        return rc;
    }
    rc = gsp_rpc_recv_poll(rpc, ctx, fn, 10000000000ull, nullptr); // 10 s
    if (rc != RPC_OK) {
        return rc;
    }
    gsp_msg_queue_element* elem = reinterpret_cast<gsp_msg_queue_element*>(rpc.rx_staging.cpu_va);
    if (out_result) {
        *out_result = elem->rpc.rpc_result;
    }
    return RPC_OK;
}

// Offset of the reply control/alloc body (after the rpc header) in rx_staging.
static inline uint32_t reply_body_off() {
    return static_cast<uint32_t>(__builtin_offsetof(gsp_msg_queue_element, rpc) +
                                 sizeof(rpc_message_header_v));
}

int32_t gsp_rpc_control(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va,
                        uint32_t hClient, uint32_t hObject, uint32_t cmd,
                        void* params, uint32_t paramsSize, uint32_t* out_status) {
    // rpc_gsp_rm_control_v03_00: {hClient,hObject,cmd,status,paramsSize,flags}=24B + params.
    uint32_t chdr[6] = { hClient, hObject, cmd, 0u, paramsSize, 0u };
    int32_t rc = gsp_rpc_send2(rpc, bar0_va, FUNC_GSP_RM_CONTROL,
                               chdr, sizeof(chdr), params, paramsSize,
                               sizeof(chdr) + paramsSize); // controls count params in length
    if (rc != RPC_OK) {
        log::error("nvidia: rpc: CONTROL cmd=0x%x send failed rc=%d", cmd, rc);
        return rc;
    }
    rc = gsp_rpc_recv_poll(rpc, ctx, FUNC_GSP_RM_CONTROL, 10000000000ull, nullptr);
    if (rc != RPC_OK) {
        log::error("nvidia: rpc: CONTROL cmd=0x%x no reply rc=%d", cmd, rc);
        return rc;
    }
    uint8_t* st = reinterpret_cast<uint8_t*>(rpc.rx_staging.cpu_va);
    uint32_t status = 0;
    string::memcpy(&status, st + reply_body_off() + 12, 4); // control hdr status @12
    if (out_status) *out_status = status;
    if (params != nullptr && paramsSize != 0) {
        string::memcpy(params, st + reply_body_off() + 24, paramsSize); // reply params back
    }
    return RPC_OK;
}

int32_t gsp_rpc_alloc(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va,
                      uint32_t hClient, uint32_t hParent, uint32_t hObject, uint32_t hClass,
                      const void* params, uint32_t paramsSize, uint32_t* out_status) {
    // rpc_gsp_rm_alloc_v03_00: {hClient,hParent,hObject,hClass,status,paramsSize,flags,reserved}=32B + params.
    uint32_t ahdr[8] = { hClient, hParent, hObject, hClass, 0u, paramsSize, 0u, 0u };
    // wire length = alloc header only (32); params ride beyond, per rpcRmApiAlloc_GSP.
    int32_t rc = gsp_rpc_send2(rpc, bar0_va, FUNC_GSP_RM_ALLOC,
                               ahdr, sizeof(ahdr), params, paramsSize,
                               sizeof(ahdr));
    if (rc != RPC_OK) {
        log::error("nvidia: rpc: ALLOC hClass=0x%x send failed rc=%d", hClass, rc);
        return rc;
    }
    rc = gsp_rpc_recv_poll(rpc, ctx, FUNC_GSP_RM_ALLOC, 10000000000ull, nullptr);
    if (rc != RPC_OK) {
        log::error("nvidia: rpc: ALLOC hClass=0x%x no reply rc=%d", hClass, rc);
        return rc;
    }
    uint8_t* st = reinterpret_cast<uint8_t*>(rpc.rx_staging.cpu_va);
    uint32_t status = 0;
    string::memcpy(&status, st + reply_body_off() + 16, 4); // alloc hdr status @16
    if (out_status) *out_status = status;
    return RPC_OK;
}

int32_t gsp_rpc_alloc_inout(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va,
                            uint32_t hClient, uint32_t hParent, uint32_t hObject, uint32_t hClass,
                            void* params, uint32_t paramsSize, uint32_t* out_status) {
    uint32_t ahdr[8] = { hClient, hParent, hObject, hClass, 0u, paramsSize, 0u, 0u };
    int32_t rc = gsp_rpc_send2(rpc, bar0_va, FUNC_GSP_RM_ALLOC,
                               ahdr, sizeof(ahdr), params, paramsSize, sizeof(ahdr));
    if (rc != RPC_OK) {
        log::error("nvidia: rpc: ALLOC(inout) hClass=0x%x send failed rc=%d", hClass, rc);
        return rc;
    }
    rc = gsp_rpc_recv_poll(rpc, ctx, FUNC_GSP_RM_ALLOC, 10000000000ull, nullptr);
    if (rc != RPC_OK) {
        log::error("nvidia: rpc: ALLOC(inout) hClass=0x%x no reply rc=%d", hClass, rc);
        return rc;
    }
    uint8_t* st = reinterpret_cast<uint8_t*>(rpc.rx_staging.cpu_va);
    uint32_t status = 0;
    string::memcpy(&status, st + reply_body_off() + 16, 4); // alloc hdr status @16
    if (out_status) *out_status = status;
    // rpcRmApiAlloc_GSP echoes the (possibly updated) alloc-params back after the
    // 32-byte alloc header -> copy them out (e.g. NVOS32 OUT offset/address).
    if (params != nullptr && paramsSize != 0) {
        string::memcpy(params, st + reply_body_off() + 32, paramsSize);
    }
    return RPC_OK;
}

int32_t gsp_rpc_set_guest_system_info(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va) {
    rpc_set_guest_system_info_v si;
    string::memset(&si, 0, sizeof(si));
    si.vgxVersionMajorNum             = VGX_MAJOR_VERSION;
    si.vgxVersionMinorNum             = VGX_MINOR_VERSION;
    si.guestDriverVersionBufferLength = VGX_INFO_BUF_SIZE;
    si.guestVersionBufferLength       = VGX_INFO_BUF_SIZE;
    si.guestTitleBufferLength         = VGX_INFO_BUF_SIZE;
    si.guestClNum                     = NV_BUILD_CL_NUM;
    string::memcpy(si.guestDriverVersion, "535.183.01", sizeof("535.183.01"));
    string::memcpy(si.guestVersion, "rel/gpu_drv/r535/r538_67-552", sizeof("rel/gpu_drv/r535/r538_67-552"));
    string::memcpy(si.guestTitle, "NVIDIA Open GPU Kernel Module 535.183.01",
                   sizeof("NVIDIA Open GPU Kernel Module 535.183.01"));

    uint32_t result = 0xFFFFFFFFu;
    const int32_t rc = gsp_rpc_call(rpc, ctx, bar0_va, FUNC_SET_GUEST_SYSTEM_INFO,
                                    &si, sizeof(si), &result);
    if (rc != RPC_OK) {
        log::error("nvidia: rpc: SET_GUEST_SYSTEM_INFO failed rc=%d", rc);
        return rc;
    }
    log::info("nvidia: rpc: SET_GUEST_SYSTEM_INFO ok (rpc_result=0x%x, vgx %u.%u, drv 535.183.01)",
              result, VGX_MAJOR_VERSION, VGX_MINOR_VERSION);
    return (result == 0) ? RPC_OK : ERR_RPC_MSGQ;
}

int32_t gsp_rpc_get_static_info(gsp_rpc& rpc, const gsp_seq_ctx& ctx, uintptr_t bar0_va,
                                gsp_static_info& out) {
    string::memset(&out, 0, sizeof(out));

    // Request body is a zero-filled GspStaticConfigInfo; the GSP fills it in the
    // reply. gsp_rpc_send zero-fills the element, so a null payload suffices.
    uint32_t result = 0xFFFFFFFFu;
    const int32_t rc = gsp_rpc_call(rpc, ctx, bar0_va, FUNC_GET_GSP_STATIC_INFO,
                                    nullptr, SCI_SIZE, &result);
    if (rc != RPC_OK) {
        log::error("nvidia: rpc: GET_GSP_STATIC_INFO failed rc=%d", rc);
        return rc;
    }
    if (result != 0) {
        log::error("nvidia: rpc: GET_GSP_STATIC_INFO rpc_result=0x%x", result);
        return ERR_RPC_MSGQ;
    }

    // GspStaticConfigInfo begins right after the rpc header in the reply element.
    const uint8_t* sci = reinterpret_cast<uint8_t*>(rpc.rx_staging.cpu_va) +
                         __builtin_offsetof(gsp_msg_queue_element, rpc) +
                         sizeof(rpc_message_header_v);

    string::memcpy(out.gpu_name, sci + SCI_OFF_GPU_NAME, 63);
    out.gpu_name[63] = '\0';
    string::memcpy(&out.fb_length, sci + SCI_OFF_FB_LENGTH, sizeof(out.fb_length));
    string::memcpy(&out.fbio_mask, sci + SCI_OFF_FBIO_MASK, 4);
    string::memcpy(&out.fb_bus_width, sci + SCI_OFF_FB_BUS_WIDTH, 4);
    string::memcpy(&out.fb_ram_type, sci + SCI_OFF_FB_RAM_TYPE, 4);
    string::memcpy(&out.l2_cache_size, sci + SCI_OFF_L2_CACHE_SIZE, 4);
    out.poison_fuse_enabled = sci[SCI_OFF_POISON_FUSE];
    out.vbios_valid = sci[SCI_OFF_VBIOS_VALID];
    string::memcpy(&out.vbios_sub_vendor, sci + SCI_OFF_VBIOS_SUBVENDOR, 4);
    string::memcpy(&out.vbios_sub_device, sci + SCI_OFF_VBIOS_SUBDEVICE, 4);
    string::memcpy(&out.h_internal_client, sci + SCI_OFF_H_CLIENT, 4);
    string::memcpy(&out.h_internal_device, sci + SCI_OFF_H_DEVICE, 4);
    string::memcpy(&out.h_internal_subdevice, sci + SCI_OFF_H_SUBDEVICE, 4);

    log::info("nvidia: ====== GSP static GPU info ======");
    log::info("nvidia:   GPU            : %s", out.gpu_name);
    log::info("nvidia:   Framebuffer    : %lu MiB (0x%lx bytes)",
              static_cast<unsigned long>(out.fb_length >> 20),
              static_cast<unsigned long>(out.fb_length));
    log::info("nvidia:   FB ram_type=%u bus_width=%u fbio_mask=0x%x L2=%u KiB",
              out.fb_ram_type, out.fb_bus_width, out.fbio_mask, out.l2_cache_size >> 10);
    log::info("nvidia:   VBIOS valid=%u subVendor=0x%04x subDevice=0x%04x | ECC poison-fuse=%u",
              out.vbios_valid, out.vbios_sub_vendor, out.vbios_sub_device, out.poison_fuse_enabled);
    log::info("nvidia:   internal handles: client=0x%08x device=0x%08x subdevice=0x%08x",
              out.h_internal_client, out.h_internal_device, out.h_internal_subdevice);
    log::info("nvidia: =================================");
    return RPC_OK;
}

} // namespace nvidia
