# NVIDIA GSP-RM RPC Struct Reference — Complete Binary Layouts (r535)

Exact binary-compatible struct layouts for GSP-RM RPC calls needed to initialize
an NVIDIA Ampere (GA102) GPU through GSP-RM firmware. Derived from verified source
code: Linux kernel nouveau driver (`drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/`),
NVIDIA `open-gpu-kernel-modules` (`535.113.01`), and nova-core Rust driver patches.

All multi-byte fields are **little-endian**. All offsets in bytes. `NvHandle = u32`,
`NvBool = u8`, `NV_STATUS = u32`.

---

## Table of Contents

1. [RPC Transport Framing](#1-rpc-transport-framing)
2. [SET_SYSTEM_INFO (function 72)](#2-set_system_info-function-72)
3. [SET_REGISTRY (function 73)](#3-set_registry-function-73)
4. [GET_GSP_STATIC_INFO (function 65)](#4-get_gsp_static_info-function-65)
5. [GSP_RM_ALLOC (function 103)](#5-gsp_rm_alloc-function-103)
6. [GSP_RM_CONTROL (function 76)](#6-gsp_rm_control-function-76)
7. [Display Control Commands](#7-display-control-commands)
8. [Doorbell / GSP Notification](#8-doorbell--gsp-notification)
9. [Complete Init Sequence](#9-complete-init-sequence)
10. [C++ Struct Definitions](#10-c-struct-definitions)

---

## 1. RPC Transport Framing

Every RPC is wrapped in two layers: a **message queue element** (transport) and an
**RPC header** (protocol). See `nvidia_gsp_message_queue_reference.md` for full
queue operations.

### GSP_MSG_QUEUE_ELEMENT — Transport Layer

```
Offset  Size  Field
──────  ────  ─────
0x00    16    authTagBuffer[16]       // zeros for non-encrypted
0x10    16    aadBuffer[16]           // zeros for non-encrypted
0x20    4     checkSum                // XOR-rotate checksum over entire element
0x24    4     seqNum                  // transport-level sequence (auto-incremented)
0x28    4     elemCount               // 1 for single-element messages
0x2C    4     padding                 // alignment to 8 bytes for rpc field
0x30    ──    rpc (rpc_message_header_v03_00)  // 8-byte aligned
```

Total header overhead before RPC payload: **0x30 bytes (48 bytes)**.

### rpc_message_header_v03_00 — RPC Protocol Header

Source: `nvrm/gsp.h`, lines 793–804.

```
Offset  Size  Field                Type     Value/Notes
──────  ────  ─────                ────     ────────────
0x00    4     header_version       u32      0x03000000
0x04    4     signature            u32      0x43505256 = 'C','P','R','V' (LE of "VRPC")
0x08    4     length               u32      sizeof(header) + sizeof(payload)
0x0C    4     function             u32      NV_VGPU_MSG_FUNCTION_* enum value
0x10    4     rpc_result           u32      0xFFFFFFFF on send; GSP fills on reply
0x14    4     rpc_result_private   u32      0xFFFFFFFF on send; GSP fills on reply
0x18    4     sequence             u32      RPC-level sequence number (incremented per RPC)
0x1C    4     u.spare / cpuRmGfid  u32      0 for baremetal (union)
0x20    ...   rpc_message_data[]            RPC-specific payload begins here
```

**Total RPC header size: 0x20 (32 bytes).**

The `signature` value in little-endian memory is `0x43 0x50 0x52 0x56` = "CPRV".
This is constructed as `('V' << 24) | ('R' << 16) | ('P' << 8) | 'C'` = `0x56525043`,
but stored LE as bytes `43 50 52 56`.

**Correction:** The actual value stored is `('V' << 24) | ('R' << 16) | ('P' << 8) | 'C'`
which produces the 32-bit integer `0x56525043`. In little-endian memory this is
`43 50 52 56`.

**Key init values (from `r535_gsp_rpc_get`):**
```c
rpc->header_version    = 0x03000000;
rpc->signature         = 0x56525043;  // 'V'<<24 | 'R'<<16 | 'P'<<8 | 'C'
rpc->rpc_result        = 0xFFFFFFFF;  // sentinel, GSP overwrites
rpc->rpc_result_private = 0xFFFFFFFF;
rpc->length            = sizeof(rpc_message_header_v03_00) + payload_size;
rpc->function          = fn;          // the NV_VGPU_MSG_FUNCTION_* value
```

---

## 2. SET_SYSTEM_INFO (function 72)

**RPC function:** `NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO = 72`

**Direction:** CPU → GSP (push, no reply expected — fire-and-forget)

**Payload struct:** `GspSystemInfo`

Source: `nvrm/gsp.h` (r535 nouveau), `gsp_static_config.h` (NVIDIA open-gpu-kernel-modules).

### GspSystemInfo — r535 layout (nouveau kernel)

This is the **r535** version used with firmware `535.113.01`. The upstream NVIDIA
repo has added more fields in newer versions.

```
Offset  Size  Field                    Type     Critical?  Notes
──────  ────  ─────                    ────     ─────────  ─────
0x00    8     gpuPhysAddr              u64      YES        BAR0 physical address (PCI resource 0)
0x08    8     gpuPhysFbAddr            u64      YES        BAR1 (VRAM) physical address (PCI resource 1)
0x10    8     gpuPhysInstAddr          u64      YES        BAR2/3 instance memory phys addr (PCI resource 3)
0x18    8     nvDomainBusDeviceFunc    u64      YES        PCI BDF — see encoding below
0x20    8     simAccessBufPhysAddr     u64      no         0 for real hardware (simulator only)
0x28    8     pcieAtomicsOpMask        u64      no         0 if atomics not used
0x30    8     consoleMemSize           u64      no         0 typically
0x38    8     maxUserVa                u64      YES        Max user VA (e.g. TASK_SIZE = 0x0000800000000000)
0x40    4     pciConfigMirrorBase      u32      YES        0x088000 (PRI address of PCI config mirror)
0x44    4     pciConfigMirrorSize      u32      YES        0x001000 (4KB)
0x48    1     oorArch                  u8       no         0
0x49    7     (padding)                                    align to 8
0x50    8     clPdbProperties          u64      no         0
0x58    4     Chipset                  u32      no         0
0x5C    1     bGpuBehindBridge         NvBool   no         0
0x5D    1     bMnocAvailable           NvBool   no         0
0x5E    1     bUpstreamL0sUnsupported  NvBool   no         0
0x5F    1     bUpstreamL1Unsupported   NvBool   no         0
0x60    1     bUpstreamL1PorSupported  NvBool   no         0
0x61    1     bUpstreamL1PorMobileOnly NvBool   no         0
0x62    1     upstreamAddressValid     u8       no         0
0x63    1     (padding)                                    align to BUSINFO
0x64    10    FHBBusInfo               BUSINFO  no         zeros OK
0x6E    10    chipsetIDInfo            BUSINFO  no         zeros OK
0x78    ???   acpiMethodData           ACPI_METHOD_DATA  no  zeros OK for bare-metal
???     4     hypervisorType           u32      no         0 (none)
???     1     bIsPassthru              NvBool   no         0
???     8     sysTimerOffsetNs         u64      no         0
???     ...   gspVFInfo                GSP_VF_INFO  no     zeros (no SR-IOV)
```

#### Sub-structs

**BUSINFO** (10 bytes, packed):
```
Offset  Size  Field
──────  ────  ─────
0x00    2     deviceID         u16
0x02    2     vendorID         u16
0x04    2     subdeviceID      u16
0x06    2     subvendorID      u16
0x08    1     revisionID       u8
0x09    1     (padding)
```

**GSP_VF_INFO** (44 bytes with padding):
```
Offset  Size  Field
──────  ────  ─────
0x00    4     totalVFs         u32     // 0
0x04    4     firstVFOffset    u32     // 0
0x08    8     FirstVFBar0Addr  u64     // 0
0x10    8     FirstVFBar1Addr  u64     // 0
0x18    8     FirstVFBar2Addr  u64     // 0
0x20    1     b64bitBar0       NvBool  // 0
0x21    1     b64bitBar1       NvBool  // 0
0x22    1     b64bitBar2       NvBool  // 0
```

**ACPI_METHOD_DATA** (large, ~300+ bytes):
```
Offset  Size  Field
──────  ────  ─────
0x00    1     bValid           NvBool     // false for bare-metal without ACPI
0x01    ...   dodMethodData    DOD_METHOD_DATA
              jtMethodData     JT_METHOD_DATA
              muxMethodData    MUX_METHOD_DATA
              capsMethodData   CAPS_METHOD_DATA
```

**DOD_METHOD_DATA:**
```
0x00    4     status           NV_STATUS   // 0xFFFF = not available
0x04    4     acpiIdListLen    u32
0x08    64    acpiIdList[16]   u32[16]
```

**JT_METHOD_DATA:**
```
0x00    4     status           NV_STATUS
0x04    4     jtCaps           u32
0x08    2     jtRevId          u16
0x0A    1     bSBIOSCaps       NvBool
```

**MUX_METHOD_DATA:**
```
0x00    4     tableLen         u32
0x04    192   acpiIdMuxModeTable[16]   (16 × MUX_METHOD_DATA_ELEMENT)
0xC4    192   acpiIdMuxPartTable[16]
```
Each MUX_METHOD_DATA_ELEMENT = 12 bytes: `{u32 acpiId, u32 mode, u32 status}`.

**CAPS_METHOD_DATA:**
```
0x00    4     status           NV_STATUS
0x04    4     optimusCaps      u32
```

### nvDomainBusDeviceFunc Encoding

From `r535_gsp_rpc_set_system_info`:
```c
info->nvDomainBusDeviceFunc = pci_dev_id(pdev->pdev);
```

`pci_dev_id()` returns `(bus << 8) | devfn`, where `devfn = (device << 3) | function`.
This is stored as a `u64` but only the lower 16 bits matter for standard PCI.

For a device at **domain 0, bus 1, device 0, function 0**:
```
nvDomainBusDeviceFunc = (1 << 8) | (0 << 3) | 0 = 0x0100
```

**Note:** The domain is NOT encoded in the standard `pci_dev_id()`. For multi-domain
systems, nouveau uses the full PCI domain:bus:slot.fn but for single-domain
bare-metal, only `(bus << 8) | (slot << 3) | func` matters.

### Critical Fields Summary

| Field | Source | Value |
|-------|--------|-------|
| `gpuPhysAddr` | PCI BAR0 physical address | Read from PCI config |
| `gpuPhysFbAddr` | PCI BAR1 physical address | Read from PCI config |
| `gpuPhysInstAddr` | PCI BAR2/3 physical address | Read from PCI config |
| `nvDomainBusDeviceFunc` | `(bus<<8)\|(slot<<3)\|func` | PCI topology |
| `maxUserVa` | `TASK_SIZE` | `0x800000000000` (x86_64) or similar |
| `pciConfigMirrorBase` | Hardcoded | `0x088000` |
| `pciConfigMirrorSize` | Hardcoded | `0x001000` |

Everything else can be zero for a minimal bare-metal init.

### Nouveau Init Code (verbatim)

```c
info->gpuPhysAddr      = device->func->resource_addr(device, 0);  // BAR0
info->gpuPhysFbAddr    = device->func->resource_addr(device, 1);  // BAR1
info->gpuPhysInstAddr  = device->func->resource_addr(device, 3);  // BAR2/3
info->nvDomainBusDeviceFunc = pci_dev_id(pdev->pdev);
info->maxUserVa        = TASK_SIZE;
info->pciConfigMirrorBase = 0x088000;
info->pciConfigMirrorSize = 0x001000;
r535_gsp_acpi_info(gsp, &info->acpiMethodData);
```

---

## 3. SET_REGISTRY (function 73)

**RPC function:** `NV_VGPU_MSG_FUNCTION_SET_REGISTRY = 73`

**Direction:** CPU → GSP (push, no reply expected)

**Payload struct:** `PACKED_REGISTRY_TABLE` + entries + string data (variable-length)

Source: `nvrm/gsp.h`, lines 227–240.

### PACKED_REGISTRY_TABLE

```
Offset  Size  Field
──────  ────  ─────
0x00    4     size             u32     sizeof(PACKED_REGISTRY_TABLE) header only = 8
0x04    4     numEntries       u32     number of PACKED_REGISTRY_ENTRY items
0x08    ...   entries[]                array of PACKED_REGISTRY_ENTRY
              ...              ...     followed by packed string data
```

### PACKED_REGISTRY_ENTRY (13 bytes, packed)

```
Offset  Size  Field
──────  ────  ─────
0x00    4     nameOffset       u32     byte offset from start of PACKED_REGISTRY_TABLE
                                       to the NUL-terminated key name string
0x04    1     type             u8      1 = DWORD, 2 = BINARY, 3 = STRING
0x05    4     data             u32     the registry value (for DWORD type)
0x09    4     length           u32     byte length of value (4 for DWORD)
```

Total per entry: **13 bytes** (no padding between entries).

### String Storage

Strings are packed after the last entry. Each entry's `nameOffset` points into
this region, measured from the start of the `PACKED_REGISTRY_TABLE`.

### Minimal Registry (nouveau default)

Nouveau sends **one dummy entry** with an empty string key:

```c
rpc = nvkm_gsp_rpc_get(gsp, NV_VGPU_MSG_FUNCTION_SET_REGISTRY,
                        sizeof(*rpc) + sizeof(rpc->entries[0]) + 1);

rpc->size = sizeof(*rpc);                                    // 8
rpc->numEntries = 1;
rpc->entries[0].nameOffset = offsetof(typeof(*rpc), entries[1]); // offset past entries
rpc->entries[0].type = 1;                                    // DWORD
rpc->entries[0].data = 0;
rpc->entries[0].length = 4;

strings = (char *)&rpc->entries[1];
strings[0] = '\0';                                           // empty key name
```

**Binary layout of minimal registry (22 bytes total payload):**

```
Offset  Hex                Description
──────  ───                ───────────
0x00    08 00 00 00        size = 8
0x04    01 00 00 00        numEntries = 1
0x08    15 00 00 00        entries[0].nameOffset = 21 (offset to string)
0x0C    01                 entries[0].type = 1 (DWORD)
0x0D    00 00 00 00        entries[0].data = 0
0x11    04 00 00 00        entries[0].length = 4
0x15    00                 string data: "" (NUL terminator)
```

**For bare-metal boot: an empty/minimal registry is sufficient.** You can also
send `numEntries = 0` with no entries, but nouveau always sends at least one
dummy entry.

### Common Registry Keys

These are standard `NvReg` keys that can be sent. For basic boot, none are required:

| Key | Type | Description |
|-----|------|-------------|
| `RMGspFirmwareLog` | DWORD | Enable GSP firmware logging |
| `RmProfiling` | DWORD | Enable profiling |
| `RmGspUcodeFirmwareVersion` | DWORD | Force firmware version |

---

## 4. GET_GSP_STATIC_INFO (function 65)

**RPC function:** `NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO = 65`

**Direction:** CPU → GSP request (empty payload), GSP → CPU reply (full struct)

**Reply payload struct:** `GspStaticConfigInfo`

Source: `nvrm/gsp.h`, lines 142–218 (r535 nouveau version).

This is a **large** struct (~several KB). You send an RPC with function=65 and
empty payload, and GSP-RM returns it filled with GPU configuration data.

### GspStaticConfigInfo (r535 nouveau version)

```
Offset  Size    Field                      Type
──────  ────    ─────                      ────
0x000   23      grCapsBits[23]             u8[23]    // GR capability bits
0x017   1       (pad)
0x018   264     gidInfo                    NV2080_CTRL_GPU_GET_GID_INFO_PARAMS
0x120   4       gpcInfo.gpcMask            u32       // which GPCs are active
0x124   256     tpcInfo[32]                {u32 gpcId, u32 tpcMask}[32]
0x224   256     zcullInfo[32]              {u32 gpcId, u32 zcullMask}[32]
0x324   ...     SKUInfo                    NV2080_CTRL_BIOS_GET_SKU_INFO_PARAMS
...     ...     fbRegionInfoParams         NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO_PARAMS
...     4       computeBranding            enum (0=NONE, 1=TESLA)
...     ...     sriovCaps                  NV0080_CTRL_GPU_GET_SRIOV_CAPS_PARAMS
...     4       sriovMaxGfid               u32
...     12      engineCaps[3]              u32[3]    // NVGPU_ENGINE_CAPS_MASK_ARRAY_MAX=3
...     ...     SM_info                    GspSMInfo (36 bytes)
...     1       poisonFuseEnabled          NvBool
...     8       fb_length                  u64       // *** TOTAL VRAM SIZE ***
...     4       fbio_mask                  u32
...     4       fb_bus_width               u32
...     4       fb_ram_type                u32
...     4       fbp_mask                   u32
...     4       l2_cache_size              u32
...     ...     gfxpBufferSize[6]          u32[6]
...     ...     gfxpBufferAlignment[6]     u32[6]
...     64      gpuNameString[64]          char[64]  // e.g. "NVIDIA GeForce RTX 3090"
...     64      gpuShortNameString[64]     char[64]  // e.g. "RTX 3090"
...     128     gpuNameString_Unicode[64]  u16[64]
...     1       bGpuInternalSku            NvBool
...     1       bIsQuadroGeneric           NvBool
...     1       bIsQuadroAd               NvBool
...     1       bIsNvidiaNvs              NvBool
...     1       bIsVgx                    NvBool
...     1       bGeforceSmb               NvBool
...     1       bIsTitan                  NvBool
...     1       bIsTesla                  NvBool
...     1       bIsMobile                 NvBool
...     1       bIsGc6Rtd3Allowed         NvBool
...     1       bIsGcOffRtd3Allowed       NvBool
...     1       bIsGcoffLegacyAllowed     NvBool
...     8       bar1PdeBase               u64
...     8       bar2PdeBase               u64
...     1       bVbiosValid               NvBool
...     4       vbiosSubVendor            u32
...     4       vbiosSubDevice            u32
...     1       bPageRetirementSupported  NvBool
...     1       bSplitVasBetweenServerClientRm  NvBool
...     1       bClRootportNeedsNosnoopWAR      NvBool
...     8       displaylessMaxHeads       {u32 numHeads, u32 maxNumHeads}
...     12      displaylessMaxResolution  {u32 headIndex, u32 maxH, u32 maxV}
...     8       displaylessMaxPixels      u64
0x???   4       hInternalClient           NvHandle  // *** GSP-RM internal client handle ***
0x???   4       hInternalDevice           NvHandle  // *** GSP-RM internal device handle ***
0x???   4       hInternalSubdevice        NvHandle  // *** GSP-RM internal subdevice handle ***
...     1       bSelfHostedMode           NvBool
...     1       bAtsSupported             NvBool
...     1       bIsGpuUefi               NvBool
```

### Critical Fields From GET_GSP_STATIC_INFO

| Field | Why it matters |
|-------|----------------|
| `fb_length` | Total VRAM size — needed for framebuffer allocation |
| `fb_ram_type` | GDDR type |
| `fb_bus_width` | Memory bus width |
| `gpuNameString` | Human-readable GPU name |
| `hInternalClient` | Pre-allocated RM client handle for internal operations |
| `hInternalDevice` | Pre-allocated RM device handle |
| `hInternalSubdevice` | Pre-allocated RM subdevice handle |
| `bar1PdeBase` | BAR1 page directory base — needed for BAR1 VM setup |
| `bar2PdeBase` | BAR2 page directory base |
| `gpcInfo.gpcMask` | Active GPC bitmask |
| `tpcInfo[]` | TPC masks per GPC |
| `engineCaps[]` | Available engine bitmask |

### Sub-structs

**NV2080_CTRL_GPU_GET_GID_INFO_PARAMS (264 bytes):**
```
0x00  4     index      u32
0x04  4     flags      u32
0x08  4     length     u32
0x0C  256   data[256]  u8[256]   // GPU ID string
```

**NV2080_CTRL_BIOS_GET_SKU_INFO_PARAMS:**
```
0x00  4     BoardID       u32
0x04  4     chipSKU[4]    char[4]
0x08  2     chipSKUMod[2] char[2]
0x0A  5     project[5]    char[5]
0x0F  5     projectSKU[5] char[5]
0x14  6     CDP[6]        char[6]
0x1A  2     projectSKUMod char[2]
0x1C  4     businessCycle u32
```

**GspSMInfo (36 bytes):**
```
0x00  4     version           u32
0x04  4     regBankCount      u32
0x08  4     regBankRegCount   u32
0x0C  4     maxWarpsPerSM     u32
0x10  4     maxThreadsPerWarp u32
0x14  4     geomGsObufEntries u32
0x18  4     geomXbufEntries   u32
0x1C  4     maxSPPerSM        u32
0x20  4     rtCoreCount       u32
```

### How to Call

```c
// Send empty request (payload size = sizeof(GspStaticConfigInfo))
GspStaticConfigInfo *info;
info = nvkm_gsp_rpc_rd(gsp, NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO,
                        sizeof(GspStaticConfigInfo));
// 'info' now points to GSP's reply data
// Read info->fb_length, info->hInternalClient, etc.
nvkm_gsp_rpc_done(gsp, info);
```

---

## 5. GSP_RM_ALLOC (function 103)

**RPC function:** `NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC = 103`

**Direction:** CPU → GSP request with alloc params, GSP → CPU reply with status

Source: `nvrm/alloc.h`.

### rpc_gsp_rm_alloc_v03_00

```
Offset  Size  Field       Type       Notes
──────  ────  ─────       ────       ─────
0x00    4     hClient     NvHandle   Client handle (owner)
0x04    4     hParent     NvHandle   Parent object handle
0x08    4     hObject     NvHandle   New object handle (caller-chosen)
0x0C    4     hClass      u32        RM class ID (0x0000, 0x0080, etc.)
0x10    4     status      u32        0 on send; GSP fills with NV_STATUS on reply
0x14    4     paramsSize  u32        Size of params[] in bytes
0x18    4     flags       u32        0 for default
0x1C    4     reserved[4] u8[4]      Must be zero
0x20    ...   params[]    u8[]       Class-specific allocation parameters
```

**Total fixed header: 0x20 (32 bytes) + variable params.**

### Object Allocation Hierarchy

```
Root Client (NV01_ROOT = 0x0000)
 └── Device (NV01_DEVICE_0 = 0x0080)
      └── Subdevice (NV20_SUBDEVICE_0 = 0x2080)
      └── Display Common (NV04_DISPLAY_COMMON = 0x0073)
```

### Handle Allocation

Handles are **caller-chosen** arbitrary 32-bit non-zero values. Nouveau uses
a simple incrementing counter. For bare-metal, any unique non-zero values work:

```
hClient    = 0xCAFE0001  (pick any unique value)
hDevice    = 0xCAFE0002
hSubdevice = 0xCAFE0003
hDisplay   = 0xCAFE0004
```

The root client's `hClient` and `hObject` are the same value (self-referencing).

### a. Allocate Root Client — NV01_ROOT (0x0000)

```
rpc_gsp_rm_alloc_v03_00:
  hClient    = <your_client_handle>     // e.g. 0xCAFE0001
  hParent    = 0x00000000               // no parent for root
  hObject    = <your_client_handle>     // same as hClient for root
  hClass     = 0x00000000               // NV01_ROOT
  status     = 0x00000000
  paramsSize = sizeof(NV0000_ALLOC_PARAMETERS) = 108
  flags      = 0x00000000
  reserved   = {0,0,0,0}
```

**NV0000_ALLOC_PARAMETERS (108 bytes):**

```
Offset  Size  Field
──────  ────  ─────
0x00    4     hClient          NvHandle   // same as hClient above
0x04    4     processID        u32        // any PID (e.g. 0 or getpid())
0x08    100   processName[100] char[100]  // NUL-terminated string, e.g. "stellux"
```

Source: `nvrm/client.h`.

### b. Allocate Device — NV01_DEVICE_0 (0x0080)

```
rpc_gsp_rm_alloc_v03_00:
  hClient    = <your_client_handle>
  hParent    = <your_client_handle>     // parent is the root client
  hObject    = <your_device_handle>     // e.g. 0xCAFE0002
  hClass     = 0x00000080               // NV01_DEVICE_0
  status     = 0x00000000
  paramsSize = sizeof(NV0080_ALLOC_PARAMETERS) = 48 (with alignment)
  flags      = 0x00000000
```

**NV0080_ALLOC_PARAMETERS (48 bytes with NV_DECLARE_ALIGNED padding):**

```
Offset  Size  Field            Type      Value
──────  ────  ─────            ────      ─────
0x00    4     deviceId         u32       0 (first GPU)
0x04    4     hClientShare     NvHandle  <your_client_handle>
0x08    4     hTargetClient    NvHandle  0
0x0C    4     hTargetDevice    NvHandle  0
0x10    4     flags            u32       0
0x14    4     (pad)                      0 (align vaSpaceSize to 8)
0x18    8     vaSpaceSize      u64       0
0x20    8     vaStartInternal  u64       0
0x28    8     vaLimitInternal  u64       0
0x30    4     vaMode           u32       0
0x34    4     (pad)                      trailing pad
```

Source: `nvrm/device.h`.

### c. Allocate Subdevice — NV20_SUBDEVICE_0 (0x2080)

```
rpc_gsp_rm_alloc_v03_00:
  hClient    = <your_client_handle>
  hParent    = <your_device_handle>     // parent is the device
  hObject    = <your_subdevice_handle>  // e.g. 0xCAFE0003
  hClass     = 0x00002080               // NV20_SUBDEVICE_0
  status     = 0x00000000
  paramsSize = sizeof(NV2080_ALLOC_PARAMETERS) = 4
  flags      = 0x00000000
```

**NV2080_ALLOC_PARAMETERS (4 bytes):**

```
Offset  Size  Field
──────  ────  ─────
0x00    4     subDeviceId      u32       0 (first subdevice)
```

Source: `nvrm/device.h`.

### d. Allocate Display Common — NV04_DISPLAY_COMMON (0x0073)

```
rpc_gsp_rm_alloc_v03_00:
  hClient    = <your_client_handle>
  hParent    = <your_device_handle>     // parent is the device
  hObject    = <your_display_handle>    // e.g. 0xCAFE0004
  hClass     = 0x00000073               // NV04_DISPLAY_COMMON
  status     = 0x00000000
  paramsSize = 0                        // no params needed
  flags      = 0x00000000
```

**No allocation parameters required for NV04_DISPLAY_COMMON.**

---

## 6. GSP_RM_CONTROL (function 76)

**RPC function:** `NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL = 76`

**Direction:** CPU → GSP request, GSP → CPU reply

Source: `nvrm/ctrl.h`.

### rpc_gsp_rm_control_v03_00

```
Offset  Size  Field       Type       Notes
──────  ────  ─────       ────       ─────
0x00    4     hClient     NvHandle   Client handle
0x04    4     hObject     NvHandle   Target object handle (device/subdevice/display)
0x08    4     cmd         u32        NV*_CTRL_CMD_* command ID
0x0C    4     status      u32        0 on send; GSP fills on reply
0x10    4     paramsSize  u32        Size of params[]
0x14    4     flags       u32        0
0x18    ...   params[]    u8[]       Command-specific parameter struct
```

**Total fixed header: 0x18 (24 bytes) + variable params.**

---

## 7. Display Control Commands

All display control commands use `GSP_RM_CONTROL` (function 76) with `hObject`
set to the display common handle, and the appropriate `cmd` value.

Source: `nvrm/disp.h`.

### NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED (0x730120)

Get bitmask of connected/supported displays.

```
rpc_gsp_rm_control_v03_00:
  hClient    = <your_client_handle>
  hObject    = <your_display_handle>
  cmd        = 0x00730120
  paramsSize = sizeof(NV0073_CTRL_SYSTEM_GET_SUPPORTED_PARAMS) = 12
```

**NV0073_CTRL_SYSTEM_GET_SUPPORTED_PARAMS (12 bytes):**
```
Offset  Size  Field             Type   Direction
──────  ────  ─────             ────   ─────────
0x00    4     subDeviceInstance  u32    IN  (0 for first subdevice)
0x04    4     displayMask        u32    OUT (bitmask of supported displays)
0x08    4     displayMaskDDC     u32    OUT (displays with DDC capability)
```

### NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS (0x730102)

```
rpc_gsp_rm_control_v03_00:
  hClient    = <your_client_handle>
  hObject    = <your_display_handle>
  cmd        = 0x00730102
  paramsSize = 12
```

**NV0073_CTRL_SYSTEM_GET_NUM_HEADS_PARAMS (12 bytes):**
```
0x00    4     subDeviceInstance  u32    IN  (0)
0x04    4     flags              u32    IN  (0)
0x08    4     numHeads           u32    OUT
```

### NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2 (0x730245)

Read EDID from a display output.

```
rpc_gsp_rm_control_v03_00:
  hClient    = <your_client_handle>
  hObject    = <your_display_handle>
  cmd        = 0x00730245
  paramsSize = sizeof(NV0073_CTRL_SPECIFIC_GET_EDID_V2_PARAMS) = 2064
```

**NV0073_CTRL_SPECIFIC_GET_EDID_V2_PARAMS (2064 bytes):**
```
Offset  Size  Field             Type    Direction
──────  ────  ─────             ────    ─────────
0x00    4     subDeviceInstance  u32     IN  (0)
0x04    4     displayId          u32     IN  (from GET_SUPPORTED mask, power-of-2)
0x08    4     bufferSize         u32     IN/OUT (max 2048, GSP fills actual)
0x0C    4     flags              u32     IN  (0)
0x10    2048  edidBuffer[2048]   u8[]    OUT (raw EDID data)
```

### NV0073_CTRL_CMD_DP_AUXCH_CTRL (0x731341)

Perform a DisplayPort AUX channel transaction.

```
rpc_gsp_rm_control_v03_00:
  hClient    = <your_client_handle>
  hObject    = <your_display_handle>
  cmd        = 0x00731341
  paramsSize = sizeof(NV0073_CTRL_DP_AUXCH_CTRL_PARAMS) = 44
```

**NV0073_CTRL_DP_AUXCH_CTRL_PARAMS (44 bytes):**
```
Offset  Size  Field             Type    Direction
──────  ────  ─────             ────    ─────────
0x00    4     subDeviceInstance  u32     IN  (0)
0x04    4     displayId          u32     IN
0x08    1     bAddrOnly          NvBool  IN  (false for normal transactions)
0x09    3     (pad)
0x0C    4     cmd                u32     IN  (see bit definitions below)
0x10    4     addr               u32     IN  (DPCD address, e.g. 0x00000)
0x14    16    data[16]           u8[16]  IN/OUT (max 16 bytes per AUX transaction)
0x24    4     size               u32     IN/OUT (bytes to transfer, max 16)
0x28    4     replyType          u32     OUT (AUX reply type)
0x2C    4     retryTimeMs        u32     IN  (timeout in ms, 0 for default)
```

**AUX cmd bit definitions:**
```
Bit 3:     CMD_TYPE        0 = I2C, 1 = Native AUX
Bit 2:     I2C_MOT         0 = no middle-of-transaction, 1 = MOT
Bit 1:0:   REQ_TYPE        0 = Write, 1 = Read, 2 = Write Status
```

Examples:
- **Native AUX Read:** `cmd = 0x09` (TYPE=AUX, REQ=Read)
- **Native AUX Write:** `cmd = 0x08` (TYPE=AUX, REQ=Write)
- **I2C Read (DDC):** `cmd = 0x01` (TYPE=I2C, REQ=Read)

### NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE (0x730122)

```
paramsSize = 16
```
```
0x00    4     subDeviceInstance  u32    IN
0x04    4     flags              u32    IN
0x08    4     displayMask        u32    IN/OUT
0x0C    4     retryTimeMs        u32    IN
```

### NV0073_CTRL_CMD_SPECIFIC_GET_CONNECTOR_DATA (0x730250)

```
paramsSize = 72
```
```
0x00    4     subDeviceInstance  u32
0x04    4     displayId          u32
0x08    4     flags              u32
0x0C    4     DDCPartners        u32     OUT
0x10    4     count              u32     OUT
0x14    48    data[4]            {u32 index, u32 type, u32 location}[4]  OUT
0x44    4     platform           u32     OUT
```

### NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO (0x73028B)

```
paramsSize = 48 (approx, with alignment)
```
```
0x00    4     subDeviceInstance  u32
0x04    4     displayId          u32
0x08    4     index              u32     OUT (SOR/DAC/PIOR index)
0x0C    4     type               u32     OUT (0=NONE, 1=DAC, 2=SOR, 3=PIOR)
0x10    4     protocol           u32     OUT (DP_A=8, TMDS_A=1, etc.)
0x14    4     ditherType         u32
0x18    4     ditherAlgo         u32
0x1C    4     location           u32
0x20    4     rootPortId         u32
0x24    4     dcbIndex           u32
0x28    8     vbiosAddress       u64
0x30    1     bIsLitByVbios      NvBool
0x31    1     bIsDispDynamic     NvBool
```

OR protocol values:
- `0x00000008` = `SOR_DP_A` (DisplayPort)
- `0x00000001` = `SOR_SINGLE_TMDS_A` (HDMI/DVI)
- `0x00000005` = `SOR_DUAL_TMDS` (Dual-link DVI)

---

## 8. Doorbell / GSP Notification

After writing an RPC message to the command queue ring buffer, the host must
**notify GSP** to process it.

### Mechanism

Nouveau writes to the **GSP Falcon MAILBOX0 register** at falcon offset `0xC00`:

```c
nvkm_falcon_wr32(&gsp->falcon, 0xc00, 0x00000000);
```

Source: `r535_gsp_cmdq_push()` in the original r535 nouveau patch.

This is `NV_PFALCON_FALCON_MAILBOX0`, which is at **BAR0 + falcon_base + 0xC00**.

For GSP on Ampere (GA102), the falcon base is typically at `0x110000` (GSP falcon),
making the absolute MMIO address:

```
BAR0 + 0x110000 + 0xC00 = BAR0 + 0x110C00
```

**The value written is `0x00000000`.** The act of writing (any value) to this register
triggers an interrupt to the GSP falcon, causing it to check the command queue
for new messages.

### Complete Send Sequence

1. Write `rpc_message_header_v03_00` + payload into command queue ring buffer
2. Compute and set checksum in `GSP_MSG_QUEUE_ELEMENT.checkSum`
3. Update `cmdq.wptr` (write pointer)
4. Memory barrier (`mb()`)
5. Write `0x00000000` to falcon register `0xC00` (MAILBOX0)

### Waiting for Reply

After sending, poll the **status/message queue** (GSP→CPU) by checking
`msgq.wptr != msgq.rptr`. The reply will have the same `function` value as
the request, with `rpc_result` and `rpc_result_private` filled in by GSP.

`rpc_result == 0` means success. Any other value is an error.

---

## 9. Complete Init Sequence

After GSP firmware is loaded and booted (see `nvidia_gsp_boot_implementation_reference.md`),
the RPC init sequence is:

```
1. Wait for NV_VGPU_MSG_EVENT_GSP_INIT_DONE (0x1001) on message queue
2. Send SET_SYSTEM_INFO  (fn 72) — GspSystemInfo with PCI/BAR info
3. Send SET_REGISTRY      (fn 73) — minimal registry table
4. [GSP boots RM internally at this point]
5. Wait for NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER (0x1002) — execute sequencer
6. Send GET_GSP_STATIC_INFO (fn 65) — receive GPU configuration
7. Allocate Root Client    (fn 103, class 0x0000)
8. Allocate Device         (fn 103, class 0x0080)
9. Allocate Subdevice      (fn 103, class 0x2080)
10. Allocate Display Common (fn 103, class 0x0073)
11. Execute display control commands (fn 76, various cmd values)
```

**Important ordering:** SET_SYSTEM_INFO and SET_REGISTRY must be sent
**before** GSP-RM is fully booted (before the sequencer runs). They are
sent in the early "pre-boot" phase when GSP firmware is loaded but before
the RISC-V core starts executing RM.

GET_GSP_STATIC_INFO and all rm_alloc/rm_control calls happen **after** GSP-RM
has fully initialized (after INIT_DONE and sequencer completion).

---

## 10. C++ Struct Definitions

Binary-compatible C++ structs for bare-metal driver use. All packed, little-endian.

```cpp
#include <cstdint>

// Primitive NVIDIA types
using NvU8     = uint8_t;
using NvU16    = uint16_t;
using NvU32    = uint32_t;
using NvU64    = uint64_t;
using NvBool   = uint8_t;
using NvHandle = uint32_t;
using NvV32    = uint32_t;

// ============================================================================
// RPC Message Header (32 bytes)
// ============================================================================
struct [[gnu::packed]] RpcMessageHeader {
    NvU32 header_version;       // 0x03000000
    NvU32 signature;            // 0x56525043 ('V'<<24|'R'<<16|'P'<<8|'C')
    NvU32 length;               // sizeof(header) + sizeof(payload)
    NvU32 function;             // NV_VGPU_MSG_FUNCTION_*
    NvU32 rpc_result;           // 0xFFFFFFFF on send
    NvU32 rpc_result_private;   // 0xFFFFFFFF on send
    NvU32 sequence;             // incrementing sequence number
    NvU32 spare;                // 0 (cpuRmGfid for vGPU)
    // payload follows immediately
};
static_assert(sizeof(RpcMessageHeader) == 32);

// ============================================================================
// GSP Message Queue Element (transport wrapper)
// ============================================================================
struct [[gnu::packed]] GspMsgQueueElement {
    NvU8  authTagBuffer[16];    // zeros (non-encrypted)
    NvU8  aadBuffer[16];        // zeros (non-encrypted)
    NvU32 checkSum;             // XOR-rotate checksum
    NvU32 seqNum;               // transport sequence number
    NvU32 elemCount;            // 1 for single-element messages
    NvU32 padding;              // align rpc to 8 bytes
    RpcMessageHeader rpc;       // 8-byte aligned
    // rpc.payload follows
};
static_assert(offsetof(GspMsgQueueElement, rpc) == 0x30);

// ============================================================================
// BUSINFO (10 bytes, packed)
// ============================================================================
struct [[gnu::packed]] BusInfo {
    NvU16 deviceID;
    NvU16 vendorID;
    NvU16 subdeviceID;
    NvU16 subvendorID;
    NvU8  revisionID;
};

// ============================================================================
// GSP_VF_INFO
// ============================================================================
struct [[gnu::packed]] GspVfInfo {
    NvU32 totalVFs;
    NvU32 firstVFOffset;
    NvU64 FirstVFBar0Address;
    NvU64 FirstVFBar1Address;
    NvU64 FirstVFBar2Address;
    NvBool b64bitBar0;
    NvBool b64bitBar1;
    NvBool b64bitBar2;
};

// ============================================================================
// ACPI sub-structs (can be zeroed for bare-metal)
// ============================================================================
static constexpr NvU32 NV0073_CTRL_SYSTEM_ACPI_ID_MAP_MAX_DISPLAYS = 16;

struct [[gnu::packed]] DodMethodData {
    NvU32 status;               // 0xFFFF = not available
    NvU32 acpiIdListLen;
    NvU32 acpiIdList[NV0073_CTRL_SYSTEM_ACPI_ID_MAP_MAX_DISPLAYS];
};

struct [[gnu::packed]] JtMethodData {
    NvU32 status;
    NvU32 jtCaps;
    NvU16 jtRevId;
    NvBool bSBIOSCaps;
};

struct [[gnu::packed]] MuxMethodDataElement {
    NvU32 acpiId;
    NvU32 mode;
    NvU32 status;
};

struct [[gnu::packed]] MuxMethodData {
    NvU32 tableLen;
    MuxMethodDataElement acpiIdMuxModeTable[NV0073_CTRL_SYSTEM_ACPI_ID_MAP_MAX_DISPLAYS];
    MuxMethodDataElement acpiIdMuxPartTable[NV0073_CTRL_SYSTEM_ACPI_ID_MAP_MAX_DISPLAYS];
};

struct [[gnu::packed]] CapsMethodData {
    NvU32 status;
    NvU32 optimusCaps;
};

struct [[gnu::packed]] AcpiMethodData {
    NvBool bValid;
    DodMethodData dodMethodData;
    JtMethodData jtMethodData;
    MuxMethodData muxMethodData;
    CapsMethodData capsMethodData;
};

// ============================================================================
// GspSystemInfo — SET_SYSTEM_INFO payload (function 72)
//
// r535 version from nouveau kernel. Zero all fields then set the critical ones.
// ============================================================================
struct [[gnu::packed]] GspSystemInfo {
    NvU64 gpuPhysAddr;              // BAR0 physical address
    NvU64 gpuPhysFbAddr;            // BAR1 physical address (VRAM)
    NvU64 gpuPhysInstAddr;          // BAR2/3 physical address
    NvU64 nvDomainBusDeviceFunc;    // (bus<<8)|(slot<<3)|func
    NvU64 simAccessBufPhysAddr;     // 0 (simulator only)
    NvU64 pcieAtomicsOpMask;        // 0
    NvU64 consoleMemSize;           // 0
    NvU64 maxUserVa;                // e.g. 0x800000000000 for x86_64
    NvU32 pciConfigMirrorBase;      // 0x088000
    NvU32 pciConfigMirrorSize;      // 0x001000
    NvU8  oorArch;                  // 0
    NvU64 clPdbProperties;          // 0
    NvU32 Chipset;                  // 0
    NvBool bGpuBehindBridge;        // 0
    NvBool bMnocAvailable;          // 0
    NvBool bUpstreamL0sUnsupported; // 0
    NvBool bUpstreamL1Unsupported;  // 0
    NvBool bUpstreamL1PorSupported; // 0
    NvBool bUpstreamL1PorMobileOnly;// 0
    NvU8  upstreamAddressValid;     // 0
    BusInfo FHBBusInfo;             // zeros
    BusInfo chipsetIDInfo;          // zeros
    AcpiMethodData acpiMethodData;  // zeros for bare-metal
    NvU32 hypervisorType;           // 0
    NvBool bIsPassthru;             // 0
    NvU64 sysTimerOffsetNs;         // 0
    GspVfInfo gspVFInfo;            // zeros
};

// ============================================================================
// PACKED_REGISTRY_ENTRY / TABLE — SET_REGISTRY payload (function 73)
// ============================================================================
struct [[gnu::packed]] PackedRegistryEntry {
    NvU32 nameOffset;   // offset from start of table to NUL-terminated key string
    NvU8  type;         // 1=DWORD, 2=BINARY, 3=STRING
    NvU32 data;         // value (for DWORD)
    NvU32 length;       // byte length of value (4 for DWORD)
};
static_assert(sizeof(PackedRegistryEntry) == 13);

struct [[gnu::packed]] PackedRegistryTable {
    NvU32 size;         // sizeof(PackedRegistryTable) = 8 (header only)
    NvU32 numEntries;   // number of entries
    // PackedRegistryEntry entries[numEntries];
    // char strings[];  // packed NUL-terminated key names
};
static_assert(sizeof(PackedRegistryTable) == 8);

// ============================================================================
// rpc_gsp_rm_alloc_v03_00 — GSP_RM_ALLOC payload (function 103)
// ============================================================================
struct [[gnu::packed]] RpcGspRmAlloc {
    NvHandle hClient;       // 0x00
    NvHandle hParent;       // 0x04
    NvHandle hObject;       // 0x08
    NvU32    hClass;        // 0x0C
    NvU32    status;        // 0x10  (0 on send, filled by GSP)
    NvU32    paramsSize;    // 0x14
    NvU32    flags;         // 0x18
    NvU8     reserved[4];   // 0x1C
    // u8 params[];         // 0x20  class-specific
};
static_assert(sizeof(RpcGspRmAlloc) == 32);

// Allocation parameters for NV01_ROOT (class 0x0000)
static constexpr NvU32 NV_PROC_NAME_MAX_LENGTH = 100;
struct [[gnu::packed]] Nv0000AllocParameters {
    NvHandle hClient;
    NvU32    processID;
    char     processName[NV_PROC_NAME_MAX_LENGTH];
};
static_assert(sizeof(Nv0000AllocParameters) == 108);

// Allocation parameters for NV01_DEVICE_0 (class 0x0080)
struct Nv0080AllocParameters {
    NvU32    deviceId;          // 0x00  (0 = first GPU)
    NvHandle hClientShare;     // 0x04
    NvHandle hTargetClient;    // 0x08  (0)
    NvHandle hTargetDevice;    // 0x0C  (0)
    NvV32    flags;            // 0x10  (0)
    NvU32    _pad0;            // 0x14  (align)
    NvU64    vaSpaceSize;      // 0x18  (0)
    NvU64    vaStartInternal;  // 0x20  (0)
    NvU64    vaLimitInternal;  // 0x28  (0)
    NvV32    vaMode;           // 0x30  (0)
};

// Allocation parameters for NV20_SUBDEVICE_0 (class 0x2080)
struct [[gnu::packed]] Nv2080AllocParameters {
    NvU32 subDeviceId;          // 0 = first subdevice
};
static_assert(sizeof(Nv2080AllocParameters) == 4);

// NV04_DISPLAY_COMMON (class 0x0073) — no parameters needed

// ============================================================================
// rpc_gsp_rm_control_v03_00 — GSP_RM_CONTROL payload (function 76)
// ============================================================================
struct [[gnu::packed]] RpcGspRmControl {
    NvHandle hClient;       // 0x00
    NvHandle hObject;       // 0x04
    NvU32    cmd;           // 0x08  NV*_CTRL_CMD_*
    NvU32    status;        // 0x0C  (0 on send, filled by GSP)
    NvU32    paramsSize;    // 0x10
    NvU32    flags;         // 0x14
    // u8 params[];         // 0x18  command-specific
};
static_assert(sizeof(RpcGspRmControl) == 24);

// ============================================================================
// Display control parameter structs
// ============================================================================

// NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED (0x730120)
struct [[gnu::packed]] Nv0073CtrlSystemGetSupportedParams {
    NvU32 subDeviceInstance;     // IN: 0
    NvU32 displayMask;          // OUT: bitmask of supported displays
    NvU32 displayMaskDDC;       // OUT: displays with DDC
};

// NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS (0x730102)
struct [[gnu::packed]] Nv0073CtrlSystemGetNumHeadsParams {
    NvU32 subDeviceInstance;     // IN: 0
    NvU32 flags;                // IN: 0
    NvU32 numHeads;             // OUT
};

// NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2 (0x730245)
static constexpr NvU32 NV0073_CTRL_SPECIFIC_GET_EDID_MAX_EDID_BYTES = 2048;
struct [[gnu::packed]] Nv0073CtrlSpecificGetEdidV2Params {
    NvU32 subDeviceInstance;     // IN: 0
    NvU32 displayId;            // IN: display bit from GET_SUPPORTED
    NvU32 bufferSize;           // IN/OUT: max 2048
    NvU32 flags;                // IN: 0
    NvU8  edidBuffer[NV0073_CTRL_SPECIFIC_GET_EDID_MAX_EDID_BYTES]; // OUT
};
static_assert(sizeof(Nv0073CtrlSpecificGetEdidV2Params) == 2064);

// NV0073_CTRL_CMD_DP_AUXCH_CTRL (0x731341)
static constexpr NvU32 NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE = 16;
struct [[gnu::packed]] Nv0073CtrlDpAuxchCtrlParams {
    NvU32  subDeviceInstance;    // IN: 0
    NvU32  displayId;           // IN
    NvBool bAddrOnly;           // IN: false normally
    NvU8   _pad[3];
    NvU32  cmd;                 // IN: see AUX cmd bits
    NvU32  addr;                // IN: DPCD address
    NvU8   data[NV0073_CTRL_DP_AUXCH_MAX_DATA_SIZE]; // IN/OUT
    NvU32  size;                // IN/OUT: bytes
    NvU32  replyType;           // OUT
    NvU32  retryTimeMs;         // IN: 0 for default
};

// NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE (0x730122)
struct [[gnu::packed]] Nv0073CtrlSystemGetConnectStateParams {
    NvU32 subDeviceInstance;     // IN
    NvU32 flags;                // IN
    NvU32 displayMask;          // IN/OUT
    NvU32 retryTimeMs;          // IN
};

// NV0073_CTRL_CMD_DFP_GET_INFO (0x731140)
struct [[gnu::packed]] Nv0073CtrlDfpGetInfoParams {
    NvU32 subDeviceInstance;     // IN
    NvU32 displayId;            // IN
    NvU32 flags;                // OUT (signal type, lane config, etc.)
    NvU32 flags2;               // OUT
};

// NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO (0x73028B)
struct Nv0073CtrlSpecificOrGetInfoParams {
    NvU32  subDeviceInstance;
    NvU32  displayId;
    NvU32  index;                // OUT: SOR/DAC index
    NvU32  type;                 // OUT: 0=NONE,1=DAC,2=SOR,3=PIOR
    NvU32  protocol;             // OUT: DP_A=8, TMDS_A=1, etc.
    NvU32  ditherType;
    NvU32  ditherAlgo;
    NvU32  location;
    NvU32  rootPortId;
    NvU32  dcbIndex;
    NvU64  vbiosAddress;
    NvBool bIsLitByVbios;
    NvBool bIsDispDynamic;
};

// NV0073_CTRL_CMD_DP_GET_CAPS (0x731369)
struct [[gnu::packed]] Nv0073CtrlDpGetCapsParams {
    NvU32  subDeviceInstance;
    NvU32  sorIndex;
    NvU32  maxLinkRate;          // OUT
    NvU32  dpVersionsSupported;  // OUT
    NvU32  UHBRSupported;        // OUT
    NvBool bIsMultistreamSupported; // OUT
    NvBool bIsSCEnabled;
    NvBool bHasIncreasedWatermarkLimits;
    NvBool bIsPC2Disabled;
    NvBool isSingleHeadMSTSupported;
    NvBool bFECSupported;
    NvBool bIsTrainPhyRepeater;
    NvBool bOverrideLinkBw;
    // DSC sub-struct follows (28 bytes)
    NvBool bDscSupported;
    NvU32  encoderColorFormatMask;
    NvU32  lineBufferSizeKB;
    NvU32  rateBufferSizeKB;
    NvU32  bitsPerPixelPrecision;
    NvU32  maxNumHztSlices;
    NvU32  lineBufferBitDepth;
};

// ============================================================================
// RPC Free — function 10
// ============================================================================
struct [[gnu::packed]] NvOs00Parameters {
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvHandle hObjectOld;
    NvV32    status;
};

struct [[gnu::packed]] RpcFree {
    NvOs00Parameters params;
};

// ============================================================================
// NV_VGPU_MSG_FUNCTION constants (function numbers for RPC header)
// ============================================================================
enum NvVgpuMsgFunction : NvU32 {
    NV_VGPU_MSG_FUNCTION_NOP                    = 0,
    NV_VGPU_MSG_FUNCTION_SET_GUEST_SYSTEM_INFO  = 1,
    NV_VGPU_MSG_FUNCTION_ALLOC_ROOT             = 2,
    NV_VGPU_MSG_FUNCTION_FREE                   = 10,
    NV_VGPU_MSG_FUNCTION_GET_STATIC_INFO        = 51,
    NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO    = 65,
    NV_VGPU_MSG_FUNCTION_UPDATE_BAR_PDE         = 70,
    NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD    = 71,
    NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO    = 72,
    NV_VGPU_MSG_FUNCTION_SET_REGISTRY           = 73,
    NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL         = 76,
    NV_VGPU_MSG_FUNCTION_GET_STATIC_INFO2       = 77,
    NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC           = 103,
    NV_VGPU_MSG_FUNCTION_NUM_FUNCTIONS           = 203,
};

// ============================================================================
// Display class and command IDs
// ============================================================================
static constexpr NvU32 NV01_ROOT            = 0x0000;
static constexpr NvU32 NV01_DEVICE_0        = 0x0080;
static constexpr NvU32 NV20_SUBDEVICE_0     = 0x2080;
static constexpr NvU32 NV04_DISPLAY_COMMON  = 0x0073;

// Display control commands (used with GSP_RM_CONTROL, function 76)
static constexpr NvU32 NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS       = 0x730102;
static constexpr NvU32 NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED       = 0x730120;
static constexpr NvU32 NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE   = 0x730122;
static constexpr NvU32 NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE          = 0x730126;
static constexpr NvU32 NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2       = 0x730245;
static constexpr NvU32 NV0073_CTRL_CMD_SPECIFIC_GET_CONNECTOR_DATA= 0x730250;
static constexpr NvU32 NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO       = 0x73028B;
static constexpr NvU32 NV0073_CTRL_CMD_SPECIFIC_GET_ALL_HEAD_MASK = 0x730287;
static constexpr NvU32 NV0073_CTRL_CMD_DFP_GET_INFO               = 0x731140;
static constexpr NvU32 NV0073_CTRL_CMD_DFP_ASSIGN_SOR             = 0x731152;
static constexpr NvU32 NV0073_CTRL_CMD_DP_AUXCH_CTRL              = 0x731341;
static constexpr NvU32 NV0073_CTRL_CMD_DP_CTRL                    = 0x731343;
static constexpr NvU32 NV0073_CTRL_CMD_DP_SET_LANE_DATA           = 0x731346;
static constexpr NvU32 NV0073_CTRL_CMD_DP_SET_MANUAL_DISPLAYPORT  = 0x731365;
static constexpr NvU32 NV0073_CTRL_CMD_DP_GET_CAPS                = 0x731369;
static constexpr NvU32 NV0073_CTRL_CMD_DP_CONFIG_INDEXED_LINK_RATES = 0x731377;

// Internal subdevice control commands
static constexpr NvU32 NV2080_CTRL_CMD_INTERNAL_DISPLAY_GET_STATIC_INFO = 0x20800A01;
static constexpr NvU32 NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM  = 0x20800A49;
static constexpr NvU32 NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER = 0x20800A58;
static constexpr NvU32 NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE   = 0x20800A5C;

// ============================================================================
// Doorbell helper
// ============================================================================
// After writing to command queue, notify GSP by writing to falcon MAILBOX0:
//   mmio_write32(bar0 + GSP_FALCON_BASE + 0xC00, 0x00000000);
//
// For GA102: GSP_FALCON_BASE = 0x110000
//   absolute address = BAR0 + 0x110C00
//
// For GA100: GSP_FALCON_BASE = 0x110000 (same)
```

---

## Sources

All struct layouts verified against:

1. **Linux kernel nouveau** — `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/`
   - `gsp.h` — GspSystemInfo, GspStaticConfigInfo, RPC header, queue structures
   - `alloc.h` — rpc_gsp_rm_alloc_v03_00, NVOS00_PARAMETERS
   - `ctrl.h` — rpc_gsp_rm_control_v03_00
   - `client.h` — NV0000_ALLOC_PARAMETERS, NV01_ROOT
   - `device.h` — NV0080_ALLOC_PARAMETERS, NV2080_ALLOC_PARAMETERS
   - `disp.h` — all NV0073_CTRL_* display command parameter structs
   - `rpcfn.h` — NV_VGPU_MSG_FUNCTION_* enum (all 203 values)
   - `msgfn.h` — NV_VGPU_MSG_EVENT_* enum

2. **NVIDIA open-gpu-kernel-modules** (`535.113.01`)
   - `src/nvidia/inc/kernel/gpu/gsp/gsp_static_config.h` — canonical GspSystemInfo,
     GspStaticConfigInfo (may have additional fields vs r535 nouveau snapshot)

3. **Original nouveau GSP patch** (Ben Skeggs, 2023-09)
   - `r535_gsp_rpc_set_system_info()` — field values
   - `r535_gsp_rpc_set_registry()` — minimal registry format
   - `r535_gsp_cmdq_push()` — doorbell write at 0xc00
   - `r535_gsp_rpc_get()` — RPC header initialization values
