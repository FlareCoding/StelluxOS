# NVIDIA Falcon DMA Interface & Boot Sequence Reference
## For GSP and SEC2 Falcons on Ampere (GA102) GPUs

All register offsets, bit layouts, and code sequences extracted from:
- Linux kernel `drivers/gpu/drm/nouveau/nvkm/falcon/` (v1.c, gm200.c, ga102.c, base.c, fw.c)
- Linux kernel `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/` (ga102.c, tu102.c, fwsec.c)
- Linux kernel `drivers/gpu/drm/nouveau/nvkm/engine/sec2/` (ga102.c, tu102.c)
- Linux kernel `drivers/gpu/nova-core/` patches (falcon.rs, regs.rs, firmware/fwsec.rs, falcon/hal/ga102.rs)

---

## 1. Falcon Base Addresses in BAR0

| Falcon Instance | BAR0 Base Address | Source |
|-----------------|-------------------|--------|
| **GSP Falcon** | `0x00110000` | `nova-core/falcon/gsp.rs`: `const BASE: usize = 0x00110000` |
| **SEC2 Falcon** | `0x00840000` | `nova-core/falcon/sec2.rs`: `const BASE: usize = 0x00840000`; also `engine/sec2/ga102.c`: `const u32 addr = 0x840000` |

All falcon registers below are at offsets **relative to these base addresses**. For example, GSP CPUCTL is at `0x110000 + 0x100 = 0x110100`, SEC2 CPUCTL is at `0x840000 + 0x100 = 0x840100`.

GA102 falcons have a second register block at `BASE + 0x1000` (the `addr2` field). For GSP: `0x111000`, for SEC2: `0x841000`.

---

## 2. Complete Falcon Register Map

All offsets are relative to the falcon's base address (`BASE`).

### 2.1 Core Control Registers

| Offset | Register Name | Bits | Description |
|--------|--------------|------|-------------|
| `+0x004` | **IRQSCLR** | `[4]` halt, `[6]` swgen0 | IRQ status clear. Write 1 to clear specific IRQ bits. |
| `+0x008` | **IRQSTAT** | `[3]` | Interrupt status (used by `gm200_flcn_bind_stat` for bind completion) |
| `+0x014` | **IRQMASK** | `[31:0]` | IRQ mask. Written to `0xffffffff` during disable to mask all IRQs. |
| `+0x040` | **MAILBOX0** | `[31:0]` value | Mailbox register 0. Used to pass parameters to/from firmware. |
| `+0x044` | **MAILBOX1** | `[31:0]` value | Mailbox register 1. Used to pass parameters to/from firmware. |
| `+0x048` | **IRQMODE** | `[1:0]` | IRQ delivery mode. Set to 0 during disable, bit 0 set during fw load. |
| `+0x054` | **INST_BIND** | `[30]` valid, `[29:28]` target, `[27:0]` addr>>12 | Instance block binding for virtual memory context. |
| `+0x058` | **BIND_CTRL** | `[1]` | Bind control. Bit 1 written to trigger inst block unbind after load. |
| `+0x084` | **RM** | `[31:0]` value | Firmware resource manager register. Written with `NV_PMC_BOOT_0` value during reset. |
| `+0x090` | **BIND_CFG** | `[16]` | Bind configuration (bit 16 set during bind to enable). |
| `+0x0a4` | **BIND_CTRL2** | `[3]` | Additional bind control. |
| `+0x0dc` | **BIND_STAT** | `[14:12]` status | Bind status. Value `5` = bind complete, `0` = unbind complete. |

### 2.2 Hardware Configuration Registers

| Offset | Register Name | Bits | Description |
|--------|--------------|------|-------------|
| `+0x0f4` | **HWCFG2** | `[10]` riscv, `[12]` mem_scrubbing, `[31]` reset_ready | Hardware config 2. GA102+ uses `[12]` for mem scrub status (`!bit12` = done), `[31]` for reset ready. Earlier gens use `+0x10c` bits `[2:1]`. |
| `+0x108` | **HWCFG_CODE_DATA** | `[8:0]` code_limit, `[17:9]` data_limit | Code/data memory sizes. `code_limit = (val & 0x1ff) << 8`, `data_limit = (val & 0x3fe00) >> 1`. |
| `+0x10c` | **DMACTL** | `[0]` require_ctx, `[1]` dmem_scrub, `[2]` imem_scrub, `[6:3]` dmaq_num, `[7]` secure_stat | DMA control. Written to `0` before DMA transfers. On pre-GA102, bits `[2:1]` also used as mem scrubbing done check. |
| `+0x12c` | **HWCFG1** | `[3:0]` core_rev, `[5:4]` security_model, `[7:6]` core_rev_subversion | Hardware config 1. core_rev: 1-7, security: 0=None/2=Light/3=Heavy. |

### 2.3 CPU Control Registers

