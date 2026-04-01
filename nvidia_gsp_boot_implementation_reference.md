# NVIDIA GSP Boot Implementation Reference (GA102 / Ampere)
## Complete Implementation Details from Linux Nouveau and NVIDIA Open Source

All code, struct layouts, field values, and sequences derived from:
- `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/r535.c` (r535_gsp_oneinit, r535_gsp_init, etc.)
- `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c` (tu102_gsp_oneinit, tu102_gsp_booter_ctor)
- `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/ga102.c` (ga102_gsp_booter_ctor, ga102_gsp_fwsec_signature)
- `drivers/gpu/drm/nouveau/include/nvrm/535.54.03/` (NVIDIA RM headers)
- `NVIDIA/open-gpu-kernel-modules/src/nvidia/` (gsp_fw_wpr_meta.h, gsp_fw_heap.h, gsp_init_args.h, rmgspseq.h, libos_init_args.h)

---

## 1. r535_gsp_oneinit() — Master Boot Orchestration

This function runs exactly once during driver init. Here is the COMPLETE step-by-step sequence, extracted verbatim from the kernel source:

### Step 1: Initialize mutexes
```c
mutex_init(&gsp->cmdq.mutex);
mutex_init(&gsp->msgq.mutex);
```

### Step 2: Construct booter firmware objects (for SEC2 falcon)
```c
gsp->func->booter.ctor(gsp, "booter-load", gsp->fws.booter.load,
                        &device->sec2->falcon, &gsp->booter.load);

gsp->func->booter.ctor(gsp, "booter-unload", gsp->fws.booter.unload,
                        &device->sec2->falcon, &gsp->booter.unload);
```
For GA102, `booter.ctor` = `ga102_gsp_booter_ctor()` (see Section 5).

### Step 3: Extract .fwimage from ELF → scatter-gather DMA
```c
r535_gsp_elf_section(gsp, ".fwimage", &data, &size);
nvkm_firmware_ctor(&r535_gsp_fw, "gsp-rm", device, data, size, &gsp->fw);
```
`nvkm_firmware_ctor` with `type = NVKM_FIRMWARE_IMG_SGT` allocates an `sg_table`, copies the firmware data page-by-page, and calls `dma_map_sgtable()` to get DMA addresses for each page.

### Step 4: Extract .fwsignature_ga10x → coherent DMA
```c
r535_gsp_elf_section(gsp, gsp->func->sig_section, &data, &size);
// sig_section = ".fwsignature_ga10x" for GA102
nvkm_gsp_mem_ctor(gsp, ALIGN(size, 256), &gsp->sig);
memcpy(gsp->sig.data, data, size);
```
`nvkm_gsp_mem_ctor` calls `dma_alloc_coherent()`:
```c
mem->data = dma_alloc_coherent(dev, size, &mem->addr, GFP_KERNEL);
```

### Step 5: Build Radix-3 page table
```c
nvkm_gsp_radix3_sg(device, &gsp->fw.mem.sgt, gsp->fw.len, &gsp->radix3);
```
See Section 2 for EXACT construction.

### Step 6: Register message notification handlers
```c
r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER,
                      r535_gsp_msg_run_cpu_sequencer, gsp);
r535_gsp_msg_ntfy_add(gsp, NV_VGPU_MSG_EVENT_OS_ERROR_LOG,
                      r535_gsp_msg_os_error_log, gsp);
```
- `NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER` = `0x1002`
- `NV_VGPU_MSG_EVENT_OS_ERROR_LOG` = `0x1006`

### Step 7: Construct bootloader DMA buffer
```c
r535_gsp_rm_boot_ctor(gsp);
```
This function:
```c
hdr = nvfw_bin_hdr(&gsp->subdev, fw->data);         // Parse BinHdr from bootloader blob
desc = (void *)fw->data + hdr->header_offset;         // Get RM_RISCV_UCODE_DESC

nvkm_gsp_mem_ctor(gsp, hdr->data_size, &gsp->boot.fw);  // Allocate coherent DMA
memcpy(gsp->boot.fw.data, fw->data + hdr->data_offset, hdr->data_size);

gsp->boot.code_offset     = desc->monitorCodeOffset;
gsp->boot.data_offset     = desc->monitorDataOffset;
gsp->boot.manifest_offset = desc->manifestOffset;
gsp->boot.app_version     = desc->appVersion;
```

### Step 8: Release raw firmware blobs
```c
r535_gsp_dtor_fws(gsp);
```
Frees the kernel firmware objects (the raw file contents). Data has been copied to DMA buffers.

### Step 9: Compute FB layout
```c
// FRTS region: 1MB, below VGA workspace, 128KB-aligned
gsp->fb.wpr2.frts.size = 0x100000;
gsp->fb.wpr2.frts.addr = ALIGN_DOWN(gsp->fb.bios.addr, 0x20000) - gsp->fb.wpr2.frts.size;

// Boot firmware region: bootloader size, 4KB-aligned below FRTS
gsp->fb.wpr2.boot.size = gsp->boot.fw.size;
gsp->fb.wpr2.boot.addr = ALIGN_DOWN(gsp->fb.wpr2.frts.addr - gsp->fb.wpr2.boot.size, 0x1000);

// GSP FW ELF region: firmware image size, 64KB-aligned below boot
gsp->fb.wpr2.elf.size = gsp->fw.len;
gsp->fb.wpr2.elf.addr = ALIGN_DOWN(gsp->fb.wpr2.boot.addr - gsp->fb.wpr2.elf.size, 0x10000);

// GSP heap: computed from FB size (see Section 4)
{
    u32 fb_size_gb = DIV_ROUND_UP_ULL(gsp->fb.size, 1 << 30);

    gsp->fb.wpr2.heap.size =
        gsp->func->wpr_heap.os_carveout_size +      // 22 << 20 for baremetal libos3
        gsp->func->wpr_heap.base_size +              // 8 << 20 for Turing-Ada
        ALIGN(GSP_FW_HEAP_PARAM_SIZE_PER_GB_FB * fb_size_gb, 1 << 20) +  // 96KB/GB
        ALIGN(GSP_FW_HEAP_PARAM_CLIENT_ALLOC_SIZE, 1 << 20);   // 48KB * 2048

    gsp->fb.wpr2.heap.size = max(gsp->fb.wpr2.heap.size, gsp->func->wpr_heap.min_size);
}

// Heap address: 1MB-aligned below ELF
gsp->fb.wpr2.heap.addr = ALIGN_DOWN(gsp->fb.wpr2.elf.addr - gsp->fb.wpr2.heap.size, 0x100000);
gsp->fb.wpr2.heap.size = ALIGN_DOWN(gsp->fb.wpr2.elf.addr - gsp->fb.wpr2.heap.addr, 0x100000);

// WPR2 start: 1MB-aligned below heap (leaving room for GspFwWprMeta)
gsp->fb.wpr2.addr = ALIGN_DOWN(gsp->fb.wpr2.heap.addr - sizeof(GspFwWprMeta), 0x100000);
gsp->fb.wpr2.size = gsp->fb.wpr2.frts.addr + gsp->fb.wpr2.frts.size - gsp->fb.wpr2.addr;

// Non-WPR heap: 1MB below WPR2 start
gsp->fb.heap.size = 0x100000;
gsp->fb.heap.addr = gsp->fb.wpr2.addr - gsp->fb.heap.size;
```

