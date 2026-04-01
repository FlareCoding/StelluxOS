# NVIDIA GSP Message Queue System — Complete Binary Reference (r535)

Complete byte-level reference for the GSP-RM message queue communication system
used in NVIDIA r535 firmware. Derived from Linux kernel nouveau driver source
(`drivers/gpu/drm/nouveau/nvkm/subdev/gsp/`), NVIDIA open-gpu-kernel-modules
(`535.113.01`), and nova-core Rust driver patches.

All multi-byte fields are **little-endian**. All sizes in bytes unless noted.

---

## Table of Contents

1. [Constants and Sizing](#1-constants-and-sizing)
2. [Queue Header Structs](#2-queue-header-structs)
3. [Shared Memory Layout](#3-shared-memory-layout)
4. [GSP Message Element (r535_gsp_msg / GSP_MSG_QUEUE_ELEMENT)](#4-gsp-message-element)
5. [RPC Message Header (nvfw_gsp_rpc / rpc_message_header)](#5-rpc-message-header)
6. [Complete Message Layout](#6-complete-message-layout)
7. [Queue Ring Buffer Operations](#7-queue-ring-buffer-operations)
8. [Large Message / Continuation Records](#8-large-message--continuation-records)
9. [LibOS Init Arguments](#9-libos-init-arguments)
10. [RM Boot Arguments (GSP_ARGUMENTS_CACHED)](#10-rm-boot-arguments)
11. [Boot Sequence and INIT_DONE](#11-boot-sequence-and-init_done)
12. [First RPCs After Boot](#12-first-rpcs-after-boot)
13. [RM Alloc RPC Format](#13-rm-alloc-rpc-format)
14. [All NV_VGPU_MSG_FUNCTION Constants](#14-all-nv_vgpu_msg_function-constants)
15. [All NV_VGPU_MSG_EVENT Constants](#15-all-nv_vgpu_msg_event-constants)
16. [C++ Struct Definitions](#16-c-struct-definitions)

---

## 1. Constants and Sizing

```
GSP_PAGE_SHIFT     = 12
GSP_PAGE_SIZE      = 0x1000          (4096 bytes)

GSP_MSG_MIN_SIZE   = GSP_PAGE_SIZE   (0x1000 = 4096)
GSP_MSG_MAX_SIZE   = GSP_MSG_MIN_SIZE * 16  (0x10000 = 65536 = 16 pages)

Command Queue Size = 0x40000         (256 KB = 64 pages)
Status Queue Size  = 0x40000         (256 KB = 64 pages)

Queue Entry Size   = GSP_PAGE_SIZE   (0x1000 per slot)
Queue Entry Count  = (queue_size - entryOff) / msgSize
                   = (0x40000 - 0x1000) / 0x1000 = 63 entries

Total Queues       = 2 (command queue: CPU→GSP, status/message queue: GSP→CPU)
```

Source: `nvkm/subdev/gsp/rm/r535/rpc.c`, `include/nvkm/subdev/gsp.h`

---

## 2. Queue Header Structs

### msgqTxHeader (transmit side — writer's header)

**Source:** `nvkm/subdev/gsp/rm/r535/nvrm/gsp.h` — from NVIDIA `message_queue_priv.h`

| Offset | Size | Field       | Description |
|--------|------|-------------|-------------|
| 0x00   | 4    | `version`   | Queue version. Set to `0` |
| 0x04   | 4    | `size`      | Total queue size in bytes (page-aligned). `0x40000` |
| 0x08   | 4    | `msgSize`   | Entry size in bytes. Must be power-of-2, minimum 16. Set to `0x1000` |
| 0x0C   | 4    | `msgCount`  | Number of entries in queue. `(size - entryOff) / msgSize` = 63 |
| 0x10   | 4    | `writePtr`  | Write pointer: index of next slot to write. Initialized to `0` |
| 0x14   | 4    | `flags`     | If set to `1`, means "I want to swap RX". Command queue sets to `1` |
| 0x18   | 4    | `rxHdrOff`  | Offset of corresponding `msgqRxHeader` from start of this queue's backing store |
| 0x1C   | 4    | `entryOff`  | Offset of first entry from start of backing store. Set to `0x1000` (one page) |

**Total size: 32 bytes (0x20)**

### msgqRxHeader (receive side — reader's header)

| Offset | Size | Field      | Description |
|--------|------|------------|-------------|
| 0x00   | 4    | `readPtr`  | Read pointer: index of last message read. Initialized to `0` |

**Total size: 4 bytes (0x04)**

### Combined Queue Header Layout

Each queue's first page (`entryOff = 0x1000`) contains both headers:

```
Offset 0x000: msgqTxHeader  (32 bytes)
Offset 0x020: msgqRxHeader  (4 bytes)
Offset 0x024: padding       (to fill page)
...
Offset 0x1000: First queue entry (page 0)
Offset 0x2000: Second queue entry (page 1)
...
```

### Cross-Queue Pointer Layout

The **command queue** (CPU→GSP):
- `cmdq.wptr` → `&cmdq_header->tx.writePtr`    (writer = CPU)
- `cmdq.rptr` → `&msgq_header->rx.readPtr`      (reader = GSP, stored in msgq's rx header)

The **status/message queue** (GSP→CPU):
- `msgq.wptr` → `&msgq_header->tx.writePtr`     (writer = GSP)
- `msgq.rptr` → `&cmdq_header->rx.readPtr`       (reader = CPU, stored in cmdq's rx header)

This means each queue has its read pointer stored at the **other** queue's `rxHdrOff` offset.

```c
// From r535_gsp_shared_init():
cmdq->tx.rxHdrOff = offsetof(typeof(*cmdq), rx.readPtr);  // = 0x20

gsp->cmdq.wptr = &cmdq->tx.writePtr;     // command queue write ptr
gsp->cmdq.rptr = &msgq->rx.readPtr;      // command queue read ptr (in msgq)
gsp->msgq.wptr = &msgq->tx.writePtr;     // message queue write ptr
gsp->msgq.rptr = &cmdq->rx.readPtr;      // message queue read ptr (in cmdq)
```

---

## 3. Shared Memory Layout

### PTE Array

Before the queues, there is a page table entry (PTE) array that maps the shared memory
pages for GSP DMA access.

```
Total shared mem = ptes_size + cmdq_size + msgq_size

ptes.nr   = (cmdq_size + msgq_size) >> GSP_PAGE_SHIFT
          + DIV_ROUND_UP(ptes.nr * sizeof(u64), GSP_PAGE_SIZE)
          = (0x40000 + 0x40000) / 0x1000 + ceil(128*8 / 4096)
          = 128 + 1 = 129

ptes.size = ALIGN(ptes.nr * sizeof(u64), GSP_PAGE_SIZE)
          = ALIGN(129 * 8, 0x1000) = ALIGN(1032, 4096) = 0x1000

Each PTE entry: u64 physical_address_of_page[i]
```

### Complete Shared Memory Map

```
Base address = shm.mem.addr (DMA-coherent allocation)

Offset 0x000000: PTE array         (0x1000 bytes = 1 page)
                 ptes[0] = base + 0*0x1000
                 ptes[1] = base + 1*0x1000
                 ...
                 ptes[128] = base + 128*0x1000

Offset 0x001000: Command Queue     (0x40000 bytes = 256 KB)
  +0x000: cmdq TxHeader (32 bytes)
  +0x020: cmdq RxHeader (4 bytes)  ← msgq read pointer lives here
  +0x024: padding
  +0x1000: cmdq entry[0]  (4096 bytes)
  +0x2000: cmdq entry[1]
  ...
  +0x3F000: cmdq entry[62]

Offset 0x041000: Status/Message Queue (0x40000 bytes = 256 KB)
  +0x000: msgq TxHeader (32 bytes)
  +0x020: msgq RxHeader (4 bytes)  ← cmdq read pointer lives here
  +0x024: padding
  +0x1000: msgq entry[0]
  ...
  +0x3F000: msgq entry[62]
```

---

## 4. GSP Message Element

**Source:** `nvkm/subdev/gsp/rm/r535/rpc.c` (struct `r535_gsp_msg`),
`nvrm/gsp.h` (struct `GSP_MSG_QUEUE_ELEMENT`)

This is the transport-level header that wraps every RPC message.

### struct r535_gsp_msg / GSP_MSG_QUEUE_ELEMENT

| Offset | Size | Field             | Description |
|--------|------|-------------------|-------------|
| 0x00   | 16   | `authTagBuffer`   | Authentication tag buffer (encryption, zero for plaintext) |
| 0x10   | 16   | `aadBuffer`       | AAD (Additional Authenticated Data) buffer (zero for plaintext) |
| 0x20   | 4    | `checkSum`        | XOR checksum over entire padded message. Set so total XOR = 0 |
| 0x24   | 4    | `seqNum`/`sequence` | Transport sequence number, maintained by queue (`cmdq.seq++`) |
| 0x28   | 4    | `elemCount`       | Number of GSP pages this element spans: `ceil(total_size / 0x1000)` |
| 0x2C   | 4    | `pad`             | Padding, set to 0 before checksum calculation |
| 0x30   | var  | `data[]` / `rpc`  | Start of RPC message header (`nvfw_gsp_rpc`) |

**Header size: 0x30 (48 bytes)**

Note: In the NVIDIA headers, `GSP_MSG_QUEUE_ELEMENT` has the `rpc` field
`NV_DECLARE_ALIGNED(..., 8)`, meaning the RPC header is 8-byte aligned at offset 0x30.

### Checksum Calculation

```c
// Pad message to page boundary
u32 len = ALIGN(GSP_MSG_HDR_SIZE + gsp_rpc_len, GSP_PAGE_SIZE);
msg->pad = 0;
msg->checksum = 0;

// XOR all 64-bit words
u64 csum = 0;
for (u64 *ptr = (u64*)msg; ptr < end; ptr++)
    csum ^= *ptr;

msg->checksum = upper_32_bits(csum) ^ lower_32_bits(csum);
```

---

## 5. RPC Message Header

**Source:** `nvkm/subdev/gsp/rm/r535/rpc.c` (struct `nvfw_gsp_rpc`),
`nvrm/gsp.h` (struct `rpc_message_header_v03_00`)

### struct nvfw_gsp_rpc / rpc_message_header_v03_00

| Offset | Size | Field                | Description |
|--------|------|----------------------|-------------|
| 0x00   | 4    | `header_version`     | Always `0x03000000` |
| 0x04   | 4    | `signature`          | Always `0x56525043` = `'V'<<24 \| 'R'<<16 \| 'P'<<8 \| 'C'` = `"VRPC"` (stored LE as bytes `43 50 52 56`) |
| 0x08   | 4    | `length`             | Total size of RPC header + payload (bytes). `sizeof(nvfw_gsp_rpc) + payload_size` |
| 0x0C   | 4    | `function`           | RPC function number. See [NV_VGPU_MSG_FUNCTION constants](#14-all-nv_vgpu_msg_function-constants) |
| 0x10   | 4    | `rpc_result`         | Result code. Initialized to `0xFFFFFFFF`, GSP sets to status |
| 0x14   | 4    | `rpc_result_private` | Private result. Initialized to `0xFFFFFFFF` |
| 0x18   | 4    | `sequence`           | RPC-level sequence number (`gsp->rpc_seq++`). Distinct from transport `seqNum` |
| 0x1C   | 4    | `spare`/`cpuRmGfid`  | Union: spare field or CPU RM GFID |
| 0x20   | var  | `data[]`             | Payload starts here |

**Header size: 0x20 (32 bytes)**

### Signature Construction

```c
rpc->signature = ('C' << 24) | ('P' << 16) | ('R' << 8) | 'V';
// = 0x43505256 when read as big-endian
// In little-endian memory: bytes 56 52 50 43 → "VRPC"
```

Wait — the code says:
```c
rpc->signature = ('C' << 24) | ('P' << 16) | ('R' << 8) | 'V';
```
This gives: `0x43505256`. In little-endian memory at offset 0x04:
`56 52 50 43` → reads as ASCII "VRPC".

---

## 6. Complete Message Layout

A complete GSP message as placed in the queue:

```
Byte offsets from start of queue entry:

0x00 +----------------------------------+
     | authTagBuffer[16]                |  (16 bytes, all zero)
0x10 +----------------------------------+
     | aadBuffer[16]                    |  (16 bytes, all zero)
0x20 +----------------------------------+
     | checkSum         (u32)           |
0x24 +----------------------------------+
     | seqNum           (u32)           |  transport sequence
0x28 +----------------------------------+
     | elemCount        (u32)           |  number of pages
0x2C +----------------------------------+
     | pad              (u32)           |  zero
0x30 +----------------------------------+ ← nvfw_gsp_rpc starts here
     | header_version   (u32)           |  0x03000000
0x34 +----------------------------------+
     | signature        (u32)           |  0x43505256
0x38 +----------------------------------+
     | length           (u32)           |  sizeof(rpc_hdr) + payload
0x3C +----------------------------------+
     | function         (u32)           |  RPC function number
0x40 +----------------------------------+
     | rpc_result       (u32)           |  0xFFFFFFFF initially
0x44 +----------------------------------+
     | rpc_result_private (u32)         |  0xFFFFFFFF initially
0x48 +----------------------------------+
     | sequence         (u32)           |  RPC sequence number
0x4C +----------------------------------+
     | spare/cpuRmGfid  (u32)           |
0x50 +----------------------------------+ ← payload starts here
     | RPC-specific payload             |
     | (e.g. GspSystemInfo, etc.)       |
     +----------------------------------+
     | zero-padded to GSP_PAGE_SIZE     |
     +----------------------------------+
```

**Key offsets from queue entry start:**
- Transport header: 0x00–0x2F (48 bytes)
- RPC header: 0x30–0x4F (32 bytes)
- Payload: 0x50 onward
- Total overhead: 80 bytes (0x50)

**Maximum payload in a single element:**
```
max_rpc_size    = GSP_MSG_MAX_SIZE - GSP_MSG_HDR_SIZE
                = 0x10000 - 0x30 = 0xFFD0 = 65488 bytes

max_payload     = max_rpc_size - sizeof(nvfw_gsp_rpc)
                = 0xFFD0 - 0x20 = 0xFFB0 = 65456 bytes
```

### Alignment Requirements

```c
// RPC get allocates with 8-byte alignment on payload size:
rpc = r535_gsp_cmdq_get(gsp, ALIGN(sizeof(*rpc) + payload_size, sizeof(u64)));

// Message allocation is rounded up to GSP_MSG_MIN_SIZE (one page):
size = ALIGN(GSP_MSG_HDR_SIZE + gsp_rpc_len, GSP_MSG_MIN_SIZE);
```

---

## 7. Queue Ring Buffer Operations

### Write (CPU → Command Queue)

```
1. Calculate total length:
   len = ALIGN(GSP_MSG_HDR_SIZE + gsp_rpc_len, GSP_PAGE_SIZE)

2. Fill in transport header:
   msg->pad = 0
   msg->checksum = 0
   msg->sequence = cmdq.seq++
   msg->elem_count = len / 0x1000

3. Calculate XOR checksum over entire padded message

4. Wait for free space:
   free = rptr + cnt - wptr - 1
   (wrapping: if free >= cnt then free -= cnt)
   Wait until free >= 1

5. Copy message to queue:
   cqe = cmdq_base + 0x1000 + wptr * 0x1000
   Handle wrap-around: if wptr reaches cnt, wrap to 0

6. Update write pointer:
   wmb()  // write memory barrier
   *cmdq.wptr = new_wptr
   mb()   // full barrier

7. Ring doorbell:
   falcon_wr32(0xc00, 0x00000000)
```

### Read (Status/Message Queue → CPU)

```
1. Wait for data:
   used = wptr + cnt - rptr
   (if used >= cnt then used -= cnt)
   Wait until used >= needed_pages

2. Read entry:
   mqe = msgq_base + 0x1000 + rptr * 0x1000

3. Copy data, handling wrap-around:
   If entry wraps past end of queue, copy in two parts:
   - Part 1: from rptr to end of queue
   - Part 2: from start of queue (entry[0])

4. Advance read pointer:
   rptr = (rptr + num_pages) % cnt
   mb()
   *msgq.rptr = rptr
```

### Wrap-Around Semantics

The queue is a simple modular ring buffer. Both `writePtr` and `readPtr` are
**entry indices** (not byte offsets), ranging from 0 to `msgCount - 1`.
There is NO explicit wrap counter — wrap is detected by modular arithmetic.

---

## 8. Large Message / Continuation Records

When a payload exceeds `max_payload_size` (65456 bytes), it is split:

```
Element 1: function = original_function_number
  Payload: first max_payload_size bytes

Element 2: function = NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD (71)
  Payload: next chunk (up to max_payload_size bytes)

Element 3: function = NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD (71)
  Payload: next chunk
  ...
```

Each element is sent separately with `NVKM_GSP_RPC_REPLY_NOWAIT`. After all
chunks are sent, the driver waits for the reply with the original function number.

On the receive side, continuation elements are detected by their function number (71),
and their payloads (minus the RPC header) are concatenated. The final assembled RPC
has its `length` field updated to the total size.

---

## 9. LibOS Init Arguments

**Source:** `nvkm/subdev/gsp/rm/r535/gsp.c` — `r535_gsp_libos_init()`

GSP firmware expects a 4KB page (`gsp->libos`) containing an array of
`LibosMemoryRegionInitArgument` structs.

### LibosMemoryRegionInitArgument

| Offset | Size | Field  | Description |
|--------|------|--------|-------------|
| 0x00   | 8    | `id8`  | 8-byte ID tag (ASCII name packed big-endian into u64) |
| 0x08   | 8    | `pa`   | Physical/DMA address of the memory region |
| 0x10   | 8    | `size` | Size of the memory region in bytes |
| 0x18   | 1    | `kind` | Memory kind: 0=NONE, 1=CONTIGUOUS, 2=RADIX3 |
| 0x19   | 1    | `loc`  | Memory location: 0=NONE, 1=SYSMEM, 2=FB |
| 0x1A   | 6    | padding | (struct padding to 0x20 — implied by 8-byte aligned `id8`) |

**Entry size: 0x20 (32 bytes)** (due to alignment of the `LibosAddress` u64 fields)

### ID8 Encoding

```c
static inline u64 r535_gsp_libos_id8(const char *name) {
    u64 id = 0;
    for (int i = 0; i < sizeof(id) && *name; i++, name++)
        id = (id << 8) | *name;
    return id;
}
```

Examples:
```
"LOGINIT" → 0x4C4F47494E495400  (shifted into high bytes, zero-padded)
"LOGINTR" → 0x4C4F47494E545200
"LOGRM"   → 0x4C4F47524D000000
"RMARGS"  → 0x524D415247530000
```

### LibOS Array (4 entries, index 0–3)

| Index | ID8       | DMA Address        | Size      | Kind        | Loc    |
|-------|-----------|--------------------|-----------| ------------|--------|
| 0     | "LOGINIT" | `loginit.addr`     | 0x10000 (64 KB) | CONTIGUOUS (1) | SYSMEM (1) |
| 1     | "LOGINTR" | `logintr.addr`     | 0x10000 (64 KB) | CONTIGUOUS (1) | SYSMEM (1) |
| 2     | "LOGRM"   | `logrm.addr`      | 0x10000 (64 KB) | CONTIGUOUS (1) | SYSMEM (1) |
| 3     | "RMARGS"  | `rmargs.addr`      | 0x1000 (4 KB)  | CONTIGUOUS (1) | SYSMEM (1) |

### Log Buffer Internal Format

Each log buffer (loginit, logintr, logrm) has this layout:
```
Offset 0x00: u64 put_pointer    (initialized to 0; dword index for next write)
Offset 0x08: u64 pte[0]         (DMA address of page 0)
Offset 0x10: u64 pte[1]         (DMA address of page 1)
...
```

The PTE array is filled by `create_pte_array()`:
```c
for (i = 0; i < num_pages; i++)
    ptes[i] = (u64)addr + (i << GSP_PAGE_SHIFT);
```

GSP writes log data starting after the PTE array. If `put_pointer == 0`,
the buffer has never been written to.

### How libos address is passed to GSP

```c
// The libos DMA address is written to falcon mailbox registers:
nvkm_falcon_wr32(&gsp->falcon, 0x040, lower_32_bits(gsp->libos.addr));
nvkm_falcon_wr32(&gsp->falcon, 0x044, upper_32_bits(gsp->libos.addr));
```

---

## 10. RM Boot Arguments

**Source:** `nvrm/gsp.h` — `GSP_ARGUMENTS_CACHED`

The RMARGS entry (index 3 in libos array) points to a 4KB page containing:

### GSP_ARGUMENTS_CACHED

| Offset | Size | Field | Type |
|--------|------|-------|------|
| 0x00   | — | `messageQueueInitArguments` | `MESSAGE_QUEUE_INIT_ARGUMENTS` |
| — | — | `srInitArguments` | `GSP_SR_INIT_ARGUMENTS` |
| — | 4 | `gpuInstance` | `NvU32` |
| — | 16 | `profilerArgs` | `{ u64 pa; u64 size; }` |

### MESSAGE_QUEUE_INIT_ARGUMENTS

| Offset | Size | Field                    | Description |
|--------|------|--------------------------|-------------|
| 0x00   | 8    | `sharedMemPhysAddr`      | DMA address of entire shared memory allocation (PTE array + queues) |
| 0x08   | 4    | `pageTableEntryCount`    | Number of PTEs (129 for default sizing) |
| 0x0C   | 4    | `cmdQueueOffset`         | Byte offset of command queue from sharedMemPhysAddr. = `ptes_size` (0x1000) |
| 0x10   | 4    | `statQueueOffset`        | Byte offset of status queue from sharedMemPhysAddr. = `ptes_size + cmdq_size` (0x41000) |
| 0x14   | 4    | `locklessCmdQueueOffset`  | Unused in r535 (set to 0) |
| 0x18   | 4    | `locklessStatQueueOffset` | Unused in r535 (set to 0) |

Note: `NvLength` is `NvU32` (4 bytes), `RmPhysAddr` is `NvU64` (8 bytes).

Total `MESSAGE_QUEUE_INIT_ARGUMENTS` size: **0x1C (28 bytes)**, but may have padding
to align subsequent fields.

### GSP_SR_INIT_ARGUMENTS

| Offset | Size | Field             | Description |
|--------|------|-------------------|-------------|
| 0x00   | 4    | `oldLevel`        | Power level. 0 = fresh boot, 3 = resume from suspend |
| 0x04   | 4    | `flags`           | Flags. Set to 0 |
| 0x08   | 4    | `bInPMTransition` | Boolean. 0 = fresh boot, 1 = resume |

### Initial Boot Values

```c
args->messageQueueInitArguments.sharedMemPhysAddr = gsp->shm.mem.addr;
args->messageQueueInitArguments.pageTableEntryCount = 129;  // typical
args->messageQueueInitArguments.cmdQueueOffset = 0x1000;     // after PTE page
args->messageQueueInitArguments.statQueueOffset = 0x41000;   // after cmdq

args->srInitArguments.oldLevel = 0;        // fresh boot
args->srInitArguments.flags = 0;
args->srInitArguments.bInPMTransition = 0; // not resuming
```

---

## 11. Boot Sequence and INIT_DONE

### Boot Flow

```
1. Allocate shared memory (PTEs + cmdq + msgq)
2. Initialize queue headers (msgqTxHeader, msgqRxHeader)
3. Allocate rmargs (GSP_ARGUMENTS_CACHED)
4. Allocate log buffers (loginit, logintr, logrm)
5. Build libos array (4 entries: LOGINIT, LOGINTR, LOGRM, RMARGS)
6. Send SET_SYSTEM_INFO RPC (function 72)        ← before GSP boot
7. Send SET_REGISTRY RPC (function 73)            ← before GSP boot
8. Boot GSP firmware (SEC2 booter → RISC-V start)
9. Write libos DMA address to falcon mailbox 0x040/0x044
10. Write app_version to falcon register 0x080
11. Poll for GSP_INIT_DONE event on status queue
12. Send GET_GSP_STATIC_INFO RPC (function 65)
```

### GSP_INIT_DONE Event

**Event number:** `NV_VGPU_MSG_EVENT_GSP_INIT_DONE = 0x1001`

This is received as an RPC message on the **status queue** with:
- `function = 0x1001`
- No payload (just the RPC header)
- `rpc_result = 0` on success

```c
// Wait for GSP_INIT_DONE:
r535_gsp_rpc_poll(gsp, NV_VGPU_MSG_EVENT_GSP_INIT_DONE);

// rpc_poll receives the message, checks function matches 0x1001,
// discards the reply, and returns 0 on success.
```

### Pre-Boot RPCs

Before GSP firmware is started, the following RPCs are sent on the command queue:

1. **GSP_SET_SYSTEM_INFO** (function 72) — GPU physical addresses, PCI info, ACPI data
2. **SET_REGISTRY** (function 73) — Registry key/value pairs for GSP-RM config

These are sent with `NVKM_GSP_RPC_REPLY_NOSEQ` (no sequence number, no wait for reply).

---

## 12. First RPCs After Boot

After `GSP_INIT_DONE` is received:

### 1. GET_GSP_STATIC_INFO (function 65)

```c
rpc = nvkm_gsp_rpc_rd(gsp, NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO, sizeof(GspStaticConfigInfo));
```

This returns `GspStaticConfigInfo` containing:
- Internal client/device/subdevice handles (`hInternalClient`, `hInternalDevice`, `hInternalSubdevice`)
- BAR1/BAR2 PDE base addresses
- FB region info
- GPC/TPC masks
- GPU name strings
- Various capability flags

### 2. Interrupt Table Query

```c
NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE (0x20800a5c)
```
Via `rm_ctrl` (not direct RPC).

### Client Allocation (user-facing)

For creating RM clients externally:
```c
NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC (103)  // wraps rm_alloc
// with hClass = NV01_ROOT = 0x0000
// NV0000_ALLOC_PARAMETERS as payload
```

Class numbers:
```
NV01_ROOT           = 0x0000   (root client allocation)
NV01_DEVICE_0       = 0x0080   (device)
NV20_SUBDEVICE_0    = 0x2080   (subdevice)
```

---

## 13. RM Alloc RPC Format

**Source:** `nvrm/alloc.h`

### rpc_gsp_rm_alloc_v03_00

Used with `NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC` (function 103).

| Offset | Size | Field        | Description |
|--------|------|--------------|-------------|
| 0x00   | 4    | `hClient`    | Client handle |
| 0x04   | 4    | `hParent`    | Parent object handle |
| 0x08   | 4    | `hObject`    | New object handle |
| 0x0C   | 4    | `hClass`     | Class number (e.g., 0x0000 for NV01_ROOT) |
| 0x10   | 4    | `status`     | Result status (filled by GSP) |
| 0x14   | 4    | `paramsSize` | Size of params[] in bytes |
| 0x18   | 4    | `flags`      | Allocation flags |
| 0x1C   | 4    | `reserved[4]`| Reserved bytes |
| 0x20   | var  | `params[]`   | Class-specific allocation parameters |

### rpc_free_v03_00

Used with `NV_VGPU_MSG_FUNCTION_FREE` (function 10).

| Offset | Size | Field             | Description |
|--------|------|-------------------|-------------|
| 0x00   | 4    | `hRoot`           | Root client handle |
| 0x04   | 4    | `hObjectParent`   | Parent handle |
| 0x08   | 4    | `hObjectOld`      | Handle of object to free |
| 0x0C   | 4    | `status`          | Result status |

---

## 14. All NV_VGPU_MSG_FUNCTION Constants

RPC function numbers (used in `nvfw_gsp_rpc.function`):

```
NV_VGPU_MSG_FUNCTION_NOP                            =   0
NV_VGPU_MSG_FUNCTION_SET_GUEST_SYSTEM_INFO           =   1
NV_VGPU_MSG_FUNCTION_ALLOC_ROOT                      =   2
NV_VGPU_MSG_FUNCTION_ALLOC_DEVICE                    =   3  (deprecated)
NV_VGPU_MSG_FUNCTION_ALLOC_MEMORY                    =   4
NV_VGPU_MSG_FUNCTION_ALLOC_CTX_DMA                   =   5
NV_VGPU_MSG_FUNCTION_ALLOC_CHANNEL_DMA               =   6
NV_VGPU_MSG_FUNCTION_MAP_MEMORY                      =   7
NV_VGPU_MSG_FUNCTION_BIND_CTX_DMA                    =   8  (deprecated)
NV_VGPU_MSG_FUNCTION_ALLOC_OBJECT                    =   9
NV_VGPU_MSG_FUNCTION_FREE                            =  10
NV_VGPU_MSG_FUNCTION_LOG                             =  11
NV_VGPU_MSG_FUNCTION_ALLOC_VIDMEM                    =  12
NV_VGPU_MSG_FUNCTION_UNMAP_MEMORY                    =  13
NV_VGPU_MSG_FUNCTION_MAP_MEMORY_DMA                  =  14
NV_VGPU_MSG_FUNCTION_UNMAP_MEMORY_DMA                =  15
NV_VGPU_MSG_FUNCTION_GET_EDID                        =  16
NV_VGPU_MSG_FUNCTION_ALLOC_DISP_CHANNEL              =  17
NV_VGPU_MSG_FUNCTION_ALLOC_DISP_OBJECT               =  18
NV_VGPU_MSG_FUNCTION_ALLOC_SUBDEVICE                 =  19
NV_VGPU_MSG_FUNCTION_ALLOC_DYNAMIC_MEMORY            =  20
NV_VGPU_MSG_FUNCTION_DUP_OBJECT                      =  21
NV_VGPU_MSG_FUNCTION_IDLE_CHANNELS                   =  22
NV_VGPU_MSG_FUNCTION_ALLOC_EVENT                     =  23
NV_VGPU_MSG_FUNCTION_SEND_EVENT                      =  24
NV_VGPU_MSG_FUNCTION_REMAPPER_CONTROL                =  25  (deprecated)
NV_VGPU_MSG_FUNCTION_DMA_CONTROL                     =  26
NV_VGPU_MSG_FUNCTION_DMA_FILL_PTE_MEM                =  27
NV_VGPU_MSG_FUNCTION_MANAGE_HW_RESOURCE              =  28
NV_VGPU_MSG_FUNCTION_BIND_ARBITRARY_CTX_DMA          =  29  (deprecated)
NV_VGPU_MSG_FUNCTION_CREATE_FB_SEGMENT               =  30
NV_VGPU_MSG_FUNCTION_DESTROY_FB_SEGMENT              =  31
NV_VGPU_MSG_FUNCTION_ALLOC_SHARE_DEVICE              =  32
NV_VGPU_MSG_FUNCTION_DEFERRED_API_CONTROL            =  33
NV_VGPU_MSG_FUNCTION_REMOVE_DEFERRED_API             =  34
NV_VGPU_MSG_FUNCTION_SIM_ESCAPE_READ                 =  35
NV_VGPU_MSG_FUNCTION_SIM_ESCAPE_WRITE                =  36
NV_VGPU_MSG_FUNCTION_SIM_MANAGE_DISPLAY_CONTEXT_DMA  =  37
NV_VGPU_MSG_FUNCTION_FREE_VIDMEM_VIRT                =  38
NV_VGPU_MSG_FUNCTION_PERF_GET_PSTATE_INFO            =  39
NV_VGPU_MSG_FUNCTION_PERF_GET_PERFMON_SAMPLE         =  40
NV_VGPU_MSG_FUNCTION_PERF_GET_VIRTUAL_PSTATE_INFO    =  41  (deprecated)
NV_VGPU_MSG_FUNCTION_PERF_GET_LEVEL_INFO             =  42
NV_VGPU_MSG_FUNCTION_MAP_SEMA_MEMORY                 =  43
NV_VGPU_MSG_FUNCTION_UNMAP_SEMA_MEMORY               =  44
NV_VGPU_MSG_FUNCTION_SET_SURFACE_PROPERTIES           =  45
NV_VGPU_MSG_FUNCTION_CLEANUP_SURFACE                  =  46
NV_VGPU_MSG_FUNCTION_UNLOADING_GUEST_DRIVER           =  47
NV_VGPU_MSG_FUNCTION_TDR_SET_TIMEOUT_STATE            =  48
NV_VGPU_MSG_FUNCTION_SWITCH_TO_VGA                    =  49
NV_VGPU_MSG_FUNCTION_GPU_EXEC_REG_OPS                 =  50
NV_VGPU_MSG_FUNCTION_GET_STATIC_INFO                  =  51
NV_VGPU_MSG_FUNCTION_ALLOC_VIRTMEM                    =  52
NV_VGPU_MSG_FUNCTION_UPDATE_PDE_2                     =  53
NV_VGPU_MSG_FUNCTION_SET_PAGE_DIRECTORY               =  54
NV_VGPU_MSG_FUNCTION_GET_STATIC_PSTATE_INFO           =  55
NV_VGPU_MSG_FUNCTION_TRANSLATE_GUEST_GPU_PTES         =  56
NV_VGPU_MSG_FUNCTION_RESERVED_57                      =  57
NV_VGPU_MSG_FUNCTION_RESET_CURRENT_GR_CONTEXT         =  58
NV_VGPU_MSG_FUNCTION_SET_SEMA_MEM_VALIDATION_STATE    =  59
NV_VGPU_MSG_FUNCTION_GET_ENGINE_UTILIZATION            =  60
NV_VGPU_MSG_FUNCTION_UPDATE_GPU_PDES                   =  61
NV_VGPU_MSG_FUNCTION_GET_ENCODER_CAPACITY              =  62
NV_VGPU_MSG_FUNCTION_VGPU_PF_REG_READ32               =  63
NV_VGPU_MSG_FUNCTION_SET_GUEST_SYSTEM_INFO_EXT         =  64
NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO               =  65
NV_VGPU_MSG_FUNCTION_RMFS_INIT                         =  66
NV_VGPU_MSG_FUNCTION_RMFS_CLOSE_QUEUE                  =  67
NV_VGPU_MSG_FUNCTION_RMFS_CLEANUP                      =  68
NV_VGPU_MSG_FUNCTION_RMFS_TEST                         =  69
NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE                    =  70
NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD               =  71
NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO               =  72
NV_VGPU_MSG_FUNCTION_SET_REGISTRY                      =  73
NV_VGPU_MSG_FUNCTION_GSP_INIT_POST_OBJGPU              =  74  (deprecated)
NV_VGPU_MSG_FUNCTION_SUBDEV_EVENT_SET_NOTIFICATION     =  75  (deprecated)
NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL                    =  76
NV_VGPU_MSG_FUNCTION_GET_STATIC_INFO2                  =  77
NV_VGPU_MSG_FUNCTION_DUMP_PROTOBUF_COMPONENT           =  78
NV_VGPU_MSG_FUNCTION_UNSET_PAGE_DIRECTORY              =  79
NV_VGPU_MSG_FUNCTION_GET_CONSOLIDATED_STATIC_INFO      =  80
NV_VGPU_MSG_FUNCTION_GMMU_REGISTER_FAULT_BUFFER        =  81  (deprecated)
NV_VGPU_MSG_FUNCTION_GMMU_UNREGISTER_FAULT_BUFFER      =  82  (deprecated)
NV_VGPU_MSG_FUNCTION_GMMU_REGISTER_CLIENT_SHADOW_FAULT_BUFFER   = 83  (deprecated)
NV_VGPU_MSG_FUNCTION_GMMU_UNREGISTER_CLIENT_SHADOW_FAULT_BUFFER = 84  (deprecated)
NV_VGPU_MSG_FUNCTION_CTRL_SET_VGPU_FB_USAGE            =  85
NV_VGPU_MSG_FUNCTION_CTRL_NVFBC_SW_SESSION_UPDATE_INFO  =  86
NV_VGPU_MSG_FUNCTION_CTRL_NVENC_SW_SESSION_UPDATE_INFO  =  87
NV_VGPU_MSG_FUNCTION_CTRL_RESET_CHANNEL                 =  88
NV_VGPU_MSG_FUNCTION_CTRL_RESET_ISOLATED_CHANNEL        =  89
NV_VGPU_MSG_FUNCTION_CTRL_GPU_HANDLE_VF_PRI_FAULT       =  90
NV_VGPU_MSG_FUNCTION_CTRL_CLK_GET_EXTENDED_INFO         =  91
NV_VGPU_MSG_FUNCTION_CTRL_PERF_BOOST                    =  92
NV_VGPU_MSG_FUNCTION_CTRL_PERF_VPSTATES_GET_CONTROL     =  93
NV_VGPU_MSG_FUNCTION_CTRL_GET_ZBC_CLEAR_TABLE           =  94
NV_VGPU_MSG_FUNCTION_CTRL_SET_ZBC_COLOR_CLEAR           =  95
NV_VGPU_MSG_FUNCTION_CTRL_SET_ZBC_DEPTH_CLEAR           =  96
NV_VGPU_MSG_FUNCTION_CTRL_GPFIFO_SCHEDULE               =  97
NV_VGPU_MSG_FUNCTION_CTRL_SET_TIMESLICE                 =  98
NV_VGPU_MSG_FUNCTION_CTRL_PREEMPT                       =  99
NV_VGPU_MSG_FUNCTION_CTRL_FIFO_DISABLE_CHANNELS         = 100
NV_VGPU_MSG_FUNCTION_CTRL_SET_TSG_INTERLEAVE_LEVEL      = 101
NV_VGPU_MSG_FUNCTION_CTRL_SET_CHANNEL_INTERLEAVE_LEVEL  = 102
NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC                       = 103
NV_VGPU_MSG_FUNCTION_CTRL_GET_P2P_CAPS_V2               = 104
  ... (105–202 follow sequentially, see rpcfn.h)
NV_VGPU_MSG_FUNCTION_NUM_FUNCTIONS                       = 203
```

**Key boot-time functions:**
- 65: `GET_GSP_STATIC_INFO` — first RPC after init_done
- 71: `CONTINUATION_RECORD` — continuation for large messages
- 72: `GSP_SET_SYSTEM_INFO` — pre-boot system info
- 73: `SET_REGISTRY` — pre-boot registry config
- 76: `GSP_RM_CONTROL` — generic RM control call
- 103: `GSP_RM_ALLOC` — generic RM object allocation

---

## 15. All NV_VGPU_MSG_EVENT Constants

Event numbers (received on status queue, `function` field):

```
NV_VGPU_MSG_EVENT_FIRST_EVENT                       = 0x1000
NV_VGPU_MSG_EVENT_GSP_INIT_DONE                     = 0x1001
NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER             = 0x1002
NV_VGPU_MSG_EVENT_POST_EVENT                        = 0x1003
NV_VGPU_MSG_EVENT_RC_TRIGGERED                      = 0x1004
NV_VGPU_MSG_EVENT_MMU_FAULT_QUEUED                  = 0x1005
NV_VGPU_MSG_EVENT_OS_ERROR_LOG                      = 0x1006
NV_VGPU_MSG_EVENT_RG_LINE_INTR                      = 0x1007
NV_VGPU_MSG_EVENT_GPUACCT_PERFMON_UTIL_SAMPLES      = 0x1008
NV_VGPU_MSG_EVENT_SIM_READ                          = 0x1009
NV_VGPU_MSG_EVENT_SIM_WRITE                         = 0x100a
NV_VGPU_MSG_EVENT_SEMAPHORE_SCHEDULE_CALLBACK       = 0x100b
NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT                 = 0x100c
NV_VGPU_MSG_EVENT_VGPU_GSP_PLUGIN_TRIGGERED         = 0x100d
NV_VGPU_MSG_EVENT_PERF_GPU_BOOST_SYNC_LIMITS_CALLBACK = 0x100e
NV_VGPU_MSG_EVENT_PERF_BRIDGELESS_INFO_UPDATE       = 0x100f
NV_VGPU_MSG_EVENT_VGPU_CONFIG                       = 0x1010
NV_VGPU_MSG_EVENT_DISPLAY_MODESET                   = 0x1011
NV_VGPU_MSG_EVENT_EXTDEV_INTR_SERVICE               = 0x1012
NV_VGPU_MSG_EVENT_NVLINK_INBAND_RECEIVED_DATA_256   = 0x1013
NV_VGPU_MSG_EVENT_NVLINK_INBAND_RECEIVED_DATA_512   = 0x1014
NV_VGPU_MSG_EVENT_NVLINK_INBAND_RECEIVED_DATA_1024  = 0x1015
NV_VGPU_MSG_EVENT_NVLINK_INBAND_RECEIVED_DATA_2048  = 0x1016
NV_VGPU_MSG_EVENT_NVLINK_INBAND_RECEIVED_DATA_4096  = 0x1017
NV_VGPU_MSG_EVENT_TIMED_SEMAPHORE_RELEASE           = 0x1018
NV_VGPU_MSG_EVENT_NVLINK_IS_GPU_DEGRADED            = 0x1019
NV_VGPU_MSG_EVENT_PFM_REQ_HNDLR_STATE_SYNC_CALLBACK = 0x101a
NV_VGPU_MSG_EVENT_GSP_SEND_USER_SHARED_DATA         = 0x101b
NV_VGPU_MSG_EVENT_NVLINK_FAULT_UP                   = 0x101c
NV_VGPU_MSG_EVENT_GSP_LOCKDOWN_NOTICE               = 0x101d
NV_VGPU_MSG_EVENT_MIG_CI_CONFIG_UPDATE               = 0x101e
NV_VGPU_MSG_EVENT_NUM_EVENTS                         = 0x101f
```

---

## 16. C++ Struct Definitions

Complete binary-compatible C++ structs for bare-metal use:

```cpp
#include <cstdint>

// ============================================================================
// Constants
// ============================================================================

static constexpr uint32_t GSP_PAGE_SHIFT = 12;
static constexpr uint32_t GSP_PAGE_SIZE  = 1u << GSP_PAGE_SHIFT;  // 0x1000

static constexpr uint32_t GSP_MSG_MIN_SIZE = GSP_PAGE_SIZE;
static constexpr uint32_t GSP_MSG_MAX_SIZE = GSP_MSG_MIN_SIZE * 16;  // 0x10000

static constexpr uint32_t GSP_CMDQ_SIZE = 0x40000;  // 256 KB
static constexpr uint32_t GSP_MSGQ_SIZE = 0x40000;  // 256 KB

// ============================================================================
// Queue Headers
// ============================================================================

struct MsgqTxHeader {
    uint32_t version;    // 0x00: queue version (0)
    uint32_t size;       // 0x04: total queue size in bytes
    uint32_t msgSize;    // 0x08: entry size (0x1000)
    uint32_t msgCount;   // 0x0C: number of entries
    uint32_t writePtr;   // 0x10: write index
    uint32_t flags;      // 0x14: 1 = swap RX
    uint32_t rxHdrOff;   // 0x18: offset to RX header from queue start
    uint32_t entryOff;   // 0x1C: offset to first entry from queue start
};
static_assert(sizeof(MsgqTxHeader) == 0x20);

struct MsgqRxHeader {
    uint32_t readPtr;    // 0x00: read index
};
static_assert(sizeof(MsgqRxHeader) == 0x04);

struct QueueHeader {
    MsgqTxHeader tx;     // 0x00
    MsgqRxHeader rx;     // 0x20
};
static_assert(sizeof(QueueHeader) == 0x24);

// ============================================================================
// GSP Message Queue Element (transport header)
// ============================================================================

static constexpr uint32_t GSP_MSG_HDR_SIZE = 0x30;

struct GspMsgQueueElement {
    uint8_t  authTagBuffer[16]; // 0x00: authentication tag (zero)
    uint8_t  aadBuffer[16];     // 0x10: AAD buffer (zero)
    uint32_t checkSum;          // 0x20: XOR checksum
    uint32_t seqNum;            // 0x24: transport sequence number
    uint32_t elemCount;         // 0x28: number of pages in this element
    uint32_t pad;               // 0x2C: zero
    // 0x30: rpc_message_header follows (nvfw_gsp_rpc)
};
static_assert(sizeof(GspMsgQueueElement) == 0x30);

// ============================================================================
// RPC Message Header
// ============================================================================

static constexpr uint32_t GSP_RPC_HEADER_VERSION = 0x03000000;
static constexpr uint32_t GSP_RPC_SIGNATURE = 0x43505256;  // 'C'<<24|'P'<<16|'R'<<8|'V'

struct NvfwGspRpc {
    uint32_t headerVersion;      // 0x00: 0x03000000
    uint32_t signature;          // 0x04: 0x43505256
    uint32_t length;             // 0x08: sizeof(NvfwGspRpc) + payload_size
    uint32_t function;           // 0x0C: RPC function number
    uint32_t rpcResult;          // 0x10: result (0xFFFFFFFF initially)
    uint32_t rpcResultPrivate;   // 0x14: private result (0xFFFFFFFF initially)
    uint32_t sequence;           // 0x18: RPC sequence number
    union {
        uint32_t spare;
        uint32_t cpuRmGfid;
    };                           // 0x1C
    // 0x20: payload begins
};
static_assert(sizeof(NvfwGspRpc) == 0x20);

// ============================================================================
// LibOS Memory Region Init Argument
// ============================================================================

struct LibosMemoryRegionInitArgument {
    uint64_t id8;    // 0x00: 8-byte packed ASCII ID
    uint64_t pa;     // 0x08: physical/DMA address
    uint64_t size;   // 0x10: size in bytes
    uint8_t  kind;   // 0x18: 0=NONE, 1=CONTIGUOUS, 2=RADIX3
    uint8_t  loc;    // 0x19: 0=NONE, 1=SYSMEM, 2=FB
    uint8_t  pad[6]; // 0x1A: padding to 0x20
};
static_assert(sizeof(LibosMemoryRegionInitArgument) == 0x20);

// ============================================================================
// Message Queue Init Arguments
// ============================================================================

struct MessageQueueInitArguments {
    uint64_t sharedMemPhysAddr;         // 0x00
    uint32_t pageTableEntryCount;       // 0x08
    uint32_t cmdQueueOffset;            // 0x0C  (NvLength = u32)
    uint32_t statQueueOffset;           // 0x10
    uint32_t locklessCmdQueueOffset;    // 0x14  (unused, 0)
    uint32_t locklessStatQueueOffset;   // 0x18  (unused, 0)
};

struct GspSrInitArguments {
    uint32_t oldLevel;        // 0x00: 0 = boot, 3 = resume
    uint32_t flags;           // 0x04: 0
    uint32_t bInPMTransition; // 0x08: NvBool (0 or 1)
};

struct GspArgumentsCached {
    MessageQueueInitArguments messageQueueInitArguments;
    GspSrInitArguments        srInitArguments;
    uint32_t                  gpuInstance;
    struct {
        uint64_t pa;
        uint64_t size;
    } profilerArgs;
};

// ============================================================================
// RM Alloc RPC Payload
// ============================================================================

struct RpcGspRmAlloc {
    uint32_t hClient;      // 0x00
    uint32_t hParent;      // 0x04
    uint32_t hObject;      // 0x08
    uint32_t hClass;       // 0x0C
    uint32_t status;       // 0x10
    uint32_t paramsSize;   // 0x14
    uint32_t flags;        // 0x18
    uint8_t  reserved[4];  // 0x1C
    // 0x20: params[] follow
};
static_assert(sizeof(RpcGspRmAlloc) == 0x20);

struct RpcFree {
    uint32_t hRoot;          // 0x00
    uint32_t hObjectParent;  // 0x04
    uint32_t hObjectOld;     // 0x08
    uint32_t status;         // 0x0C
};

// ============================================================================
// NV01_ROOT Client Allocation Parameters
// ============================================================================

static constexpr uint32_t NV01_ROOT = 0x0000;
static constexpr uint32_t NV_PROC_NAME_MAX_LENGTH = 100;

struct Nv0000AllocParameters {
    uint32_t hClient;
    uint32_t processID;
    char     processName[NV_PROC_NAME_MAX_LENGTH];
};

// ============================================================================
// System Info RPC Payload (function 72)
// ============================================================================

struct GspSystemInfo {
    uint64_t gpuPhysAddr;
    uint64_t gpuPhysFbAddr;
    uint64_t gpuPhysInstAddr;
    uint64_t nvDomainBusDeviceFunc;
    uint64_t simAccessBufPhysAddr;
    uint64_t pcieAtomicsOpMask;
    uint64_t consoleMemSize;
    uint64_t maxUserVa;
    uint32_t pciConfigMirrorBase;
    uint32_t pciConfigMirrorSize;
    uint8_t  oorArch;
    // ... many more fields follow (ACPI data, chipset info, etc.)
    // See full definition in nvrm/gsp.h
};

// ============================================================================
// RPC Function Numbers (boot-critical subset)
// ============================================================================

enum NvVgpuMsgFunction : uint32_t {
    NV_VGPU_MSG_FUNCTION_NOP                        =   0,
    NV_VGPU_MSG_FUNCTION_FREE                       =  10,
    NV_VGPU_MSG_FUNCTION_UNLOADING_GUEST_DRIVER     =  47,
    NV_VGPU_MSG_FUNCTION_GET_STATIC_INFO            =  51,
    NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO        =  65,
    NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD        =  71,
    NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO        =  72,
    NV_VGPU_MSG_FUNCTION_SET_REGISTRY               =  73,
    NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL             =  76,
    NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC               = 103,
};

enum NvVgpuMsgEvent : uint32_t {
    NV_VGPU_MSG_EVENT_GSP_INIT_DONE                 = 0x1001,
    NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER         = 0x1002,
    NV_VGPU_MSG_EVENT_POST_EVENT                    = 0x1003,
    NV_VGPU_MSG_EVENT_RC_TRIGGERED                  = 0x1004,
    NV_VGPU_MSG_EVENT_MMU_FAULT_QUEUED              = 0x1005,
    NV_VGPU_MSG_EVENT_OS_ERROR_LOG                  = 0x1006,
    NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT             = 0x100c,
    NV_VGPU_MSG_EVENT_PERF_BRIDGELESS_INFO_UPDATE   = 0x100f,
    NV_VGPU_MSG_EVENT_GSP_SEND_USER_SHARED_DATA     = 0x101b,
};

// ============================================================================
// Registry RPC Payload (function 73)
// ============================================================================

struct PackedRegistryEntry {
    uint32_t nameOffset;  // offset into table for key name string
    uint8_t  type;        // 1=DWORD, 2=BINARY, 3=STRING
    uint32_t data;        // value (DWORD) or offset (BINARY/STRING)
    uint32_t length;      // value length in bytes
};

struct PackedRegistryTable {
    uint32_t size;        // total size of the table
    uint32_t numEntries;
    // PackedRegistryEntry entries[numEntries] follow
    // then: null-terminated key strings
    // then: binary/string values
};
```

---

## Quick Reference: Complete Boot Handshake

```
CPU                                    GSP
 |                                      |
 |-- Allocate shared memory ----------->|
 |-- Fill PTE array ------------------->|
 |-- Init queue headers --------------->|
 |-- Allocate libos + logs + rmargs --->|
 |-- Fill GSP_ARGUMENTS_CACHED -------->|
 |-- Write libos addr to MBOX40/44 --->|
 |                                      |
 |-- Send GSP_SET_SYSTEM_INFO (72) --->| (pre-boot, NOSEQ)
 |-- Send SET_REGISTRY (73) ---------->| (pre-boot, NOSEQ)
 |                                      |
 |-- Start GSP (SEC2 booter) --------->|
 |-- Write app_version to reg 0x080 -->|
 |                                      |
 |   [GSP boots, processes queued RPCs] |
 |                                      |
 |<-- GSP_INIT_DONE (0x1001) ---------| (on status queue)
 |                                      |
 |-- GET_GSP_STATIC_INFO (65) -------->|
 |<-- GspStaticConfigInfo -------------|
 |                                      |
 |-- [Interrupt table query via ctrl] ->|
 |-- [Enable interrupts] ------------->|
 |                                      |
 |   READY FOR NORMAL OPERATION         |
```