| Offset | Register Name | Bits | Description |
|--------|--------------|------|-------------|
| `+0x100` | **CPUCTL** | `[1]` startcpu, `[4]` halted, `[6]` alias_en | CPU control. Write `0x2` (bit 1) to start CPU. Read bit 4 to check if halted. Bit 6 indicates alias register should be used. |
| `+0x104` | **BOOTVEC** | `[31:0]` boot_addr | Boot vector. Address where CPU starts executing after start. |
| `+0x130` | **CPUCTL_ALIAS** | `[1]` startcpu | CPU control alias. Used instead of CPUCTL when `alias_en` (bit 6 of CPUCTL) is set. Write `0x2` to start. |

### 2.4 DMA Transfer Registers

| Offset | Register Name | Bits | Description |
|--------|--------------|------|-------------|
| `+0x110` | **DMATRFBASE** | `[31:0]` base | DMA transfer base address, bits `[39:8]` of the physical address (value = `phys_addr >> 8`). |
| `+0x114` | **DMATRFMOFFS** | `[23:0]` offs | DMA transfer memory (IMEM/DMEM) offset. Destination offset within falcon memory. |
| `+0x118` | **DMATRFCMD** | See below | DMA transfer command register. Writing this triggers the DMA transfer. |
| `+0x11c` | **DMATRFFBOFFS** | `[31:0]` offs | DMA transfer framebuffer/source offset from the base address. |
| `+0x128` | **DMATRFBASE1** | `[8:0]` base | DMA transfer base address upper bits `[48:40]` (value = `phys_addr >> 40`). |

#### DMATRFCMD (`+0x118`) Bit Layout

| Bits | Field | Description |
|------|-------|-------------|
| `[0]` | full | Transfer FIFO full (read-only). |
| `[1]` | idle | Transfer complete/idle (read-only). Poll this bit until set. |
| `[3:2]` | sec | Security level: 0=NS, 1=LS, set to `0x1` for secure code. |
| `[4]` | imem | Target memory: `0` = DMEM, `1` = IMEM. |
| `[5]` | is_write | Direction: `0` = read from ext mem to falcon, `1` = write from falcon to ext mem. |
| `[10:8]` | size | Transfer size: encoded as `ilog2(bytes) - 2`. Value `0x6` = 256 bytes. |
| `[14:12]` | ctxdma | Context DMA index for aperture selection. |
| `[16]` | set_dmtag | Set DMA tag. |

### 2.5 IMEM PIO Access Registers (Programmed I/O, pre-GA102)

| Offset | Register Name | Description |
|--------|--------------|-------------|
| `+0x180 + (port * 0x10)` | **IMEMC** | IMEM control. Write: `start_addr | BIT(24)` (auto-increment) `| BIT(28)` (secure tag). |
| `+0x184 + (port * 0x10)` | **IMEMD** | IMEM data. Write 32-bit words sequentially. |
| `+0x188 + (port * 0x10)` | **IMEMT** | IMEM tag. Write tag value every 256 bytes (`i & 0x3f == 0`). |

### 2.6 DMEM PIO Access Registers (Programmed I/O)

| Offset | Register Name | Description |
|--------|--------------|-------------|
| `+0x1c0 + (port * 8)` | **DMEMC** | DMEM control. Write `start_addr | BIT(24)` (auto-increment write) or `start_addr | BIT(25)` (auto-increment read). |
| `+0x1c4 + (port * 8)` | **DMEMD** | DMEM data. Read/write 32-bit words sequentially. |

### 2.7 FBIF (Framebuffer Interface) Registers

| Offset | Register Name | Bits | Description |
|--------|--------------|------|-------------|
| `+0x600 + (idx * 4)` | **FBIF_TRANSCFG[idx]** | `[1:0]` target, `[2]` mem_type | Transfer config. target: 0=LocalFB, 1=CoherentSysmem, 2=NoncoherentSysmem. mem_type: 0=virtual, 1=physical. |
| `+0x604` | **FBIF_DMAIDX** | `[2:0]` idx | DMA index for virtual addressing (written to `0` for DMAIDX_VIRT). |
| `+0x624` | **FBIF_CTL** | `[7]` allow_phys_no_ctx | Allow physical addressing without a context. Must be set before DMA. |

### 2.8 Engine Reset Register

| Offset | Register Name | Bits | Description |
|--------|--------------|------|-------------|
| `+0x3c0` | **FALCON_ENGINE** | `[0]` reset | Engine reset. Write `1` to assert reset, then `0` to deassert. Known as `NV_PSEC_FALCON_ENGINE` (SEC2) or `NV_PGSP_FALCON_ENGINE` (GSP). |
| `+0x3e8` | **INTR_RETRIG** | `[0]` trigger | Interrupt retrigger (GA100+). |

### 2.9 BROM (Boot ROM) Registers — at `BASE + 0x1000` (addr2 block)

These registers are in the second falcon register block at `+0x1000` from the falcon base.

| Offset | Register Name | Bits | Description |
|--------|--------------|------|-------------|
| `+0x1180` | **MOD_SEL** | `[7:0]` algo | Signature algorithm selector. `1` = RSA-3K. |
| `+0x1198` | **BROM_CURR_UCODE_ID** | `[7:0]` ucode_id | Current ucode ID for signature verification. |
| `+0x119c` | **BROM_ENGIDMASK** | `[31:0]` value | Engine ID mask for the firmware being loaded. |
| `+0x1210` | **BROM_PARAADDR** | `[31:0]` value | PKC data offset — offset in DMEM where the firmware's RSA signature data is located. |