### Step 10: Run FWSEC-FRTS (establish WPR2)
```c
ret = nvkm_gsp_fwsec_frts(gsp);
```

### Step 11: Initialize LibOS arguments
```c
ret = r535_gsp_libos_init(gsp);
```
See Section 8 for EXACT details.

### Step 12: Initialize WPR metadata
```c
ret = r535_gsp_wpr_meta_init(gsp);
```
See Section 3 for EXACT population.

### Step 13: Send initial RPCs to GSP
```c
ret = r535_gsp_rpc_set_system_info(gsp);    // RPC fn=72 (GSP_SET_SYSTEM_INFO)
ret = r535_gsp_rpc_set_registry(gsp);       // RPC fn=73 (SET_REGISTRY)
```

### Step 14: Reset GSP into RISC-V mode
```c
ret = gsp->func->reset(gsp);   // ga102_gsp_reset()
```
For GA102, this calls `gp102_flcn_reset_eng()` then sets RISCV_BCR_CTRL:
```c
nvkm_falcon_mask(&gsp->falcon, 0x1668, 0x00000111, 0x00000111);
```
This sets `core_select=RISC-V`, `valid=1`, `br_fetch=1`.

### Step 15: Write LibOS DMA address to GSP mailboxes
```c
nvkm_falcon_wr32(&gsp->falcon, 0x040, lower_32_bits(gsp->libos.addr));   // MAILBOX0
nvkm_falcon_wr32(&gsp->falcon, 0x044, upper_32_bits(gsp->libos.addr));   // MAILBOX1
```

### r535_gsp_oneinit returns. Next: r535_gsp_init() is called.

---

## 1b. r535_gsp_init() — Booter Execution + INIT_DONE Wait

Called after `oneinit`. Also called at resume.

```c
int r535_gsp_init(struct nvkm_gsp *gsp)
{
    u32 mbox0, mbox1;

    if (!gsp->sr.meta.data) {
        // Normal boot path
        mbox0 = lower_32_bits(gsp->wpr_meta.addr);   // Lower 32 bits of wpr_meta DMA address
        mbox1 = upper_32_bits(gsp->wpr_meta.addr);   // Upper 32 bits of wpr_meta DMA address
    } else {
        // Resume path (not covered here)
        ...
    }

    // Execute booter_load on SEC2 → boots GSP-RM
    ret = r535_gsp_booter_load(gsp, mbox0, mbox1);

    // Wait for GSP_INIT_DONE event on message queue
    ret = r535_gsp_rpc_poll(gsp, NV_VGPU_MSG_EVENT_GSP_INIT_DONE);
    // NV_VGPU_MSG_EVENT_GSP_INIT_DONE = 0x1001

    gsp->running = true;
}
```

### r535_gsp_booter_load() details:
```c
static int r535_gsp_booter_load(struct nvkm_gsp *gsp, u32 mbox0, u32 mbox1)
{
    // Boot the booter_load firmware on SEC2 falcon
    // mbox0/mbox1 contain the GspFwWprMeta DMA address
    ret = nvkm_falcon_fw_boot(&gsp->booter.load, &gsp->subdev, true,
                              &mbox0, &mbox1, 0, 0);

    // Write app_version to GSP falcon register 0x080 (RM register)
    nvkm_falcon_wr32(&gsp->falcon, 0x080, gsp->boot.app_version);

    // Verify GSP RISC-V is now active
    if (!nvkm_falcon_riscv_active(&gsp->falcon))
        return -EIO;
}
```

`nvkm_falcon_riscv_active()` for GA102 checks `BAR0[GSP_BASE + 0x1388] & bit[7]` (RISCV_STATUS active bit).

---

## 2. Radix-3 Page Table Construction

### nvkm_gsp_radix3_sg() — EXACT code

```c
static int nvkm_gsp_radix3_sg(struct nvkm_device *device, struct sg_table *sgt,
                               u64 size, struct nvkm_gsp_radix3 *rx3)
{
    u64 addr;

    // Build levels from 2 (leaf) down to 0 (root)
    for (int i = ARRAY_SIZE(rx3->mem) - 1; i >= 0; i--) {
        u64 *ptes;
        int idx;

        // Calculate page table size for this level
        rx3->mem[i].size = ALIGN((size / GSP_PAGE_SIZE) * sizeof(u64), GSP_PAGE_SIZE);

        // Allocate coherent DMA memory
        rx3->mem[i].data = dma_alloc_coherent(device->dev, rx3->mem[i].size,
                                              &rx3->mem[i].addr, GFP_KERNEL);

        ptes = rx3->mem[i].data;

        if (i == 2) {
            // Level 2 (leaf): entries point to firmware data pages via scatter-gather
            struct scatterlist *sgl;
            for_each_sgtable_dma_sg(sgt, sgl, idx) {
                for (int j = 0; j < sg_dma_len(sgl) / GSP_PAGE_SIZE; j++)
                    *ptes++ = sg_dma_address(sgl) + (GSP_PAGE_SIZE * j);
            }
        } else {
            // Levels 0 and 1: entries point to pages of the next level
            for (int j = 0; j < size / GSP_PAGE_SIZE; j++)
                *ptes++ = addr + GSP_PAGE_SIZE * j;
        }

        // Next iteration uses this level's size and address
        size = rx3->mem[i].size;
        addr = rx3->mem[i].addr;
    }

    return 0;
}
```

### Key details:

- **Entry format**: `u64` (8 bytes), little-endian DMA physical address
- **Page size**: 4096 bytes
- **Entries per page**: 512 (4096 / 8)
- **Level 2 (leaf)**: Each entry = DMA physical address of a 4KB firmware data page. Entries are filled from the scatter-gather table, handling non-contiguous pages automatically.
- **Level 1**: Each entry = DMA physical address of a level-2 page.
- **Level 0 (root)**: Single page, single entry = DMA physical address of level-1 page.

### Sizing for ~23MB firmware:
```
Firmware pages = ceil(23MB / 4KB) ≈ 5888
Level 2 entries = 5888 × 8 bytes = 47104 bytes → ALIGN to 48KB (12 pages)
Level 1 entries = ceil(12 pages / 512) = 1 → 8 bytes (1 page allocated, minimum)
Level 0 entries = 1 → 8 bytes (1 page allocated)
```

### struct nvkm_gsp_radix3:
```c
struct nvkm_gsp_radix3 {
    struct nvkm_gsp_mem mem[3];   // [0]=root, [1]=middle, [2]=leaf
};

struct nvkm_gsp_mem {
    u32 size;           // Allocation size in bytes
    void *data;         // Kernel virtual address
    dma_addr_t addr;    // DMA physical address
};
```

