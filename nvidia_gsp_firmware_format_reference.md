# NVIDIA GSP Firmware Binary Format Reference (GA102 / Ampere)

Comprehensive reference for parsing NVIDIA GSP firmware binary blobs, derived from the
Linux kernel nouveau driver (`drivers/gpu/drm/nouveau/include/nvfw/`), the nova-core
Rust driver (`drivers/gpu/nova-core/`), and the NVIDIA open-gpu-kernel-modules source.

All multi-byte fields are **little-endian**. All offsets are in bytes unless noted.

---

## Table of Contents

1. [Common Firmware Header (BinHdr / nvfw_bin_hdr)](#1-common-firmware-header)
2. [Bootloader Descriptor (nvfw_bl_desc)](#2-bootloader-descriptor)
3. [Heavy-Secured Firmware Header V2 (nvfw_hs_header_v2)](#3-heavy-secured-firmware-header-v2)
4. [Heavy-Secured Load Header V2 (nvfw_hs_load_header_v2)](#4-heavy-secured-load-header-v2)
5. [Heavy-Secured Firmware Header V1 (nvfw_hs_header)](#5-heavy-secured-firmware-header-v1)
6. [Heavy-Secured Load Header V1 (nvfw_hs_load_header)](#6-heavy-secured-load-header-v1)
7. [Booter Firmware Format (booter_load / booter_unload)](#7-booter-firmware-format)
8. [GSP Bootloader Format (bootloader-VERSION.bin)](#8-gsp-bootloader-format)
9. [GSP Firmware Format (gsp-VERSION.bin)](#9-gsp-firmware-format)
10. [FWSEC from VBIOS](#10-fwsec-from-vbios)
11. [Falcon Ucode Descriptors (V2/V3)](#11-falcon-ucode-descriptors)
12. [VBIOS ROM Layout and Parsing](#12-vbios-rom-layout-and-parsing)
13. [Application Interface Structures](#13-application-interface-structures)
14. [ACR / WPR Structures](#14-acr--wpr-structures)
15. [Falcon Bootloader DMEM Descriptors](#15-falcon-bootloader-dmem-descriptors)
16. [Boot Flow Summary](#16-boot-flow-summary)

---

## 1. Common Firmware Header

**Source:** `drivers/gpu/drm/nouveau/include/nvfw/fw.h`, `drivers/gpu/nova-core/firmware.rs`

All firmware files loaded from userspace (`booter_load-VERSION.bin`, `booter_unload-VERSION.bin`,
`bootloader-VERSION.bin`) start with this common header. The GSP firmware (`gsp-VERSION.bin`)
is an ELF file and does NOT use this header.

### struct nvfw_bin_hdr / BinHdr

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `bin_magic` | Magic number. Must be `0x10DE` (NVIDIA vendor ID) |
| 0x04 | 4 | `bin_ver` | Version of the header format |
| 0x08 | 4 | `bin_size` | Total size in bytes of the binary (to be ignored per nova-core) |
| 0x0C | 4 | `header_offset` | Byte offset from start of file to the application-specific header |
| 0x10 | 4 | `data_offset` | Byte offset from start of file to the data payload |
| 0x14 | 4 | `data_size` | Size in bytes of the data payload |

**Total size:** 24 bytes (0x18)

**Validation:** Check that `bin_magic == 0x10DE`.

### struct nvfw_bl_desc

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `start_tag` | Start tag |
| 0x04 | 4 | `dmem_load_off` | DMEM load offset |
| 0x08 | 4 | `code_off` | Code offset |
| 0x0C | 4 | `code_size` | Code size |
| 0x10 | 4 | `data_off` | Data offset |
| 0x14 | 4 | `data_size` | Data size |

**Total size:** 24 bytes (0x18)

---

## 2. Bootloader Descriptor

See `nvfw_bl_desc` above. This describes where code/data are within the firmware payload
for loading into falcon IMEM/DMEM.

---

## 3. Heavy-Secured Firmware Header V2

**Source:** `drivers/gpu/drm/nouveau/include/nvfw/hs.h`

Used by `booter_load-VERSION.bin` and `booter_unload-VERSION.bin` on Turing/Ampere.
Located at `BinHdr.header_offset` within the firmware file.

### struct nvfw_hs_header_v2 / HsHeaderV2

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `sig_prod_offset` | Offset to the start of the production signatures section |
| 0x04 | 4 | `sig_prod_size` | Total size in bytes of all production signatures |
| 0x08 | 4 | `patch_loc` | Offset to a u32 containing the DMEM offset where to patch the signature |
| 0x0C | 4 | `patch_sig` | Offset to a u32 containing the base index into the signatures array |
| 0x10 | 4 | `meta_data_offset` | Offset to the signature metadata (HsSignatureParams) |
| 0x14 | 4 | `meta_data_size` | Size in bytes of the signature metadata |
| 0x18 | 4 | `num_sig` | Offset to a u32 containing the number of signatures |
| 0x1C | 4 | `header_offset` | Offset of the application-specific header (HsLoadHeaderV2) |
| 0x20 | 4 | `header_size` | Size in bytes of the application-specific header |

**Total size:** 36 bytes (0x24)

**Important:** `patch_loc`, `patch_sig`, and `num_sig` are NOT direct values — they are
**offsets into the firmware data** to u32 values that contain the actual parameters.

### struct HsSignatureParams

Located at `nvfw_hs_header_v2.meta_data_offset` in the firmware data:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `fuse_ver` | Fuse version for signature selection |
| 0x04 | 4 | `engine_id_mask` | Bitmask of falcon engines that can run this firmware |
| 0x08 | 4 | `ucode_id` | Microcode ID used for fuse register lookup |

**Total size:** 12 bytes (0x0C)

---

## 4. Heavy-Secured Load Header V2

**Source:** `drivers/gpu/drm/nouveau/include/nvfw/hs.h`

Located at `nvfw_hs_header_v2.header_offset` within the firmware data.

### struct nvfw_hs_load_header_v2 / HsLoadHeaderV2

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `os_code_offset` | Offset where the code (IMEM data) starts within the payload |
| 0x04 | 4 | `os_code_size` | Total size of the code section, for all apps |
| 0x08 | 4 | `os_data_offset` | Offset where the data (DMEM data) starts within the payload |
| 0x0C | 4 | `os_data_size` | Size of the data section |
| 0x10 | 4 | `num_apps` | Number of HsLoadHeaderV2App entries following this header |

**Total size:** 20 bytes (0x14), followed by `num_apps` app entries

### struct HsLoadHeaderV2App (per-app entry)

Immediately follows HsLoadHeaderV2, repeated `num_apps` times:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `offset` | Offset at which to load the app code (within IMEM) |
| 0x04 | 4 | `size` | Length in bytes of the app code |
| 0x08 | 4 | `data_offset` | Offset at which to load the app data |
| 0x0C | 4 | `data_size` | Size in bytes of the app data |

**Total size per entry:** 16 bytes (0x10)

---

## 5. Heavy-Secured Firmware Header V1

**Source:** `drivers/gpu/drm/nouveau/include/nvfw/hs.h`

Older format, used on pre-Turing GPUs.

### struct nvfw_hs_header

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `sig_dbg_offset` | Offset to debug signatures |
| 0x04 | 4 | `sig_dbg_size` | Size of debug signatures |
| 0x08 | 4 | `sig_prod_offset` | Offset to production signatures |
| 0x0C | 4 | `sig_prod_size` | Size of production signatures |
| 0x10 | 4 | `patch_loc` | Patch location |
| 0x14 | 4 | `patch_sig` | Patch signature offset |
| 0x18 | 4 | `hdr_offset` | Application header offset |
| 0x1C | 4 | `hdr_size` | Application header size |

**Total size:** 32 bytes (0x20)

---

## 6. Heavy-Secured Load Header V1

### struct nvfw_hs_load_header

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `non_sec_code_off` | Non-secure code offset |
| 0x04 | 4 | `non_sec_code_size` | Non-secure code size |
| 0x08 | 4 | `data_dma_base` | DMA base for data |
| 0x0C | 4 | `data_size` | Data size |
| 0x10 | 4 | `num_apps` | Number of apps |
| 0x14 | 4*N | `apps[]` | Flexible array of app offsets |

---

## 7. Booter Firmware Format

**Files:** `booter_load-VERSION.bin`, `booter_unload-VERSION.bin`
**Target:** SEC2 falcon
**Source:** `drivers/gpu/nova-core/firmware/booter.rs`

Booter is a Heavy-Secured firmware that runs on the SEC2 falcon core. It is responsible
for loading and running the RISC-V GSP bootloader into the GSP core.

### File Layout

```
+===================================================+
|  BinHdr (24 bytes)                                |
|    bin_magic = 0x10DE                             |
|    header_offset --> HsHeaderV2                    |
|    data_offset / data_size --> payload             |
+===================================================+
|  ... padding ...                                   |
+===================================================+
|  HsHeaderV2 (36 bytes)  @ BinHdr.header_offset    |
|    sig_prod_offset --> signatures section           |
|    patch_loc --> u32 (DMEM patch location)          |
|    patch_sig --> u32 (signature base index)         |
|    num_sig --> u32 (signature count)                |
|    meta_data_offset --> HsSignatureParams           |
|    header_offset --> HsLoadHeaderV2                 |
+===================================================+
|  HsSignatureParams (12 bytes) @ meta_data_offset   |
+===================================================+
|  HsLoadHeaderV2 (20+ bytes) @ hs_hdr.header_offset|
|    os_code_offset, os_code_size                    |
|    os_data_offset, os_data_size                    |
|    num_apps                                        |
|  +----------------------------------------------+ |
|  |  HsLoadHeaderV2App[0]  (16 bytes)            | |
|  |    offset, size (code load params)            | |
|  +----------------------------------------------+ |
|  |  HsLoadHeaderV2App[1..N-1]                    | |
|  +----------------------------------------------+ |
+===================================================+
|  Signatures Section @ sig_prod_offset              |
|    N signatures, each sig_prod_size/N bytes        |
+===================================================+
|  Data Payload @ BinHdr.data_offset                 |
|    +--------------------------------------------+  |
|    |  IMEM (code) section                       |  |
|    |    starts at app[0].offset                 |  |
|    |    length = app[0].size                    |  |
|    +--------------------------------------------+  |
|    |  DMEM (data) section                       |  |
|    |    starts at os_data_offset                |  |
|    |    length = os_data_size                   |  |
|    +--------------------------------------------+  |
+===================================================+
```

### Parsing Steps

1. Read `BinHdr` from offset 0. Validate `bin_magic == 0x10DE`.
2. Read `HsHeaderV2` from `BinHdr.header_offset`.
3. Read `HsLoadHeaderV2` from `HsHeaderV2.header_offset`.
4. Read `HsLoadHeaderV2App[0]` immediately after `HsLoadHeaderV2`.
5. The data payload (to be DMA-mapped) is at `BinHdr.data_offset`, size `BinHdr.data_size`.
6. Within that payload:
   - IMEM: `app[0].offset` to `app[0].offset + app[0].size`
   - DMEM: `os_data_offset` to `os_data_offset + os_data_size`
7. Read `*(u32*)(fw + patch_loc)` to get the DMEM offset where to patch the signature.
8. Read `*(u32*)(fw + num_sig)` to get the number of available signatures.
9. Read `*(u32*)(fw + patch_sig)` to get the base offset into the signatures array.
10. Select the correct signature based on `HsSignatureParams.fuse_ver` vs. the hardware
    fuse register value.
11. Copy the selected signature into the payload at the patch location.

### booter_load vs booter_unload

- **booter_load**: Loads the GSP bootloader + GSP firmware onto the GSP RISC-V core. Required for boot.
- **booter_unload**: Used to cleanly shut down the GSP. Runs on SEC2 to unload GSP. Optional during boot.

Both have identical binary formats. The difference is which SEC2 microcode is contained.

---

## 8. GSP Bootloader Format

**File:** `bootloader-VERSION.bin`
**Target:** GSP RISC-V core (loaded by Booter via SEC2)
**Source:** `drivers/gpu/nova-core/firmware/riscv.rs`

The GSP bootloader is a RISC-V firmware responsible for verifying, loading, and
starting the actual GSP firmware.

### File Layout

```
+===================================================+
|  BinHdr (24 bytes)                                |
|    bin_magic = 0x10DE                             |
|    header_offset --> RmRiscvUCodeDesc              |
|    data_offset / data_size --> payload             |
+===================================================+
|  RmRiscvUCodeDesc @ BinHdr.header_offset           |
|    (56 bytes, 14 x u32 fields)                    |
+===================================================+
|  Data Payload @ BinHdr.data_offset                 |
|    The actual RISC-V bootloader binary             |
+===================================================+
```

### struct RmRiscvUCodeDesc

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `version` | Descriptor version |
| 0x04 | 4 | `bootloader_offset` | Offset to bootloader within payload |
| 0x08 | 4 | `bootloader_size` | Size of bootloader |
| 0x0C | 4 | `bootloader_param_offset` | Offset to bootloader parameters |
| 0x10 | 4 | `bootloader_param_size` | Size of bootloader parameters |
| 0x14 | 4 | `riscv_elf_offset` | Offset to RISC-V ELF within payload |
| 0x18 | 4 | `riscv_elf_size` | Size of RISC-V ELF |
| 0x1C | 4 | `app_version` | Application version number |
| 0x20 | 4 | `manifest_offset` | Offset to manifest (for verification) |
| 0x24 | 4 | `manifest_size` | Size of manifest |
| 0x28 | 4 | `monitor_data_offset` | Offset to monitor data section |
| 0x2C | 4 | `monitor_data_size` | Size of monitor data |
| 0x30 | 4 | `monitor_code_offset` | Offset to monitor code section |
| 0x34 | 4 | `monitor_code_size` | Size of monitor code |

**Total size:** 56 bytes (0x38)

### Parsed Fields Used by Driver

From `RiscvFirmware` in nova-core:
- `code_offset` = `monitor_code_offset`
- `data_offset` = `monitor_data_offset`
- `manifest_offset` = `manifest_offset`
- `app_version` = `app_version`
- `ucode` = DMA-mapped copy of `fw[data_offset..data_offset+data_size]`

---

## 9. GSP Firmware Format

**File:** `gsp-VERSION.bin` (e.g., `gsp_ga10x.bin` or `nvidia/535.113.01/gsp_ga10x.bin`)
**Source:** `drivers/gpu/nova-core/firmware/gsp.rs`

### Format: ELF64

The GSP firmware is packaged as a standard **ELF64 binary**. It is NOT a raw blob
and does NOT use the `BinHdr` format.

### ELF Sections

| Section Name | Description |
|-------------|-------------|
| `.fwimage` | The actual GSP firmware binary image |
| `.fwsignature_ga10x` | Signatures for GA10x (Ampere) chipsets |
| `.fwsignature_tu10x` | Signatures for TU10x (Turing) chipsets |

The section name for signatures depends on the GPU architecture:
- Ampere: `.fwsignature_ga10x`
- Turing: `.fwsignature_tu10x`

### Parsing Steps

1. Parse the ELF64 header (`Elf64_Ehdr`):
   - `e_shoff`: offset to section header table
   - `e_shnum`: number of section headers
   - `e_shstrndx`: index of string table section header
2. Read all section headers (`Elf64_Shdr[]`).
3. Read the section name string table from section `e_shstrndx`.
4. Find section `.fwimage` by name. Extract its data — this is the firmware binary.
5. Find section `.fwsignature_ga10x` (for Ampere). Extract — these are the signatures.

### Radix-3 Page Table

The GSP bootloader does NOT accept the firmware as a flat DMA buffer. Instead, it
expects a **3-level page table** (radix-3) that maps the firmware at virtual address 0
in GSP's address space:

```
Level 0:  1 page (4KB), 1 entry (8 bytes)
  └─> Points to first Level 1 page

Level 1:  N pages, each containing 512 entries (8 bytes each)
  └─> Each entry points to a Level 2 page

Level 2:  M pages, each containing 512 entries (8 bytes each)
  └─> Each entry points to a 4KB page of firmware data
```

- Page size: 4096 bytes (4KB)
- Entry size: 8 bytes (u64 DMA address, little-endian)
- Entries per page: 512 (4096 / 8)

The GSP bootloader is given the DMA address of the Level 0 page table.

---

## 10. FWSEC from VBIOS

**Source:** `drivers/gpu/nova-core/firmware/fwsec.rs`, `drivers/gpu/nova-core/vbios.rs`
**Target:** GSP falcon (Heavy-Secure mode)

FWSEC is NOT a file on disk. It is extracted from the GPU's VBIOS ROM image (mapped
at BAR0 + 0x300000).

### Extraction Flow

1. Read VBIOS ROM from BAR0 + 0x300000 (up to 1MB scan limit).
2. Parse the chain of PCI ROM images (see Section 12).
3. Find the **second FwSec image** (type 0xE0) — this contains the FWSEC ucode.
4. In the **PciAt image** (type 0x00), find the BIT table and locate the Falcon Data token.
5. The Falcon Data pointer gives an offset to the PMU Lookup Table (in the FwSec image).
6. In the PMU Lookup Table, find the entry with `app_id = 0x85` (FWSEC_PROD).
7. The entry's `data` field points to the `FalconUCodeDescV3` header.
8. The descriptor header is followed by signatures, IMEM (code), and DMEM (data).

### FWSEC Binary Layout Within VBIOS

```
FwSec Image (Type 0xE0):
+===================================================+
|  ROM Header (signature 0xAA55)                     |
|  PCIR Structure (signature "PCIR")                 |
|  NPDE Structure (signature "NPDE")                 |
+===================================================+
|  PMU Lookup Table @ falcon_data_offset             |
|  +----------------------------------------------+ |
|  | PmuLookupTableHeader (4 bytes)                | |
|  |   version: u8                                 | |
|  |   header_len: u8                              | |
|  |   entry_len: u8                               | |
|  |   entry_count: u8                             | |
|  +----------------------------------------------+ |
|  | PmuLookupTableEntry[0] (6 bytes per entry)    | |
|  |   application_id: u8                          | |
|  |   target_id: u8                               | |
|  |   data: u32 (absolute offset to ucode desc)  | |
|  +----------------------------------------------+ |
|  | ... more entries ...                          | |
|  +----------------------------------------------+ |
+===================================================+
|  FalconUCodeDescV3 @ falcon_ucode_offset           |
|    (see Section 11)                                |
+===================================================+
|  Signatures Section (immediately after desc)       |
|    signature_count * 384 bytes each (RSA-3K)       |
+===================================================+
|  IMEM Section (Code)                               |
|    starts at: desc_end + sigs_size                 |
|    size: desc.imem_load_size                       |
+===================================================+
|  DMEM Section (Data)                               |
|    starts at: IMEM_end                             |
|    size: desc.dmem_load_size                       |
|    Contains Application Interface Table            |
+===================================================+
```

### FWSEC Ucode Data Extraction

From `FwSecBiosImage`:

```
ucode_data_offset = falcon_ucode_offset + desc.size()
                   + (signature_count * SIG_SIZE)
ucode_size        = desc.imem_load_size + desc.dmem_load_size
```

Where `desc.size()` extracts the header size from `(hdr >> 16) & 0xFFFF`.

But actually the code uses:

```
ucode_data_offset = falcon_ucode_offset + desc.size()
ucode_data = fwsec_data[ucode_data_offset .. ucode_data_offset + ucode_size]
```

And signatures:
```
sigs_data_offset = falcon_ucode_offset + sizeof(FalconUCodeDescV3)
sigs = fwsec_data[sigs_data_offset .. sigs_data_offset + sig_count * 384]
```

The actual ucode data (IMEM+DMEM concatenated) starts after the signatures.

### Loading Into Falcon

- IMEM: `src_start=0`, `dst_start=desc.imem_phys_base`, `len=desc.imem_load_size`
- DMEM: `src_start=desc.imem_load_size`, `dst_start=desc.dmem_phys_base`,
  `len=align_up(desc.dmem_load_size, 256)`
- Boot address: 0
- PKC data offset: `desc.pkc_data_offset`
- Engine ID mask: `desc.engine_id_mask`
- Ucode ID: `desc.ucode_id`

### FWSEC Command Patching

Before execution, the FWSEC DMEM must be patched with the command to run:

1. Find `FalconAppifHdrV1` at offset `imem_load_size + interface_offset` in the ucode data.
2. Iterate its entries to find `id == 0x4` (DMEMMAPPER).
3. Read `FalconAppifDmemmapperV3` at `imem_load_size + entry.dmem_base`.
4. Patch `dmem_mapper.init_cmd` with the desired command:
   - `0x15` = FRTS (create WPR2 region)
   - `0x19` = SB (secure boot)
5. Patch the FRTS command input buffer at `imem_load_size + dmem_mapper.cmd_in_buffer_offset`.

### Signature Patching

1. Read `desc.signature_versions` — each bit corresponds to one included signature.
2. Read the hardware fuse register for the `engine_id_mask` and `ucode_id`.
3. Check `signature_versions & (1 << fuse_version) != 0`.
4. Count bits set below the fuse version bit to get the signature index.
5. Copy the selected 384-byte RSA-3K signature into the ucode at
   `imem_load_size + pkc_data_offset`.

---

## 11. Falcon Ucode Descriptors

### FalconUCodeDescV3

**Source:** `drivers/gpu/nova-core/firmware.rs`

Used by FWSEC (Ampere and later). The `hdr` field encodes both version and size.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `hdr` | Header: bits[7:0]=unknown, bits[15:8]=version (must be 3), bits[31:16]=header size |
| 0x04 | 4 | `stored_size` | Size of stored firmware |
| 0x08 | 4 | `pkc_data_offset` | Offset in DMEM where the signature should be patched |
| 0x0C | 4 | `interface_offset` | Offset to Application Interface Table in DMEM |
| 0x10 | 4 | `imem_phys_base` | IMEM physical base address |
| 0x14 | 4 | `imem_load_size` | Size of code to load into IMEM |
| 0x18 | 4 | `imem_virt_base` | IMEM virtual base address |
| 0x1C | 4 | `dmem_phys_base` | DMEM physical base address |
| 0x20 | 4 | `dmem_load_size` | Size of data to load into DMEM |
| 0x24 | 2 | `engine_id_mask` | Bitmask of falcon engines that can run this firmware |
| 0x26 | 1 | `ucode_id` | Microcode ID for fuse register lookup |
| 0x27 | 1 | `signature_count` | Number of RSA-3K signatures following this header |
| 0x28 | 2 | `signature_versions` | Bitmask: each bit represents an included signature version |
| 0x2A | 2 | `_reserved` | Reserved / padding |

**Total size:** 44 bytes (0x2C)

**Extracting header size:** `(hdr >> 16) & 0xFFFF`
**Extracting version:** `(hdr >> 8) & 0xFF` (must be 3)

### FalconUCodeDescV2

**Source:** `drivers/gpu/nova-core/firmware.rs` (patch series for V2 support)

Used by Turing and GA100 VBIOS. Older format without PKC/signature fields.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `hdr` | Header: bits[15:8]=version (must be 2), bits[31:16]=header size |
| 0x04 | 4 | `stored_size` | Size of stored firmware |
| 0x08 | 4 | `uncompressed_size` | Uncompressed size |
| 0x0C | 4 | `virtual_entry` | Virtual entry point |
| 0x10 | 4 | `interface_offset` | Offset to Application Interface Table in DMEM |
| 0x14 | 4 | `imem_phys_base` | IMEM physical base address |
| 0x18 | 4 | `imem_load_size` | Size of code to load into IMEM |
| 0x1C | 4 | `imem_virt_base` | IMEM virtual base address |
| 0x20 | 4 | `imem_sec_base` | IMEM secure base address |
| 0x24 | 4 | `imem_sec_size` | IMEM secure size |
| 0x28 | 4 | `dmem_offset` | DMEM offset |
| 0x2C | 4 | `dmem_phys_base` | DMEM physical base address |
| 0x30 | 4 | `dmem_load_size` | Size of data to load into DMEM |
| 0x34 | 4 | `alt_imem_load_size` | Alternate IMEM load size |
| 0x38 | 4 | `alt_dmem_load_size` | Alternate DMEM load size |

**Total size:** 60 bytes (0x3C)

### Version Detection

Read the first 4 bytes (`hdr`). Extract version from bits [15:8]:
```
version = (hdr >> 8) & 0xFF
```
- Version 2 → `FalconUCodeDescV2`
- Version 3 → `FalconUCodeDescV3`

---

## 12. VBIOS ROM Layout and Parsing

**Source:** `drivers/gpu/nova-core/vbios.rs`, `docs.kernel.org/gpu/nova/core/vbios.html`

### ROM Location

VBIOS is mirrored onto BAR0 space starting at offset `0x300000`. Maximum scan length
is `0x100000` (1MB).

### Image Chain

The VBIOS is a series of concatenated PCI ROM images:

```
BAR0 + 0x300000:
+-------------------------------------------+
| PciAt Image (Type 0x00) - standard BIOS   |
+-------------------------------------------+
| EFI Image (Type 0x03) - UEFI GOP driver   |
+-------------------------------------------+
| First FwSec Image (Type 0xE0)             |
+-------------------------------------------+
| Second FwSec Image (Type 0xE0) - contains |
|   FWSEC-FRTS ucode                        |
+-------------------------------------------+
```

Images are contiguous (no gaps between images; aligned to 512 bytes).

### PCI ROM Expansion Header

Each image starts with:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 2 | `signature` | Must be `0xAA55` (also `0xBB77` or `0x4E56` accepted) |
| 0x02 | 20 | `reserved` | Architecture-specific reserved bytes |
| 0x16 | 2 | `nbsi_data_offset` | NBSI-specific offset (optional) |
| 0x18 | 2 | `pci_data_struct_offset` | Offset from image start to PCIR structure |
| 0x1A | 4 | `size_of_block` | NBSI-specific block size (optional) |

### PCIR Structure

At `pci_data_struct_offset`:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `signature` | Must be `"PCIR"` (0x52494350) or `"NPDS"` |
| 0x04 | 2 | `vendor_id` | PCI Vendor ID (0x10DE for NVIDIA) |
| 0x06 | 2 | `device_id` | PCI Device ID |
| 0x08 | 2 | `device_list_ptr` | Device List Pointer |
| 0x0A | 2 | `pci_data_struct_len` | Length of this PCIR structure |
| 0x0C | 1 | `pci_data_struct_rev` | Revision |
| 0x0D | 3 | `class_code` | PCI class code |
| 0x10 | 2 | `image_len` | Image size in **512-byte units** |
| 0x12 | 2 | `vendor_rom_rev` | Vendor ROM revision |
| 0x14 | 1 | `code_type` | Image type (see below) |
| 0x15 | 1 | `last_image` | Bit 7 set = last image in chain |
| 0x16 | 2 | `max_runtime_image_len` | Max runtime image length (512-byte units) |

**Total size:** 24 bytes (0x18)

**Code types:**
| Value | Type |
|-------|------|
| 0x00 | PciAt (PC-AT compatible BIOS) |
| 0x03 | EFI (UEFI GOP driver) |
| 0x70 | NBSI (Nvidia BIOS System Interface) |
| 0xE0 | FwSec (Firmware Security) |

### NPDE Structure

Immediately after PCIR (aligned to 16 bytes):

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `signature` | Must be `"NPDE"` (0x4544504E) |
| 0x04 | 2 | `npci_data_ext_rev` | Extension revision |
| 0x06 | 2 | `npci_data_ext_len` | Extension length |
| 0x08 | 2 | `subimage_len` | Sub-image length in **512-byte units** |
| 0x0A | 1 | `last_image` | Bit 7 set = last image in chain |

### BIT Header (BIOS Information Table)

Found by scanning the PciAt image for the byte pattern `[0xFF, 0xB8, 'B', 'I', 'T', 0x00]`:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 2 | `id` | Header identifier: `0xB8FF` |
| 0x02 | 4 | `signature` | `"BIT\0"` |
| 0x06 | 2 | `bcd_version` | BCD version (e.g., 0x0100 = 1.00) |
| 0x08 | 1 | `header_size` | Size of this header in bytes |
| 0x09 | 1 | `token_size` | Size of each token entry |
| 0x0A | 1 | `token_entries` | Number of token entries |
| 0x0B | 1 | `checksum` | Header checksum |

**Total size:** 12 bytes (0x0C)

### BIT Token Entry

Follows the BIT header, `token_entries` times:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 1 | `id` | Token identifier |
| 0x01 | 1 | `data_version` | Version of the token data |
| 0x02 | 2 | `data_size` | Size of token data in bytes |
| 0x04 | 2 | `data_offset` | Offset to the token data from start of PciAt image |

**Token ID `0x70`** = Falcon Data. The data at `data_offset` is a 4-byte pointer
to the PMU Lookup Table (in the FwSec image).

### PMU Lookup Table Header

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 1 | `version` | Table version (expected: 1) |
| 0x01 | 1 | `header_len` | Header length in bytes (typically 4) |
| 0x02 | 1 | `entry_len` | Size of each entry in bytes (typically 6) |
| 0x03 | 1 | `entry_count` | Number of entries |

### PMU Lookup Table Entry

Repeated `entry_count` times starting at `header_len` bytes after the table start:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 1 | `application_id` | Application ID (0x85 = FWSEC_PROD, 0x45 = FWSEC_DBG, 0x05 = FW_SEC_LIC) |
| 0x01 | 1 | `target_id` | Target ID (0x01 = PMU) |
| 0x02 | 4 | `data` | Absolute offset to the FalconUCodeDescV3 (needs adjustment, see below) |

### Falcon Data Pointer Adjustment

The `data` field in the PMU Lookup Table entry and the Falcon Data pointer from the BIT
table both assume the PciAt and FwSec images are contiguous (no EFI image in between).
The actual offset calculation requires compensation:

```
actual_offset_in_second_fwsec =
    falcon_data_ptr
    - pci_at_image.size
    - first_fwsec_image.size
```

---

## 13. Application Interface Structures

**Source:** `drivers/gpu/nova-core/firmware/fwsec.rs`

### FalconAppifHdrV1

Located at `imem_load_size + interface_offset` within the FWSEC ucode data.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 1 | `version` | Version (must be 1) |
| 0x01 | 1 | `header_size` | Size of this header in bytes |
| 0x02 | 1 | `entry_size` | Size of each entry |
| 0x03 | 1 | `entry_count` | Number of entries |

**Total size:** 4 bytes

### FalconAppifV1 (Entry)

Immediately follows the header, `entry_count` times:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `id` | Application interface ID |
| 0x04 | 4 | `dmem_base` | DMEM base offset for this app interface |

**Total size:** 8 bytes (packed)

**Known IDs:**
| Value | Name | Description |
|-------|------|-------------|
| 0x01 | DEVINIT | Device initialization |
| 0x04 | DMEMMAPPER | DMEM mapper (FWSEC commands) |

### FalconAppifDmemmapperV3

Located at `imem_load_size + entry.dmem_base` for the DMEMMAPPER entry.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `signature` | Must be `"DMAP"` (0x50414D44) |
| 0x04 | 2 | `version` | Version (expected: 3 = 0x0003) |
| 0x06 | 2 | `size` | Size of this structure (64 bytes) |
| 0x08 | 4 | `cmd_in_buffer_offset` | Offset in DMEM to command input buffer |
| 0x0C | 4 | `cmd_in_buffer_size` | Size of command input buffer |
| 0x10 | 4 | `cmd_out_buffer_offset` | Offset in DMEM to command output buffer |
| 0x14 | 4 | `cmd_out_buffer_size` | Size of command output buffer |
| 0x18 | 4 | `nvf_img_data_buffer_offset` | NVF image data buffer offset |
| 0x1C | 4 | `nvf_img_data_buffer_size` | NVF image data buffer size |
| 0x20 | 4 | `printf_buffer_hdr` | Printf buffer header |
| 0x24 | 4 | `ucode_build_time_stamp` | Build timestamp |
| 0x28 | 4 | `ucode_signature` | Ucode signature |
| 0x2C | 4 | `init_cmd` | Initial command to execute **(patched by driver)** |
| 0x30 | 4 | `ucode_feature` | Feature flags |
| 0x34 | 4 | `ucode_cmd_mask0` | Command mask 0 |
| 0x38 | 4 | `ucode_cmd_mask1` | Command mask 1 |
| 0x3C | 4 | `multi_tgt_tbl` | Multi-target table |

**Total size:** 64 bytes (0x40), packed

**FWSEC Commands (patched into `init_cmd`):**
| Value | Name | Description |
|-------|------|-------------|
| 0x15 | FRTS | Create WPR2 region, copy FRTS data |
| 0x19 | SB | Secure boot (load pre-OS apps on PMU) |

### FRTS Command Input Buffer

Located at `imem_load_size + cmd_in_buffer_offset`:

```c
struct FrtsCmd {  // packed
    struct ReadVbios {   // 24 bytes
        u32 ver;         // Version (set to 1)
        u32 hdr;         // Header size = sizeof(ReadVbios) = 24
        u64 addr;        // VBIOS DMA address (set to 0)
        u32 size;        // VBIOS size (set to 0)
        u32 flags;       // Flags (set to 2)
    } read_vbios;
    struct FrtsRegion {  // 20 bytes
        u32 ver;         // Version (set to 1)
        u32 hdr;         // Header size = sizeof(FrtsRegion) = 20
        u32 addr;        // FRTS address >> 12 (page-aligned)
        u32 size;        // FRTS size >> 12
        u32 ftype;       // Region type: 2 = NVFW_FRTS_CMD_REGION_TYPE_FB
    } frts_region;
};
```

---

## 14. ACR / WPR Structures

**Source:** `drivers/gpu/drm/nouveau/include/nvfw/acr.h`

These structures are used for the Write Protected Region and firmware signatures.

### struct wpr_header

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `falcon_id` | Falcon ID (0xFFFFFFFF = invalid) |
| 0x04 | 4 | `lsb_offset` | LSB header offset |
| 0x08 | 4 | `bootstrap_owner` | Bootstrap owner |
| 0x0C | 4 | `lazy_bootstrap` | Lazy bootstrap flag |
| 0x10 | 4 | `status` | Status code |

### struct lsb_header_tail

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `ucode_off` | Microcode offset |
| 0x04 | 4 | `ucode_size` | Microcode size |
| 0x08 | 4 | `data_size` | Data section size |
| 0x0C | 4 | `bl_code_size` | Bootloader code size |
| 0x10 | 4 | `bl_imem_off` | Bootloader IMEM offset |
| 0x14 | 4 | `bl_data_off` | Bootloader data offset |
| 0x18 | 4 | `bl_data_size` | Bootloader data size |
| 0x1C | 4 | `app_code_off` | Application code offset |
| 0x20 | 4 | `app_code_size` | Application code size |
| 0x24 | 4 | `app_data_off` | Application data offset |
| 0x28 | 4 | `app_data_size` | Application data size |
| 0x2C | 4 | `flags` | Flags |

---

## 15. Falcon Bootloader DMEM Descriptors

**Source:** `drivers/gpu/drm/nouveau/include/nvfw/flcn.h`

### struct loader_config (for DMA setup)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | `dma_idx` | DMA channel index |
| 0x04 | 4 | `code_dma_base` | Code DMA base address |
| 0x08 | 4 | `code_size_total` | Total code size |
| 0x0C | 4 | `code_size_to_load` | Code size to load |
| 0x10 | 4 | `code_entry_point` | Code entry point |
| 0x14 | 4 | `data_dma_base` | Data DMA base address |
| 0x18 | 4 | `data_size` | Data size |
| 0x1C | 4 | `overlay_dma_base` | Overlay DMA base |
| 0x20 | 4 | `argc` | Argument count |
| 0x24 | 4 | `argv` | Arguments pointer |
| 0x28 | 4 | `code_dma_base1` | Code DMA base (high bits) |
| 0x2C | 4 | `data_dma_base1` | Data DMA base (high bits) |
| 0x30 | 4 | `overlay_dma_base1` | Overlay DMA base (high bits) |

### struct flcn_bl_dmem_desc_v2 (Bootloader DMEM descriptor, packed)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 16 | `reserved[4]` | Reserved u32s |
| 0x10 | 16 | `signature[4]` | Signature u32s |
| 0x20 | 4 | `ctx_dma` | Context DMA channel |
| 0x24 | 8 | `code_dma_base` | Code DMA base (u64) |
| 0x2C | 4 | `non_sec_code_off` | Non-secure code offset |
| 0x30 | 4 | `non_sec_code_size` | Non-secure code size |
| 0x34 | 4 | `sec_code_off` | Secure code offset |
| 0x38 | 4 | `sec_code_size` | Secure code size |
| 0x3C | 4 | `code_entry_point` | Entry point |
| 0x40 | 8 | `data_dma_base` | Data DMA base (u64) |
| 0x48 | 4 | `data_size` | Data size |
| 0x4C | 4 | `argc` | Argument count |
| 0x50 | 4 | `argv` | Arguments pointer |

---

## 16. Boot Flow Summary

### Ampere (GA102) GSP Boot Sequence

```
1. Driver reads VBIOS from BAR0+0x300000
   └─> Extracts FWSEC-FRTS firmware

2. Driver loads firmware files from /lib/firmware/nvidia/VERSION/:
   ├── gsp_ga10x.bin       (ELF: GSP firmware)
   ├── bootloader-VERSION.bin  (BinHdr+RISC-V: GSP bootloader)
   ├── booter_load-VERSION.bin (BinHdr+HS: SEC2 booter)
   └── booter_unload-VERSION.bin (BinHdr+HS: SEC2 booter)

3. FWSEC-FRTS runs on GSP (Heavy-Secure mode)
   ├── Patches FWSEC with FRTS command (0x15)
   ├── Patches correct RSA-3K signature
   ├── Loads IMEM+DMEM into GSP falcon
   ├── Runs: carves WPR2 region, copies FRTS data
   └── Result: WPR2 region established in VRAM

4. Booter (booter_load) runs on SEC2
   ├── Patches correct signature from fuse register
   ├── Loaded into SEC2 falcon IMEM+DMEM
   ├── SEC2 loads GSP bootloader into GSP RISC-V core
   └── SEC2 loads GSP firmware page tables

5. GSP Bootloader runs on GSP (RISC-V)
   ├── Accesses GSP firmware via radix-3 page table
   ├── Verifies GSP firmware signatures
   └── Loads and starts GSP-RM firmware

6. GSP-RM firmware running
   └── Full GPU management from GSP processor
```

### Firmware Version String

The firmware version (e.g., `"535.113.01"`) is embedded in the file path:
```
/lib/firmware/nvidia/535.113.01/gsp_ga10x.bin
```

There is no separate version header within the binary files themselves, but the
`RmRiscvUCodeDesc.app_version` field in the bootloader contains a numeric version.

### Signature Validation

- **FWSEC**: RSA-3K signatures (384 bytes each). Multiple versions embedded.
  Selected based on `signature_versions` bitmask and hardware fuse register.
- **Booter (HS)**: Variable-size signatures. Count and size determined from
  `nvfw_hs_header_v2.sig_prod_size` and `num_sig`. Selected based on
  `HsSignatureParams.fuse_ver` vs. hardware fuse register.
- **GSP firmware**: Verified by the GSP bootloader, not by the kernel driver.
  Signatures stored in ELF section `.fwsignature_ga10x`.
- **GSP bootloader**: Contains a manifest (`manifest_offset`) for verification.

### Key Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `BIN_MAGIC` | `0x10DE` | BinHdr magic (NVIDIA vendor ID) |
| `ROM_OFFSET` | `0x300000` | VBIOS ROM offset in BAR0 |
| `BIOS_MAX_SCAN_LEN` | `0x100000` | Max VBIOS scan length (1MB) |
| `ROM_SIGNATURE` | `0xAA55` | PCI ROM expansion header signature |
| `LAST_IMAGE_BIT` | `0x80` | Bit 7 of `last_image` field |
| `BIT_TOKEN_FALCON` | `0x70` | BIT token ID for Falcon data |
| `FWSEC_PROD_APP_ID` | `0x85` | PMU lookup table app ID for FWSEC production |
| `FWSEC_DBG_APP_ID` | `0x45` | PMU lookup table app ID for FWSEC debug |
| `APPIF_DMEMMAPPER` | `0x04` | Application interface ID for DMEM mapper |
| `CMD_FRTS` | `0x15` | FWSEC command: create FRTS/WPR2 |
| `CMD_SB` | `0x19` | FWSEC command: secure boot |
| `FRTS_REGION_FB` | `0x02` | FRTS region type: framebuffer |
| `RSA3K_SIG_SIZE` | `384` | Size of each RSA-3K signature (96 * 4 bytes) |
| `GSP_PAGE_SIZE` | `4096` | GSP page size |
| `DMEM_ALIGN` | `256` | DMEM load size alignment |