### 2.10 RISCV/Peregrine Core Select Registers — at `BASE + 0x1000` (addr2 block)

| Offset | Register Name | Bits | Description |
|--------|--------------|------|-------------|
| `+0x1388` | **RISCV_STATUS** | `[7]` active | RISC-V core active status (GA102+). |
| `+0x1668` | **RISCV_BCR_CTRL** | `[0]` valid, `[4]` core_select, `[8]` br_fetch | Boot/Core Reset control. core_select: 0=Falcon, 1=RISC-V. Write 0 to select Falcon, then poll `valid` bit. |

---

## 3. Fuse Registers for Signature Version Selection

These are absolute BAR0 addresses (not relative to falcon base):

| Address | Register Name | Description |
|---------|--------------|-------------|
| `0x824100 + (ucode_id - 1) * 4` | **NV_FUSE_OPT_FPF_NVDEC_UCODE_VERSION** | Fuse version for NVDEC ucodes (engine_id_mask bit 2). |
| `0x824140 + (ucode_id - 1) * 4` | **NV_FUSE_OPT_FPF_SEC2_UCODE_VERSION** | Fuse version for SEC2 ucodes (engine_id_mask bit 0). |
| `0x8241c0 + (ucode_id - 1) * 4` | **NV_FUSE_OPT_FPF_GSP_UCODE_VERSION** | Fuse version for GSP ucodes (engine_id_mask bit 10). |

### Signature Selection Algorithm

From `ga100_flcn_fw_signature()` and `ga102_gsp_fwsec_signature()`:

```
1. Read fuse register based on engine_id_mask:
   - engine_id_mask & 0x0001 → SEC2: read 0x824140 + (ucode_id - 1) * 4
   - engine_id_mask & 0x0004 → NVDEC: read 0x824100 + (ucode_id - 1) * 4
   - engine_id_mask & 0x0400 → GSP:  read 0x8241c0 + (ucode_id - 1) * 4

2. reg_fuse_version = read_fuse_register()
3. Compute the effective version: fls(reg_fuse_version) — "find last set" bit position
4. Compute signature index from sig_fuse_version (from firmware header):
   - idx = sig_fuse_version - reg_fuse_version  (ga100 algorithm)
   — or —
   - Walk bits until reg_fuse_version & sig_fuse_version & 1 (ga102 algorithm)
5. Use idx to select the correct signature from the signatures array
```

---

## 4. FWSEC Error/Status Verification Registers

Absolute BAR0 addresses:

| Address | Register | Description |
|---------|----------|-------------|
| `0x001400 + 0x38` (= `0x001438`) | **SCRATCH_E** | FWSEC-FRTS error. Read `>> 16`, non-zero = error. |
| `0x001400 + 0x54` (= `0x001454`) | **SCRATCH_15** | FWSEC-SB error. Read `& 0xffff`, non-zero = error. |
| `0x1fa824` | **WPR2_LO** | WPR2 region lower bound (set by FWSEC-FRTS). |
| `0x1fa828` | **WPR2_HI** | WPR2 region upper bound (set by FWSEC-FRTS). |

---

## 5. How to Load Firmware via DMA (GA102 — `ga102_flcn_fw_load` / nova-core `dma_load`)

### 5.1 DMA Setup Sequence

```
Step 1: Configure FBIF for physical system memory access
  WRITE BAR0[BASE + 0x624] |= BIT(7)        // FBIF_CTL: allow_phys_no_ctx = true
  WRITE BAR0[BASE + 0x10c] = 0x00000000     // DMACTL: clear all (no ctx required, no scrubbing)
  ALTER BAR0[BASE + 0x600]:                  // FBIF_TRANSCFG[0]:
    bits[1:0] = 1 (CoherentSysmem)           //   target = coherent system memory
    bit[2] = 1 (Physical)                    //   mem_type = physical addresses

Step 2: Load IMEM via DMA
  For each 256-byte chunk of IMEM data:
    WRITE BAR0[BASE + 0x110] = (dma_phys_addr >> 8)        // DMATRFBASE: source base
    WRITE BAR0[BASE + 0x128] = (dma_phys_addr >> 40)       // DMATRFBASE1: source base upper
    WRITE BAR0[BASE + 0x114] = imem_dst_offset + pos       // DMATRFMOFFS: falcon IMEM offset
    WRITE BAR0[BASE + 0x11c] = imem_src_offset + pos       // DMATRFFBOFFS: source offset
    WRITE BAR0[BASE + 0x118] = cmd                         // DMATRFCMD: trigger transfer
      where cmd = (0x6 << 8) | BIT(4) | (sec << 2)
      // size=256B, imem=1, sec=1 for secure code
    POLL  BAR0[BASE + 0x118] until BIT(1) set              // Wait for idle

Step 3: Load DMEM via DMA
  Same as IMEM but:
    cmd bit[4] = 0 (dmem, not imem)
    cmd bit[3:2] = 0 (non-secure for data)
    DMA base address includes DMEM source offset
```