The DMA address given to the booter is `rx3->mem[0].addr` (level 0 root page).

---

## 3. GspFwWprMeta — EXACT Struct Layout and Population

### struct GspFwWprMeta (256 bytes total)

From `gsp_fw_wpr_meta.h` (NVIDIA open-gpu-kernel-modules, verbatim):

| Offset | Size | Field | Value Set By Nouveau |
|--------|------|-------|---------------------|
| 0x00 | 8 | `magic` | `0xdc3aae21371a60b3` |
| 0x08 | 8 | `revision` | `1` |
| 0x10 | 8 | `sysmemAddrOfRadix3Elf` | `gsp->radix3.mem[0].addr` |
| 0x18 | 8 | `sizeOfRadix3Elf` | `gsp->fb.wpr2.elf.size` (= firmware .fwimage size) |
| 0x20 | 8 | `sysmemAddrOfBootloader` | `gsp->boot.fw.addr` |
| 0x28 | 8 | `sizeOfBootloader` | `gsp->boot.fw.size` |
| 0x30 | 8 | `bootloaderCodeOffset` | `gsp->boot.code_offset` (= `desc->monitorCodeOffset`) |
| 0x38 | 8 | `bootloaderDataOffset` | `gsp->boot.data_offset` (= `desc->monitorDataOffset`) |
| 0x40 | 8 | `bootloaderManifestOffset` | `gsp->boot.manifest_offset` (= `desc->manifestOffset`) |
| 0x48 | 8 | `sysmemAddrOfSignature` | `gsp->sig.addr` (DMA addr of .fwsignature section) |
| 0x50 | 8 | `sizeOfSignature` | `gsp->sig.size` |
| 0x58 | 8 | `gspFwRsvdStart` | `gsp->fb.heap.addr` (= non-WPR heap start) |
| 0x60 | 8 | `nonWprHeapOffset` | `gsp->fb.heap.addr` |
| 0x68 | 8 | `nonWprHeapSize` | `gsp->fb.heap.size` (= `0x100000` = 1MB) |
| 0x70 | 8 | `gspFwWprStart` | `gsp->fb.wpr2.addr` (1MB-aligned) |
| 0x78 | 8 | `gspFwHeapOffset` | `gsp->fb.wpr2.heap.addr` |
| 0x80 | 8 | `gspFwHeapSize` | `gsp->fb.wpr2.heap.size` |
| 0x88 | 8 | `gspFwOffset` | `gsp->fb.wpr2.elf.addr` |
| 0x90 | 8 | `bootBinOffset` | `gsp->fb.wpr2.boot.addr` |
| 0x98 | 8 | `frtsOffset` | `gsp->fb.wpr2.frts.addr` |
| 0xA0 | 8 | `frtsSize` | `gsp->fb.wpr2.frts.size` (= `0x100000`) |
| 0xA8 | 8 | `gspFwWprEnd` | `ALIGN_DOWN(gsp->fb.bios.vga_workspace.addr, 0x20000)` |
| 0xB0 | 8 | `fbSize` | `gsp->fb.size` (total VRAM) |
| 0xB8 | 8 | `vgaWorkspaceOffset` | `gsp->fb.bios.vga_workspace.addr` |
| 0xC0 | 8 | `vgaWorkspaceSize` | `gsp->fb.bios.vga_workspace.size` |
| 0xC8 | 8 | `bootCount` | `0` |
| 0xD0 | 8 | `partitionRpcAddr` | `0` |
| 0xD8 | 2 | `partitionRpcRequestOffset` | `0` |
| 0xDA | 2 | `partitionRpcReplyOffset` | `0` |
| 0xDC | 4 | `elfCodeOffset` | `0` (not set by nouveau for initial boot) |
| 0xE0 | 4 | `elfDataOffset` | `0` |
| 0xE4 | 4 | `elfCodeSize` | `0` |
| 0xE8 | 4 | `elfDataSize` | `0` |
| 0xEC | 4 | `lsUcodeVersion` | `0` |
| 0xF0 | 1 | `gspFwHeapVfPartitionCount` | `0` |
| 0xF1 | 1 | `flags` | `0` |
| 0xF2 | 2 | `padding` | `0` |
| 0xF4 | 4 | `pmuReservedSize` | `0` |
| 0xF8 | 8 | `verified` | `0` (set to `0xa0a0a0a0a0a0a0a0` by Booter after verification) |

### EXACT wpr_meta_init code:
```c
static int r535_gsp_wpr_meta_init(struct nvkm_gsp *gsp)
{
    GspFwWprMeta *meta;

    nvkm_gsp_mem_ctor(gsp, 0x1000, &gsp->wpr_meta);  // allocate 4KB coherent DMA

    meta = gsp->wpr_meta.data;

    meta->magic    = GSP_FW_WPR_META_MAGIC;     // 0xdc3aae21371a60b3
    meta->revision = GSP_FW_WPR_META_REVISION;  // 1

    meta->sysmemAddrOfRadix3Elf     = gsp->radix3.mem[0].addr;
    meta->sizeOfRadix3Elf           = gsp->fb.wpr2.elf.size;
    meta->sysmemAddrOfBootloader    = gsp->boot.fw.addr;
    meta->sizeOfBootloader          = gsp->boot.fw.size;
    meta->bootloaderCodeOffset      = gsp->boot.code_offset;
    meta->bootloaderDataOffset      = gsp->boot.data_offset;
    meta->bootloaderManifestOffset  = gsp->boot.manifest_offset;
    meta->sysmemAddrOfSignature     = gsp->sig.addr;
    meta->sizeOfSignature           = gsp->sig.size;
    meta->gspFwRsvdStart            = gsp->fb.heap.addr;
    meta->nonWprHeapOffset          = gsp->fb.heap.addr;
    meta->nonWprHeapSize            = gsp->fb.heap.size;
    meta->gspFwWprStart             = gsp->fb.wpr2.addr;
    meta->gspFwHeapOffset           = gsp->fb.wpr2.heap.addr;
    meta->gspFwHeapSize             = gsp->fb.wpr2.heap.size;
    meta->gspFwOffset               = gsp->fb.wpr2.elf.addr;
    meta->bootBinOffset             = gsp->fb.wpr2.boot.addr;
    meta->frtsOffset                = gsp->fb.wpr2.frts.addr;
    meta->frtsSize                  = gsp->fb.wpr2.frts.size;
    meta->gspFwWprEnd               = ALIGN_DOWN(gsp->fb.bios.vga_workspace.addr, 0x20000);
    meta->fbSize                    = gsp->fb.size;
    meta->vgaWorkspaceOffset        = gsp->fb.bios.vga_workspace.addr;
    meta->vgaWorkspaceSize          = gsp->fb.bios.vga_workspace.size;
    meta->bootCount                 = 0;
    meta->partitionRpcAddr          = 0;
    meta->partitionRpcRequestOffset = 0;
    meta->partitionRpcReplyOffset   = 0;
    meta->verified                  = 0;

    return 0;
}
```