### 5.2 DMA Command Word Encoding (GA102)

From `ga102_flcn_dma_init()`:
```c
*cmd = (ilog2(xfer_len) - 2) << 8;   // size field at bits [10:8]
if (mem_type == IMEM) *cmd |= 0x10;   // bit 4: target is IMEM
if (sec)             *cmd |= 0x04;    // bit 2: secure mode
```

For 256-byte transfers: `ilog2(256) - 2 = 6`, so `size = 6 << 8 = 0x600`.
- IMEM secure: `cmd = 0x614`
- DMEM non-secure: `cmd = 0x600`

---

## 6. How to Start/Stop/Reset the Falcon

### 6.1 Reset Sequence (GA102 — from `ga102_flcn_*` / nova-core `reset_eng`)

```
Step 1: Pre-reset wait
  READ  BAR0[BASE + 0x0f4]                    // HWCFG2: initial read
  POLL  BAR0[BASE + 0x0f4] bit[31] (150µs)    // Wait for reset_ready (may not set on Ampere HW bug)

Step 2: Assert and deassert engine reset
  WRITE BAR0[BASE + 0x3c0] bit[0] = 1         // FALCON_ENGINE: assert reset
  DELAY 10µs
  WRITE BAR0[BASE + 0x3c0] bit[0] = 0         // FALCON_ENGINE: deassert reset

Step 3: Wait for memory scrubbing to complete (GA102)
  POLL  BAR0[BASE + 0x0f4] until bit[12] = 0  // HWCFG2: mem_scrubbing clear (20ms timeout)

Step 4: Select Falcon core (GA102 dual falcon/riscv)
  READ  BAR0[BASE + 0x1668]                   // RISCV_BCR_CTRL
  IF bit[4] != 0 (core_select != Falcon):
    WRITE BAR0[BASE + 0x1668] = 0              // Select Falcon core
    POLL  BAR0[BASE + 0x1668] bit[0] (10ms)    // Wait for valid

Step 5: Wait for memory scrubbing again
  POLL  BAR0[BASE + 0x0f4] until bit[12] = 0  // HWCFG2: mem_scrubbing clear

Step 6: Write RM register
  WRITE BAR0[BASE + 0x084] = BAR0[0x000000]   // RM = NV_PMC_BOOT_0 value
```

### 6.2 Start CPU

From `nvkm_falcon_v1_start()`:
```
READ  BAR0[BASE + 0x100]                      // CPUCTL
IF bit[6] set (alias_en):
  WRITE BAR0[BASE + 0x130] = 0x2              // CPUCTL_ALIAS: startcpu
ELSE:
  WRITE BAR0[BASE + 0x100] = 0x2              // CPUCTL: startcpu
```

### 6.3 Boot and Wait for Completion

From `gm200_flcn_fw_boot()` / nova-core `boot()`:
```
Step 1: Set mailbox values
  WRITE BAR0[BASE + 0x040] = mbox0_value      // MAILBOX0
  WRITE BAR0[BASE + 0x044] = mbox1_value      // MAILBOX1

Step 2: Set boot vector
  WRITE BAR0[BASE + 0x104] = boot_addr        // BOOTVEC

Step 3: Start CPU
  WRITE BAR0[BASE + 0x100] = 0x2              // CPUCTL: startcpu

Step 4: Wait for halt
  POLL  BAR0[BASE + 0x100] until bit[4] set   // CPUCTL: halted (2s timeout)

Step 5: Read result mailboxes
  READ  BAR0[BASE + 0x040] → mbox0_result     // MAILBOX0
  READ  BAR0[BASE + 0x044] → mbox1_result     // MAILBOX1
```

---

## 7. Mailbox Register Protocol

### 7.1 FWSEC-FRTS

- **Input**: `MAILBOX0 = 0` (no special input needed; FRTS params are in DMEM)
- **Output**: Check `MAILBOX0` value. Then verify via scratch register `0x001438` (upper 16 bits should be 0).

### 7.2 Booter Load/Unload (SEC2)

- **Booter Load Input**: `MAILBOX0 = lower_32(wpr_meta_addr)`, `MAILBOX1 = upper_32(wpr_meta_addr)`
- **Booter Unload Input**: `MAILBOX0 = 0xff`, `MAILBOX1 = 0xff` (for non-suspend); or SR meta address for suspend.
- **Booter Output**: Check `MAILBOX0 == 0` for success.

### 7.3 GSP-RM Init

After booter completes, GSP starts in RISC-V mode:
- `MAILBOX0 = lower_32(libos_addr)` written to GSP falcon
- `MAILBOX1 = upper_32(libos_addr)` written to GSP falcon

---

## 8. Complete FWSEC-FRTS Execution Sequence