### Constants:
```c
#define GSP_FW_WPR_META_MAGIC     0xdc3aae21371a60b3ULL
#define GSP_FW_WPR_META_REVISION  1
#define GSP_FW_WPR_META_VERIFIED  0xa0a0a0a0a0a0a0a0ULL
```

---

## 4. FB Layout Computation

### VGA Workspace Address (tu102_gsp_vga_workspace_addr):
```c
static u64 tu102_gsp_vga_workspace_addr(struct nvkm_gsp *gsp, u64 fb_size)
{
    const u64 base = fb_size - 0x100000;   // Default: 1MB from end
    u64 addr = 0;

    if (device->disp)
        addr = nvkm_rd32(device, 0x625f04);  // NV_PDISP_VGA_WORKSPACE_BASE
    if (!(addr & 0x00000008))
        return base;

    addr = (addr & 0xffffff00) << 8;
    if (addr < base)
        return fb_size - 0x20000;   // Fallback: 128KB from end

    return addr;
}
```

### tu102_gsp_oneinit (pre-r535_gsp_oneinit):
```c
int tu102_gsp_oneinit(struct nvkm_gsp *gsp)
{
    gsp->fb.size = nvkm_fb_vidmem_size(gsp->subdev.device);

    gsp->fb.bios.vga_workspace.addr = tu102_gsp_vga_workspace_addr(gsp, gsp->fb.size);
    gsp->fb.bios.vga_workspace.size = gsp->fb.size - gsp->fb.bios.vga_workspace.addr;
    gsp->fb.bios.addr = gsp->fb.bios.vga_workspace.addr;
    gsp->fb.bios.size = gsp->fb.bios.vga_workspace.size;

    return r535_gsp_oneinit(gsp);
}
```

### GSP Heap Size Formula (from gsp_fw_heap.h):
```c
#define GSP_FW_HEAP_PARAM_OS_SIZE_LIBOS3_BAREMETAL  (22 << 20)   // 22 MB
#define GSP_FW_HEAP_PARAM_BASE_RM_SIZE_TU10X        (8 << 20)    // 8 MB (Turing thru Ada)
#define GSP_FW_HEAP_PARAM_BASE_RM_SIZE_GH100        (14 << 20)   // 14 MB (Hopper+)
#define GSP_FW_HEAP_PARAM_SIZE_PER_GB               (96 << 10)   // 96 KB per GB VRAM
#define GSP_FW_HEAP_PARAM_CLIENT_ALLOC_SIZE  ((48 << 10) * 2048) // 96 MB

// Min heap: 64 MB (Turing-Ada), 88 MB (baremetal min override)
```

### GA102 wpr_heap config:
```c
.wpr_heap.base_size = 8 << 20,       // 8 MB
.wpr_heap.min_size = 64 << 20,       // 64 MB minimum (from tu102, ga102 doesn't override)
.wpr_heap.os_carveout_size = 22 << 20, // 22 MB (LIBOS3 baremetal)
```

### Concrete example for GA102 with 12GB VRAM:

```
fb_size              = 12 * 1024 * 1024 * 1024 = 0x300000000
fb_size_gb           = 12
vga_workspace        = fb_size - 0x100000 = 0x2FFF00000

FRTS:
  size = 0x100000 (1MB)
  addr = ALIGN_DOWN(0x2FFF00000, 0x20000) - 0x100000
       = 0x2FFE00000 - 0x100000
       = 0x2FFD00000

boot (bootloader ucode, ~4KB):
  size = gsp->boot.fw.size (e.g., 0x1000)
  addr = ALIGN_DOWN(0x2FFD00000 - 0x1000, 0x1000) = 0x2FFCFF000

elf (.fwimage, ~23MB = 0x1700000):
  size = 0x1700000
  addr = ALIGN_DOWN(0x2FFCFF000 - 0x1700000, 0x10000) = 0x2FE5F0000

heap:
  os_carveout = 22 MB = 0x1600000
  base        = 8 MB  = 0x800000
  per_gb      = ALIGN(96KB * 12, 1MB) = ALIGN(0x120000, 0x100000) = 0x200000 (2MB)
  client      = ALIGN(96MB, 1MB) = 0x6000000 (96MB)
  total       = 0x1600000 + 0x800000 + 0x200000 + 0x6000000 = 0x8000000 (128MB)
  min         = 0x4000000 (64MB)
  effective   = max(128MB, 64MB) = 128MB = 0x8000000
  addr        = ALIGN_DOWN(0x2FE5F0000 - 0x8000000, 0x100000) = 0x2F65F0000
  size readjusted = ALIGN_DOWN(0x2FE5F0000 - 0x2F65F0000, 0x100000) = 0x8000000

WPR2 start:
  addr = ALIGN_DOWN(0x2F65F0000 - 256, 0x100000) = 0x2F6500000
  size = (0x2FFD00000 + 0x100000) - 0x2F6500000

non-WPR heap:
  size = 0x100000 (1MB)
  addr = 0x2F6500000 - 0x100000 = 0x2F6400000

gspFwRsvdStart = 0x2F6400000 (same as non-WPR heap start)
```

### Alignment Requirements Summary:
| Region | Alignment |
|--------|-----------|
| FRTS | Below 128KB-aligned VGA workspace |
| Boot firmware | 4KB (0x1000) |
| GSP FW ELF | 64KB (0x10000) |
| WPR2 heap | 1MB (0x100000) |
| WPR2 start | 1MB (0x100000) |
| WPR2 end | 128KB (0x20000) |
| Non-WPR heap | derived from WPR2 start |

---

## 5. Booter Signature Patching (ga102_gsp_booter_ctor)

### ga102_gsp_booter_ctor() — EXACT code for GA102:
```c
int ga102_gsp_booter_ctor(struct nvkm_gsp *gsp, const char *name,
                          const struct firmware *blob,
                          struct nvkm_falcon *falcon, struct nvkm_falcon_fw *fw)
{
    const struct nvkm_falcon_fw_func *func = &ga102_flcn_fw;  // GA102 DMA-based loader
    const struct nvfw_bin_hdr *hdr;
    const struct nvfw_hs_header_v2 *hshdr;
    const struct nvfw_hs_load_header_v2 *lhdr;
    u32 loc, sig, cnt, *meta;

    hdr   = nvfw_bin_hdr(subdev, blob->data);
    hshdr = nvfw_hs_header_v2(subdev, blob->data + hdr->header_offset);
    meta  = (u32 *)(blob->data + hshdr->meta_data_offset);     // HsSignatureParams
    loc   = *(u32 *)(blob->data + hshdr->patch_loc);            // DMEM patch offset
    sig   = *(u32 *)(blob->data + hshdr->patch_sig);            // Signature base index
    cnt   = *(u32 *)(blob->data + hshdr->num_sig);              // Number of signatures

    // Create falcon FW object with DMA payload
    nvkm_falcon_fw_ctor(func, name, subdev->device, true,
                        blob->data + hdr->data_offset, hdr->data_size, falcon, fw);

    // Register signature info: location, per-sig size, signature data base, count
    nvkm_falcon_fw_sign(fw, loc, hshdr->sig_prod_size / cnt,
                        blob->data, cnt, hshdr->sig_prod_offset + sig, 0, 0);

    lhdr = nvfw_hs_load_header_v2(subdev, blob->data + hshdr->header_offset);

    fw->imem_base_img = lhdr->app[0].offset;    // IMEM source offset in payload
    fw->imem_base = 0;                            // IMEM destination base in falcon
    fw->imem_size = lhdr->app[0].size;            // IMEM size

    fw->dmem_base_img = lhdr->os_data_offset;   // DMEM source offset in payload
    fw->dmem_base = 0;                            // DMEM destination base in falcon
    fw->dmem_size = lhdr->os_data_size;           // DMEM size
    fw->dmem_sign = loc - lhdr->os_data_offset;  // Signature patch offset within DMEM

    fw->boot_addr = lhdr->app[0].offset;         // Boot address

    // GA102-specific: extract fuse metadata from HsSignatureParams
    fw->fuse_ver  = meta[0];    // HsSignatureParams.fuse_ver
    fw->engine_id = meta[1];    // HsSignatureParams.engine_id_mask
    fw->ucode_id  = meta[2];    // HsSignatureParams.ucode_id
}
```