This is the full step-by-step sequence for loading and executing FWSEC on the GSP falcon in HS mode to set up the WPR2 (FRTS) region. Derived from `nvkm_gsp_fwsec_frts()`, `nvkm_gsp_fwsec_v3()`, `nvkm_gsp_fwsec_patch()`, `ga102_flcn_fw_load()`, `ga102_flcn_fw_boot()`, `gm200_flcn_fw_boot()`, and nova-core patches.

### 8.1 Prerequisites

- GPU has been initialized (GFW_BOOT complete).
- VBIOS ROM accessible.
- DMA-coherent system memory buffer allocated for the firmware image.
- FB layout computed (FRTS region address known).

### 8.2 Extract FWSEC from VBIOS

```
1. Scan VBIOS PMU ucode table for entry with app_id = 0x85 (FWSEC).
2. Read the FalconUCodeDesc header:
   - For v3 descriptors (GA102):
     - IMEMPhysBase, IMEMLoadSize → IMEM parameters
     - DMEMPhysBase, DMEMLoadSize → DMEM parameters
     - PKCDataOffset → signature location in DMEM
     - InterfaceOffset → DMEMMAPPER application interface offset
     - EngineIdMask, UcodeId → for fuse version lookup
     - SignatureCount, SignatureVersions → for signature selection
3. Copy IMEM+DMEM data into DMA-coherent buffer.
```

### 8.3 Select and Patch Signature

```
1. Read fuse register to get reg_fuse_version:
   For FWSEC on GSP (engine_id_mask & 0x0400):
     reg_fuse_version = READ BAR0[0x8241c0 + (ucode_id - 1) * 4]

2. Compute signature index:
   reg_fuse_version = BIT(fls(reg_fuse_version))
   Walk bits of sig_fuse_version (from VBIOS header) to find matching index.

3. Copy selected 384-byte RSA-3K signature into the firmware image at PKCDataOffset
   within the DMEM section.
```

### 8.4 Patch DMEM with FRTS Command

```
1. Locate DMEMMAPPER interface in DMEM (at InterfaceOffset from DMEM base):
   - Parse FalconAppifHdr (version=1, header_size=4, entry_size=8)
   - Find entry with id = 0x04 (NVFW_FALCON_APPIF_ID_DMEMMAPPER)
   - Read dmem_base from that entry

2. Set init_cmd in DMEMMAPPER to FRTS command:
   dmemmapper.init_cmd = 0x15  (NVFW_FALCON_APPIF_DMEMMAPPER_CMD_FRTS)

3. Write FRTS command data into cmd_in_buffer:
   cmd_in_buffer = {
     read_vbios: {
       ver: 1,
       hdr: sizeof(read_vbios) = 24,
       addr: 0,
       size: 0,
       flags: 2
     },
     frts_region: {
       ver: 1,
       hdr: sizeof(frts_region) = 20,
       addr: frts_fb_addr >> 12,     // Page-aligned FRTS address in VRAM
       size: frts_size >> 12,        // 0x100000 >> 12 = 0x100 pages
       type: 2                       // NVFW_FRTS_CMD_REGION_TYPE_FB
     }
   }
```

### 8.5 Reset GSP Falcon

```
// Full reset sequence for GSP falcon at BASE=0x110000:

1. WRITE BAR0[0x110048] = 0x00000000         // IRQMODE: disable IRQ delivery
2. WRITE BAR0[0x110014] = 0xffffffff         // IRQMASK: mask all interrupts

3. READ  BAR0[0x1100f4]                      // HWCFG2: initial read
4. POLL  BAR0[0x1100f4] bit[31] (~150µs)     // Wait for reset_ready

5. Call gp102_flcn_reset_eng:
   This uses the PMC engine reset mechanism for the GSP engine.

6. WRITE BAR0[0x1110f4] bit[12] check        // Wait mem scrub (GA102: check 0x0f4, not 0x10c)
   POLL  BAR0[0x1100f4] until bit[12] = 0    // HWCFG2: mem_scrubbing clear

7. Select Falcon core (GA102):
   READ  BAR0[0x111668]                      // RISCV_BCR_CTRL
   IF bit[4] != 0:
     WRITE BAR0[0x111668] = 0                // Select Falcon
     POLL  BAR0[0x111668] bit[0] (10ms)      // Wait for valid

8. WRITE BAR0[0x110084] = BAR0[0x000000]     // RM = PMC_BOOT_0
```

### 8.6 Load FWSEC via DMA onto GSP Falcon