### ga102_gsp_fwsec_signature() — Signature Index Selection for GA102:
```c
static int ga102_gsp_fwsec_signature(struct nvkm_falcon_fw *fw, u32 *src_base_src)
{
    u32 sig_fuse_version = fw->fuse_ver;
    u32 reg_fuse_version;
    int idx = 0;

    // Read fuse register based on engine_id_mask
    if (fw->engine_id & 0x00000400) {
        // GSP engine: fuse at 0x8241c0 + (ucode_id - 1) * 4
        reg_fuse_version = nvkm_rd32(device, 0x8241c0 + (fw->ucode_id - 1) * 4);
    } else {
        WARN_ON(1);  // Only GSP fuse lookup implemented for GA102
        return -ENOSYS;
    }

    // BIT(fls(reg_fuse_version)): highest set bit → power of 2
    reg_fuse_version = BIT(fls(reg_fuse_version));

    // Check that the firmware supports this fuse version
    if (!(reg_fuse_version & fw->fuse_ver))
        return -EINVAL;

    // Walk bits to find the signature index
    while (!(reg_fuse_version & sig_fuse_version & 1)) {
        idx += (sig_fuse_version & 1);
        reg_fuse_version >>= 1;
        sig_fuse_version >>= 1;
    }

    return idx;  // Index into the signatures array
}
```

### For SEC2 Booter (engine_id_mask & 0x0001):
```
Fuse register = 0x824140 + (ucode_id - 1) * 4
```

### Fuse Register Addresses:
| Engine | Mask | Fuse Base | Formula |
|--------|------|-----------|---------|
| SEC2 | 0x0001 | 0x824140 | `0x824140 + (ucode_id - 1) * 4` |
| NVDEC | 0x0004 | 0x824100 | `0x824100 + (ucode_id - 1) * 4` |
| GSP | 0x0400 | 0x8241c0 | `0x8241c0 + (ucode_id - 1) * 4` |

---

## 6. SEC2 Booter Execution

### nvkm_falcon_fw_boot() sequence (for booter_load):

The booter is loaded onto the SEC2 falcon at BAR0 base `0x840000`.

1. **Signature selection**: `ga102_gsp_fwsec_signature()` picks correct signature index
2. **Signature patching**: Copy selected signature into DMEM at `fw->dmem_sign` offset
3. **Falcon reset**: Full GA102 reset sequence (see falcon reference)
4. **DMA load**: IMEM + DMEM via DMA transfer to SEC2 falcon
5. **BROM programming**: Set PKC parameters for HS verification
6. **Boot**: Set mailboxes and start CPU

### Mailbox values for booter_load:
```c
MAILBOX0 = lower_32_bits(wpr_meta_phys_addr)   // Lower 32 bits of GspFwWprMeta DMA
MAILBOX1 = upper_32_bits(wpr_meta_phys_addr)   // Upper 32 bits of GspFwWprMeta DMA
```

### Boot address:
```c
boot_addr = lhdr->app[0].offset   // From HsLoadHeaderV2App[0].offset
```

### Timeout:
```c
// nvkm_falcon_fw_boot uses 2 second timeout for halt
nvkm_msec(device, 2000,
    if (nvkm_falcon_rd32(&gsp->falcon, 0x100) & 0x00000010)  // CPUCTL halted
        break;
);
```

### Success check:
```c
// After boot, check mailbox0 == 0 for success
mbox0 = nvkm_falcon_rd32(&sec2->falcon, 0x040);
if (mbox0 != 0)
    return -EIO;
```

### Post-booter for GSP (in r535_gsp_booter_load):
```c
// Write app_version to GSP RM register
nvkm_falcon_wr32(&gsp->falcon, 0x080, gsp->boot.app_version);

// Verify RISC-V is active on GSP
// Check BAR0[GSP_BASE + 0x1388] bit[7] (RISCV_STATUS active)
if (!nvkm_falcon_riscv_active(&gsp->falcon))
    return -EIO;
```

### Booter Unload mailbox values:
```c
MAILBOX0 = 0xff   // For non-suspend unload
MAILBOX1 = 0xff
```

---

## 7. GSP Sequencer

### Overview
The GSP sequencer is a **CPU-side** mechanism where GSP-RM sends register operation commands to the host CPU to execute. It is NOT a pre-boot mechanism — it runs AFTER GSP-RM is booted, as an event notification.

### Event: `NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER` (0x1002)

GSP-RM sends this event via the message queue. The host driver processes it in `r535_gsp_msg_run_cpu_sequencer()`.

### Opcodes (from rmgspseq.h):
```c
typedef enum GSP_SEQ_BUF_OPCODE {
    GSP_SEQ_BUF_OPCODE_REG_WRITE = 0,         // Write a register
    GSP_SEQ_BUF_OPCODE_REG_MODIFY,             // Read-modify-write a register
    GSP_SEQ_BUF_OPCODE_REG_POLL,               // Poll a register until condition
    GSP_SEQ_BUF_OPCODE_DELAY_US,               // Delay in microseconds
    GSP_SEQ_BUF_OPCODE_REG_STORE,              // Read register, store in save area
    GSP_SEQ_BUF_OPCODE_CORE_RESET,             // Reset GSP falcon
    GSP_SEQ_BUF_OPCODE_CORE_START,             // Start GSP falcon CPU
    GSP_SEQ_BUF_OPCODE_CORE_WAIT_FOR_HALT,     // Wait for falcon halt
    GSP_SEQ_BUF_OPCODE_CORE_RESUME,            // Full resume sequence (reset + SEC2 boot)
} GSP_SEQ_BUF_OPCODE;
```

### Payload structs:
```c
struct GSP_SEQ_BUF_PAYLOAD_REG_WRITE  { u32 addr; u32 val; };                    // 8 bytes
struct GSP_SEQ_BUF_PAYLOAD_REG_MODIFY { u32 addr; u32 mask; u32 val; };          // 12 bytes
struct GSP_SEQ_BUF_PAYLOAD_REG_POLL   { u32 addr; u32 mask; u32 val;
                                        u32 timeout; u32 error; };               // 20 bytes
struct GSP_SEQ_BUF_PAYLOAD_DELAY_US   { u32 val; };                              // 4 bytes
struct GSP_SEQ_BUF_PAYLOAD_REG_STORE  { u32 addr; u32 index; };                  // 8 bytes
```

### RPC message format:
```c
typedef struct rpc_run_cpu_sequencer_v17_00 {
    NvU32 bufferSizeDWord;     // Total buffer size in DWORDs
    NvU32 cmdIndex;            // Number of valid DWORDs in commandBuffer
    NvU32 regSaveArea[8];      // Register save slots for OPCODE_REG_STORE
    NvU32 commandBuffer[];     // Variable-length array of opcodes + payloads
} rpc_run_cpu_sequencer_v17_00;
```

### Key: the sequencer is NOT used before SEC2 booter. It is received as a message from GSP-RM during or after boot, typically for suspend/resume operations.

### CORE_RESET handler in detail:
```c
case GSP_SEQ_BUF_OPCODE_CORE_RESET:
    nvkm_falcon_reset(&gsp->falcon);
    nvkm_falcon_mask(&gsp->falcon, 0x624, 0x00000080, 0x00000080); // FBIF_CTL
    nvkm_falcon_wr32(&gsp->falcon, 0x10c, 0x00000000);             // DMACTL
    break;
```

### CORE_RESUME handler (full re-boot sequence):
```c
case GSP_SEQ_BUF_OPCODE_CORE_RESUME:
    gsp->func->reset(gsp);                                    // Reset GSP falcon
    nvkm_falcon_wr32(&gsp->falcon, 0x040, lower_32(libos));   // MAILBOX0 = libos addr
    nvkm_falcon_wr32(&gsp->falcon, 0x044, upper_32(libos));   // MAILBOX1 = libos addr
    nvkm_falcon_start(&sec2->falcon);                          // Start SEC2
    // Poll 0x1180f8 bit[26] for 2 seconds (SEC2 done)
    // Check SEC2 mailbox0 == 0
    nvkm_falcon_wr32(&gsp->falcon, 0x080, app_version);       // RM register
    // Verify RISC-V active
    break;
```

---

## 8. LibOS Arguments / GSP Boot Parameters

### r535_gsp_libos_init() — EXACT code:
```c
static int r535_gsp_libos_init(struct nvkm_gsp *gsp)
{
    LibosMemoryRegionInitArgument *args;

    // Allocate 4KB page for libos arguments (holds 4 entries)
    nvkm_gsp_mem_ctor(gsp, 0x1000, &gsp->libos);
    args = gsp->libos.data;

    // --- Log buffer: LOGINIT (64KB) ---
    nvkm_gsp_mem_ctor(gsp, 0x10000, &gsp->loginit);     // 64KB coherent DMA
    args[0].id8  = r535_gsp_libos_id8("LOGINIT");        // 0x4c4f47494e495400
    args[0].pa   = gsp->loginit.addr;                    // DMA physical address
    args[0].size = gsp->loginit.size;                    // 0x10000 = 64KB
    args[0].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;       // 1
    args[0].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;       // 1
    // Create PTE array at loginit.data + 8:
    create_pte_array(gsp->loginit.data + sizeof(u64), gsp->loginit.addr, gsp->loginit.size);

    // --- Log buffer: LOGINTR (64KB) ---
    nvkm_gsp_mem_ctor(gsp, 0x10000, &gsp->logintr);
    args[1].id8  = r535_gsp_libos_id8("LOGINTR");
    args[1].pa   = gsp->logintr.addr;
    args[1].size = gsp->logintr.size;
    args[1].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
    args[1].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;
    create_pte_array(gsp->logintr.data + sizeof(u64), gsp->logintr.addr, gsp->logintr.size);

    // --- Log buffer: LOGRM (64KB) ---
    nvkm_gsp_mem_ctor(gsp, 0x10000, &gsp->logrm);
    args[2].id8  = r535_gsp_libos_id8("LOGRM");
    args[2].pa   = gsp->logrm.addr;
    args[2].size = gsp->logrm.size;
    args[2].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
    args[2].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;
    create_pte_array(gsp->logrm.data + sizeof(u64), gsp->logrm.addr, gsp->logrm.size);

    // --- RM arguments ---
    r535_gsp_rmargs_init(gsp, false);   // Also allocates shared memory + message queues
    args[3].id8  = r535_gsp_libos_id8("RMARGS");
    args[3].pa   = gsp->rmargs.addr;
    args[3].size = gsp->rmargs.size;                     // 0x1000 = 4KB
    args[3].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
    args[3].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;

    return 0;
}
```

### struct LibosMemoryRegionInitArgument:
```c
typedef struct {
    LibosAddress id8;    // 8-byte tag (ASCII string packed as u64, MSB first)
    LibosAddress pa;     // Physical (DMA) address
    LibosAddress size;   // Size in bytes
    NvU8 kind;           // 0=NONE, 1=CONTIGUOUS, 2=RADIX3
    NvU8 loc;            // 0=NONE, 1=SYSMEM, 2=FB
} LibosMemoryRegionInitArgument;
```
Size: 26 bytes per entry, but likely padded to 32 or 40 bytes per entry.

### id8 encoding (r535_gsp_libos_id8):
```c
static inline u64 r535_gsp_libos_id8(const char *name)
{
    u64 id = 0;
    for (int i = 0; i < sizeof(id) && *name; i++, name++)
        id = (id << 8) | *name;
    return id;
}
```
- `"LOGINIT"` → `0x4c4f47494e495400` (null-padded to 8 bytes)
- `"LOGINTR"` → `0x4c4f47494e545200`
- `"LOGRM"` → `0x4c4f47524d000000`
- `"RMARGS"` → `0x524d415247530000`

### PTE array in log buffers:
Each log buffer has at byte offset 8 a PTE array mapping its own pages:
```c
static void create_pte_array(u64 *ptes, dma_addr_t addr, size_t size)
{
    unsigned int num_pages = DIV_ROUND_UP_ULL(size, GSP_PAGE_SIZE);
    for (unsigned int i = 0; i < num_pages; i++)
        ptes[i] = (u64)addr + (i << GSP_PAGE_SHIFT);
}
```