```
// Configure DMA aperture
1. WRITE BAR0[0x110624] |= BIT(7)            // FBIF_CTL: allow_phys_no_ctx
2. WRITE BAR0[0x11010c] = 0x00000000         // DMACTL: clear
3. ALTER BAR0[0x110600]:                      // FBIF_TRANSCFG[0]:
     bits[1:0] = 1, bit[2] = 1               // CoherentSysmem, Physical

// Load IMEM (secure code)
4. For each 256-byte chunk at position `pos`:
   WRITE BAR0[0x110110] = (dma_addr >> 8)    // DMATRFBASE
   WRITE BAR0[0x110128] = 0                  // DMATRFBASE1 (usually 0 for < 1TB)
   WRITE BAR0[0x110114] = imem_base + pos    // DMATRFMOFFS: destination in IMEM
   WRITE BAR0[0x11011c] = imem_src + pos     // DMATRFFBOFFS: source offset
   WRITE BAR0[0x110118] = 0x00000614         // DMATRFCMD: size=256B, imem=1, sec=1
   POLL  BAR0[0x110118] bit[1] until set     // Wait for idle

// Load DMEM (data)
5. For each 256-byte chunk at position `pos`:
   WRITE BAR0[0x110110] = ((dma_addr + dmem_src) >> 8)  // DMATRFBASE (includes src offset)
   WRITE BAR0[0x110128] = 0                              // DMATRFBASE1
   WRITE BAR0[0x110114] = dmem_base + pos                // DMATRFMOFFS: destination in DMEM
   WRITE BAR0[0x11011c] = pos                            // DMATRFFBOFFS: offset from base
   WRITE BAR0[0x110118] = 0x00000600                     // DMATRFCMD: size=256B, dmem, no sec
   POLL  BAR0[0x110118] bit[1] until set                 // Wait for idle
```

### 8.7 Program BROM Registers (GA102)

```
// Write PKC/signature parameters for BROM verification

1. WRITE BAR0[0x111210] = pkc_data_offset    // BROM_PARAADDR: sig offset in DMEM
2. WRITE BAR0[0x11119c] = engine_id_mask     // BROM_ENGIDMASK: engine mask (0x0400 for GSP)
3. WRITE BAR0[0x111198] = ucode_id           // BROM_CURR_UCODE_ID
4. WRITE BAR0[0x111180] = 0x01               // MOD_SEL: algo = RSA-3K
```

### 8.8 Set Boot Vector and Mailboxes

```
1. WRITE BAR0[0x110104] = 0x00000000         // BOOTVEC: boot address (0 for FWSEC)

2. WRITE BAR0[0x110040] = 0x00000000         // MAILBOX0: input parameter

// For GA102, additional BROM boot registers:
3. WRITE BAR0[0x111210] = dmem_sign          // BROM_PARAADDR[0]: signature offset (already done)
4. WRITE BAR0[0x11119c] = engine_id          // BROM_ENGIDMASK (already done)
5. WRITE BAR0[0x111198] = ucode_id           // BROM_CURR_UCODE_ID (already done)
6. WRITE BAR0[0x111180] = 0x00000001         // MOD_SEL BROM_BOOT_TRIGGER
```

### 8.9 Start Execution and Wait

```
1. WRITE BAR0[0x110100] = 0x00000002         // CPUCTL: startcpu

2. POLL  BAR0[0x110100] bit[4] (2s timeout)  // Wait for halted

3. READ  BAR0[0x110040] → mbox0              // MAILBOX0: result
4. READ  BAR0[0x110044] → mbox1              // MAILBOX1: result
```

### 8.10 Verify FWSEC-FRTS Success

```
1. READ  BAR0[0x001438] → scratch_e          // SCRATCH register 0xe
   IF (scratch_e >> 16) != 0:
     ERROR: FWSEC-FRTS failed with code (scratch_e >> 16)

2. READ  BAR0[0x1fa824] → wpr2_lo            // WPR2 lower bound
3. READ  BAR0[0x1fa828] → wpr2_hi            // WPR2 upper bound
   // Success: WPR2 region is set up at [wpr2_lo, wpr2_hi]
```

---

## 9. Complete Booter Load Sequence (SEC2 Falcon)

The Booter is loaded onto the SEC2 falcon to authenticate and boot GSP-RM.

### 9.1 Reset SEC2 Falcon

Same sequence as GSP but at `BASE = 0x840000`:
```
// SEC2 also uses PMC reset (reset_pmc = true)
1. ga102_flcn_select: Check/set BAR0[0x841668] for Falcon core
2. ga102_flcn_reset_prep: Pre-reset wait on BAR0[0x8400f4]
3. PMC engine reset for SEC2
4. ga102_flcn_reset_wait_mem_scrubbing: Poll BAR0[0x8400f4] bit[12]
5. WRITE BAR0[0x840084] = BAR0[0x000000]   // RM = PMC_BOOT_0
```

### 9.2 Load Booter via DMA

Same DMA sequence as FWSEC but targeting SEC2 at `BASE = 0x840000`:
```
1. Configure FBIF at 0x840600, 0x840624
2. DMA IMEM chunks to 0x840114/0x840118
3. DMA DMEM chunks
4. Program BROM at 0x841210, 0x84119c, 0x841198, 0x841180
```

### 9.3 Boot Booter

```
1. WRITE BAR0[0x840040] = lower_32(wpr_meta_addr)  // MAILBOX0
2. WRITE BAR0[0x840044] = upper_32(wpr_meta_addr)  // MAILBOX1
3. WRITE BAR0[0x840104] = boot_addr                 // BOOTVEC
4. WRITE BAR0[0x840100] = 0x2                       // CPUCTL: start
5. POLL  BAR0[0x840100] bit[4] until set             // Wait for halt
6. CHECK BAR0[0x840040] == 0                         // MAILBOX0 = success
```