### Shared Memory and Message Queues (r535_gsp_shared_init):
```c
// Queue sizes
gsp->shm.cmdq.size = 0x40000;    // 256KB command queue
gsp->shm.msgq.size = 0x40000;    // 256KB message/status queue

// PTE count: pages for queues + pages for PTE array itself
gsp->shm.ptes.nr = (cmdq_size + msgq_size) >> 12;
gsp->shm.ptes.nr += DIV_ROUND_UP(gsp->shm.ptes.nr * sizeof(u64), GSP_PAGE_SIZE);

// Allocate single coherent DMA buffer for PTE array + cmdq + msgq
nvkm_gsp_mem_ctor(gsp, ptes_size + cmdq_size + msgq_size, &gsp->shm.mem);

// Layout within allocation:
// [PTE array] [Command Queue (256KB)] [Message Queue (256KB)]

// Fill PTE array: self-referencing page addresses
for (i = 0; i < ptes_nr; i++)
    gsp->shm.ptes.ptr[i] = gsp->shm.mem.addr + (i << GSP_PAGE_SHIFT);
```

### Command Queue Header (msgqTxHeader):
```c
cmdq->tx.version   = 0;
cmdq->tx.size       = 0x40000;          // 256KB
cmdq->tx.entryOff   = GSP_PAGE_SIZE;    // 4096 — entries start at page 1
cmdq->tx.msgSize    = GSP_PAGE_SIZE;    // 4096 — each message slot is one page
cmdq->tx.msgCount   = (size - entryOff) / msgSize;  // (256K - 4K) / 4K = 63
cmdq->tx.writePtr   = 0;
cmdq->tx.flags      = 1;                // "I want to swap RX"
cmdq->tx.rxHdrOff   = offsetof(typeof(*cmdq), rx.readPtr);  // Offset to RX header
```

### GSP_ARGUMENTS_CACHED (rmargs):
```c
args->messageQueueInitArguments.sharedMemPhysAddr = gsp->shm.mem.addr;  // DMA addr
args->messageQueueInitArguments.pageTableEntryCount = gsp->shm.ptes.nr;
args->messageQueueInitArguments.cmdQueueOffset = cmdq_ptr - mem_base;   // Offset in SHM
args->messageQueueInitArguments.statQueueOffset = msgq_ptr - mem_base;  // Offset in SHM

args->srInitArguments.oldLevel = 0;
args->srInitArguments.flags = 0;
args->srInitArguments.bInPMTransition = 0;
```

### How GSP finds the queues:
1. During `r535_gsp_oneinit()`, the libos args page address is written to GSP MAILBOX0/MAILBOX1
2. LibOS args entry[3] ("RMARGS") points to `GSP_ARGUMENTS_CACHED`
3. `GSP_ARGUMENTS_CACHED.messageQueueInitArguments.sharedMemPhysAddr` → shared memory base
4. Shared memory contains PTE array (self-referencing pages), then cmdq, then msgq
5. The PTE array allows GSP to map the shared memory pages
6. `cmdQueueOffset` and `statQueueOffset` within the shared memory locate the actual queues

---

## 9. INIT_DONE Wait

### How the driver knows GSP is done booting:

After `r535_gsp_booter_load()` returns, the driver waits for the `GSP_INIT_DONE` event:

```c
ret = r535_gsp_rpc_poll(gsp, NV_VGPU_MSG_EVENT_GSP_INIT_DONE);
```

`NV_VGPU_MSG_EVENT_GSP_INIT_DONE = 0x1001`

### r535_gsp_rpc_poll:
```c
static int r535_gsp_rpc_poll(struct nvkm_gsp *gsp, u32 fn)
{
    mutex_lock(&gsp->cmdq.mutex);
    repv = r535_gsp_msg_recv(gsp, fn, 0);   // Wait for message with function == fn
    mutex_unlock(&gsp->cmdq.mutex);
    return PTR_ERR_OR_ZERO(repv);
}
```

### r535_gsp_msg_recv:
```c
static struct nvfw_gsp_rpc *r535_gsp_msg_recv(struct nvkm_gsp *gsp, int fn, u32 repc)
{
    int time = 4000000;  // 4 second timeout (in microseconds)

retry:
    msg = r535_gsp_msgq_wait(gsp, sizeof(*msg), &size, &time);
    // Polls gsp->msgq.wptr != gsp->msgq.rptr
    // Reads message from msgq ring buffer

    msg = r535_gsp_msgq_recv(gsp, msg->length, &time);

    if (msg->function == fn) {
        // Found our message!
        return msg;
    }

    // Process notification handlers for other events
    for (i = 0; i < gsp->msgq.ntfy_nr; i++) {
        if (gsp->msgq.ntfy[i].fn == msg->function) {
            gsp->msgq.ntfy[i].func(priv, fn, msg->data, msg->length);
            break;
        }
    }

    r535_gsp_msg_done(gsp, msg);
    goto retry;  // Keep polling for our target message
}
```

### Timeout: **4 seconds** (4,000,000 µs).

### Message format (nvfw_gsp_rpc):
```c
struct nvfw_gsp_rpc {
    u32 header_version;       // 0x03000000
    u32 signature;            // 'CPRVR' = ('V'<<24)|('R'<<16)|('P'<<8)|'C'
    u32 length;               // Total message length
    u32 function;             // RPC function ID or event ID
    u32 rpc_result;           // 0 = success
    u32 rpc_result_private;
    u32 sequence;
    union {
        u32 spare;
        u32 cpuRmGfid;
    };
    u8 data[];                // Payload
};
```

### What information comes with INIT_DONE:
The GSP_INIT_DONE event is a simple notification (repc=0, meaning no reply data is expected). The driver just checks `msg->rpc_result == 0` for success. After INIT_DONE, `gsp->running = true` is set.

### During the wait, other events may arrive:
- `GSP_RUN_CPU_SEQUENCER` (0x1002) — GSP asks CPU to execute register operations
- `OS_ERROR_LOG` (0x1006) — GSP reports an error

These are processed via the registered notification handlers.

---

## 10. After INIT_DONE

### In r535_gsp_init():
```c
gsp->running = true;

// If this was a resume (sr.meta.data != NULL), cleanup:
if (gsp->sr.meta.data) {
    nvkm_gsp_mem_dtor(gsp, &gsp->sr.meta);
    nvkm_gsp_radix3_dtor(gsp, &gsp->sr.radix3);
    nvkm_gsp_sg_free(gsp->subdev.device, &gsp->sr.sgt);
}
```

### First RPCs (sent during oneinit, BEFORE booter):
The first RPCs are actually sent BEFORE the booter runs, during `r535_gsp_oneinit()`:

1. **`NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO`** (fn=72):
   Sends `GspSystemInfo` with PCI addresses, ACPI data, etc.

2. **`NV_VGPU_MSG_FUNCTION_SET_REGISTRY`** (fn=73):
   Sends registry key/value pairs (currently just a single empty entry).

These RPCs are queued into the command queue's shared memory BEFORE GSP boots. When GSP starts, it reads them from the queue.

### RPC format:
```c
rpc->header_version = 0x03000000;
rpc->signature = ('V' << 24) | ('R' << 16) | ('P' << 8) | 'C';  // "CRPV" in memory
rpc->function = fn;
rpc->rpc_result = 0xffffffff;
rpc->rpc_result_private = 0xffffffff;
rpc->length = sizeof(*rpc) + argc;
```

### After INIT_DONE, the higher-level nouveau driver (not in r535.c) sends:
1. `NV_VGPU_MSG_FUNCTION_ALLOC_ROOT` (fn=2) — Create RM client
2. `NV_VGPU_MSG_FUNCTION_ALLOC_DEVICE` (fn=3) — Allocate device object
3. `NV_VGPU_MSG_FUNCTION_ALLOC_SUBDEVICE` (fn=19) — Allocate subdevice
4. `NV_VGPU_MSG_FUNCTION_GET_STATIC_INFO` (fn=51) — Get GPU static configuration

### What is NOT freed after boot:
- Radix3 page tables (`gsp->radix3`) — kept for suspend/resume
- Signature buffer (`gsp->sig`) — kept for suspend/resume
- Firmware SG table (`gsp->fw`) — kept for suspend/resume
- Shared memory / message queues — used for ongoing RPC communication
- LibOS arguments — retained
- WPR metadata DMA buffer — retained

### What IS freed:
- Raw firmware file blobs (`gsp->fws.rm`, `gsp->fws.bl`, `gsp->fws.booter.*`) — freed in Step 8

---

## Appendix A: Complete Boot Sequence Timeline

```
1. nvkm_gsp_new_() → probes firmware interface, calls r535_gsp_load()
     └─ Loads 4 firmware files via request_firmware()

2. gsp->func->oneinit() = tu102_gsp_oneinit()
     ├─ Reads FB size, VGA workspace address
     └─ Calls r535_gsp_oneinit():
         ├─ Step 1-2: Init mutexes, construct booters
         ├─ Step 3-4: Parse ELF, DMA-map .fwimage + .fwsignature
         ├─ Step 5: Build radix3 page table
         ├─ Step 6: Register sequencer + error log handlers
         ├─ Step 7: DMA-map bootloader payload
         ├─ Step 8: Free raw firmware blobs
         ├─ Step 9: Compute FB layout
         ├─ Step 10: Run FWSEC-FRTS (establishes WPR2 in VRAM)
         ├─ Step 11: Init LibOS args (log buffers + rmargs + queues)
         ├─ Step 12: Populate GspFwWprMeta
         ├─ Step 13: Queue RPCs (SET_SYSTEM_INFO, SET_REGISTRY)
         ├─ Step 14: Reset GSP, set RISC-V mode
         └─ Step 15: Write libos DMA addr to GSP MAILBOX0/MAILBOX1

3. gsp->func->init() = r535_gsp_init()
     ├─ Write wpr_meta DMA addr to mbox0/mbox1
     ├─ r535_gsp_booter_load():
     │    ├─ nvkm_falcon_fw_boot(&gsp->booter.load, ..., &mbox0, &mbox1)
     │    │    ├─ Select signature (ga102_gsp_fwsec_signature)
     │    │    ├─ Patch signature into booter DMEM
     │    │    ├─ Reset SEC2 falcon
     │    │    ├─ DMA load IMEM+DMEM to SEC2
     │    │    ├─ Program BROM registers
     │    │    ├─ Set MAILBOX0 = lower_32(wpr_meta), MAILBOX1 = upper_32(wpr_meta)
     │    │    ├─ Set BOOTVEC = app[0].offset
     │    │    ├─ Write CPUCTL = 0x2 (start)
     │    │    └─ Poll CPUCTL bit[4] (halted) — 2s timeout
     │    ├─ Write app_version to GSP RM register (0x080)
     │    └─ Verify GSP RISC-V active (0x1388 bit[7])
     │
     │  [SEC2 Booter executes:]
     │    ├─ Reads GspFwWprMeta from system memory (mbox0/mbox1 address)
     │    ├─ DMA copies bootloader from sysmem → VRAM at bootBinOffset
     │    ├─ DMA copies GSP FW ELF from sysmem (via radix3) → VRAM at gspFwOffset
     │    ├─ Verifies WPR2 region, locks it
     │    └─ Launches GSP bootloader on GSP RISC-V core
     │
     │  [GSP Bootloader executes on RISC-V:]
     │    ├─ Reads firmware from VRAM via radix3 page table
     │    ├─ Verifies signatures using manifest
     │    └─ Starts GSP-RM firmware
     │
     │  [GSP-RM starts:]
     │    ├─ Reads MAILBOX0/MAILBOX1 → libos args address
     │    ├─ Maps log buffers (LOGINIT, LOGINTR, LOGRM)
     │    ├─ Maps RMARGS → finds message queues
     │    ├─ Processes queued RPCs (SET_SYSTEM_INFO, SET_REGISTRY)
     │    ├─ May send GSP_RUN_CPU_SEQUENCER events
     │    └─ Sends GSP_INIT_DONE event (0x1001) on message queue
     │
     ├─ r535_gsp_rpc_poll(NV_VGPU_MSG_EVENT_GSP_INIT_DONE)
     │    └─ Polls message queue for up to 4 seconds
     │       (processes sequencer events during wait)
     │
     └─ gsp->running = true

4. Higher-level driver creates RM client and subdevice via RPCs.
```

---

## Appendix B: Key Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `GSP_FW_WPR_META_MAGIC` | `0xdc3aae21371a60b3` | WPR meta magic |
| `GSP_FW_WPR_META_REVISION` | `1` | WPR meta revision |
| `GSP_FW_WPR_META_VERIFIED` | `0xa0a0a0a0a0a0a0a0` | Set by Booter after OK |
| `GSP_PAGE_SIZE` | `4096` | GSP page size |
| `GSP_PAGE_SHIFT` | `12` | GSP page shift |
| `WPR_ALIGNMENT` | `0x20000` (128KB) | WPR boundary alignment |
| `GSP_FW_HEAP_PARAM_SIZE_PER_GB` | `96 << 10` (96KB) | Per-GB heap overhead |
| `GSP_FW_HEAP_PARAM_CLIENT_ALLOC_SIZE` | `(48<<10)*2048` (96MB) | Channel allocation |
| `GSP_FW_HEAP_PARAM_BASE_RM_SIZE_TU10X` | `8 << 20` (8MB) | Base RM size Turing-Ada |
| `GSP_FW_HEAP_PARAM_OS_SIZE_LIBOS3_BAREMETAL` | `22 << 20` (22MB) | LibOS overhead |
| `NV_VGPU_MSG_EVENT_GSP_INIT_DONE` | `0x1001` | Init done event |
| `NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER` | `0x1002` | CPU sequencer event |
| `NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO` | `72` | System info RPC |
| `NV_VGPU_MSG_FUNCTION_SET_REGISTRY` | `73` | Registry RPC |
| `NV_VGPU_MSG_FUNCTION_ALLOC_ROOT` | `2` | Alloc RM client |
| `FALCON_GSP_BASE` | `0x110000` | GSP falcon BAR0 base |
| `FALCON_SEC2_BASE` | `0x840000` | SEC2 falcon BAR0 base |
| `RISCV_BCR_CTRL` | `+0x1668` | RISC-V core select reg |
| `RISCV_STATUS` | `+0x1388` | RISC-V active status reg |