---

## 10. Memory Layout for FRTS Region Computation

From `tu102_gsp_oneinit()` and nova-core `FbLayout`:

```
FB end (top of VRAM)
│
├── VGA Workspace        @ fb_size - 0x100000 (or from NV_PDISP_VGA_WORKSPACE_BASE)
│   Size: fb_size - vga_base
│
├── BIOS region          = VGA Workspace (same range)
│
├── FRTS region          @ ALIGN_DOWN(vga_base, 0x20000) - 0x100000
│   Size: 0x100000 (1MB)
│
├── Boot firmware        @ ALIGN_DOWN(frts_base, 0x1000) - boot_fw_size
│
├── GSP-RM ELF           @ ALIGN_DOWN(boot_base, 0x10000) - elf_size
│
├── WPR2 Heap            @ ALIGN_DOWN(elf_base, 0x100000) - heap_size
│
├── WPR2 Meta            @ ALIGN_DOWN(heap_base, 0x100000) - sizeof(GspFwWprMeta)
│
├── Non-WPR Heap         @ wpr2_base - 0x100000
│   Size: 0x100000
│
FB base (0)
```

### VRAM Size Detection (GA102)

```
For GA102+:
  vidmem_size = (READ BAR0[0x001183a4] as u64) << 20   // NV_PGC6_AON_SECURE_SCRATCH_GROUP_42

For pre-GA102:
  reg = READ BAR0[0x00100ce0]                           // NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE
  size = (reg[9:4]) << (reg[3:0] + 20)
  if reg[30] (ECC): size = size / 16 * 15
```

---

## 11. FWSEC DMEM Structures

### 11.1 FWSEC Application Interface (DMEMMAPPER v3)

```c
struct FalconAppifDmemmapperV3 {
    u32 signature;            // "DMAP" = 0x50414d44
    u16 version;              // 0x0003
    u16 size;                 // 64
    u32 cmd_in_buffer_offset; // Offset in DMEM for command input
    u32 cmd_in_buffer_size;
    u32 cmd_out_buffer_offset;
    u32 cmd_out_buffer_size;
    u32 nvf_img_data_buffer_offset;
    u32 nvf_img_data_buffer_size;
    u32 printf_buffer_hdr;
    u32 ucode_build_time_stamp;
    u32 ucode_signature;
    u32 init_cmd;             // Set to 0x15 for FRTS, 0x19 for SB
    u32 ucode_feature;
    u32 ucode_cmd_mask0;
    u32 ucode_cmd_mask1;
    u32 multi_tgt_tbl;
};
```

### 11.2 FWSEC-FRTS Command Structure

```c
struct nvfw_fwsec_frts_cmd {
    struct {
        u32 ver;              // 1
        u32 hdr;              // sizeof(read_vbios) = 24
        u64 addr;             // 0 (not used for FRTS)
        u32 size;             // 0 (not used for FRTS)
        u32 flags;            // 2
    } read_vbios;
    struct {
        u32 ver;              // 1
        u32 hdr;              // sizeof(frts_region) = 20
        u32 addr;             // frts_addr >> 12 (page number)
        u32 size;             // frts_size >> 12 (page count, typically 0x100)
        u32 type;             // 2 = NVFW_FRTS_CMD_REGION_TYPE_FB
    } frts_region;
};
```

### 11.3 FWSEC Ucode Descriptor v3 (from VBIOS)

```c
struct FalconUCodeDescV3 {
    u32 Hdr;                  // bit[0]=valid, bits[15:8]=version(3), bits[31:16]=header_size
    u32 StoredSize;
    u32 PKCDataOffset;        // Offset of RSA-3K signature data in DMEM
    u32 InterfaceOffset;      // Offset of DMEMMAPPER interface in DMEM
    u32 IMEMPhysBase;         // Physical base address for IMEM load
    u32 IMEMLoadSize;         // Size of IMEM section to load
    u32 IMEMVirtBase;         // Virtual base of IMEM
    u32 DMEMPhysBase;         // Physical base address for DMEM load
    u32 DMEMLoadSize;         // Size of DMEM section to load
    u16 EngineIdMask;         // Bitmask: 0x0400 = GSP, 0x0001 = SEC2, 0x0004 = NVDEC
    u8  UcodeId;              // ID for fuse version register lookup (1-16)
    u8  SignatureCount;       // Number of RSA-3K signatures
    u16 SignatureVersions;    // Bitmask of supported signature versions
    u16 Reserved;
};
// Followed immediately by: SignatureCount * 384 bytes of RSA-3K signatures
// Followed by: IMEM data (IMEMLoadSize bytes)
// Followed by: DMEM data (DMEMLoadSize bytes)
```

---

## 12. Quick Reference: Absolute BAR0 Addresses for GA102

### GSP Falcon (BASE = 0x110000)

| Absolute Address | Register | Purpose |
|-----------------|----------|---------|
| `0x110004` | GSP IRQSCLR | Clear interrupts |
| `0x110040` | GSP MAILBOX0 | Mailbox 0 |
| `0x110044` | GSP MAILBOX1 | Mailbox 1 |
| `0x110084` | GSP RM | Resource manager |
| `0x1100f4` | GSP HWCFG2 | HW config (scrub status, reset ready) |
| `0x110100` | GSP CPUCTL | CPU start/halt |
| `0x110104` | GSP BOOTVEC | Boot vector |
| `0x11010c` | GSP DMACTL | DMA control |
| `0x110110` | GSP DMATRFBASE | DMA base address |
| `0x110114` | GSP DMATRFMOFFS | DMA memory offset |
| `0x110118` | GSP DMATRFCMD | DMA command |
| `0x11011c` | GSP DMATRFFBOFFS | DMA FB offset |
| `0x110128` | GSP DMATRFBASE1 | DMA base address upper |
| `0x11012c` | GSP HWCFG1 | HW config (revision, security) |
| `0x110130` | GSP CPUCTL_ALIAS | CPU control alias |
| `0x1103c0` | GSP FALCON_ENGINE | Engine reset |
| `0x110600` | GSP FBIF_TRANSCFG | FBIF transfer config |
| `0x110624` | GSP FBIF_CTL | FBIF control |
| `0x111180` | GSP MOD_SEL | Signature algorithm |
| `0x111198` | GSP BROM_CURR_UCODE_ID | Current ucode ID |
| `0x11119c` | GSP BROM_ENGIDMASK | Engine ID mask |
| `0x111210` | GSP BROM_PARAADDR | PKC data address |
| `0x111668` | GSP RISCV_BCR_CTRL | RISC-V/Falcon core select |

### SEC2 Falcon (BASE = 0x840000)

| Absolute Address | Register | Purpose |
|-----------------|----------|---------|
| `0x840004` | SEC2 IRQSCLR | Clear interrupts |
| `0x840040` | SEC2 MAILBOX0 | Mailbox 0 |
| `0x840044` | SEC2 MAILBOX1 | Mailbox 1 |
| `0x840084` | SEC2 RM | Resource manager |
| `0x8400f4` | SEC2 HWCFG2 | HW config |
| `0x840100` | SEC2 CPUCTL | CPU start/halt |
| `0x840104` | SEC2 BOOTVEC | Boot vector |
| `0x84010c` | SEC2 DMACTL | DMA control |
| `0x840110` | SEC2 DMATRFBASE | DMA base address |
| `0x840114` | SEC2 DMATRFMOFFS | DMA memory offset |
| `0x840118` | SEC2 DMATRFCMD | DMA command |
| `0x84011c` | SEC2 DMATRFFBOFFS | DMA FB offset |
| `0x840128` | SEC2 DMATRFBASE1 | DMA base address upper |
| `0x84012c` | SEC2 HWCFG1 | HW config |
| `0x840130` | SEC2 CPUCTL_ALIAS | CPU control alias |
| `0x8403c0` | SEC2 FALCON_ENGINE | Engine reset |
| `0x8403e0` | SEC2 INTR_VECTOR | Interrupt vector register |
| `0x840600` | SEC2 FBIF_TRANSCFG | FBIF transfer config |
| `0x840624` | SEC2 FBIF_CTL | FBIF control |
| `0x841180` | SEC2 MOD_SEL | Signature algorithm |
| `0x841198` | SEC2 BROM_CURR_UCODE_ID | Current ucode ID |
| `0x84119c` | SEC2 BROM_ENGIDMASK | Engine ID mask |
| `0x841210` | SEC2 BROM_PARAADDR | PKC data address |
| `0x841668` | SEC2 RISCV_BCR_CTRL | RISC-V/Falcon core select |

### Other Important Absolute Addresses

| Address | Register | Purpose |
|---------|----------|---------|
| `0x000000` | NV_PMC_BOOT_0 | GPU boot ID / chipset identifier |
| `0x001438` | SCRATCH_E | FWSEC-FRTS error status (bits [31:16]) |
| `0x001454` | SCRATCH_15 | FWSEC-SB error status (bits [15:0]) |
| `0x00100ce0` | NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE | VRAM size (pre-GA102) |
| `0x001183a4` | NV_PGC6_AON_SECURE_SCRATCH_42 | VRAM size in MB (GA102+) |
| `0x00118128` | GFW_BOOT_STATUS | GFW boot status check |
| `0x00118234` | GFW_BOOT_PROGRESS | GFW boot progress (0xff = done) |
| `0x00625f04` | NV_PDISP_VGA_WORKSPACE_BASE | VGA workspace address |
| `0x00820c04` | NV_FUSE_STATUS_OPT_DISPLAY | Display fuse (Ampere) |
| `0x00824100` | NV_FUSE_OPT_FPF_NVDEC_UCODE1_VERSION | NVDEC fuse version base |
| `0x00824140` | NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION | SEC2 fuse version base |
| `0x008241c0` | NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION | GSP fuse version base |
| `0x001fa824` | WPR2_LO | WPR2 region lower bound |
| `0x001fa828` | WPR2_HI | WPR2 region upper bound |
