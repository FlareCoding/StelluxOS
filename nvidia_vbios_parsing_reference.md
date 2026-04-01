# NVIDIA VBIOS / BIT / DCB Parsing Reference

Extracted from Linux kernel nouveau driver source code (`drivers/gpu/drm/nouveau/nvkm/subdev/bios/`)
and the NVIDIA open-gpu-doc DCB 4.x Specification.

---

## 1. VBIOS ROM Shadow Methods

Source: `shadow.c`, `shadowramin.c`, `shadowrom.c`, `shadowpci.c`, `shadowacpi.c`, `shadowof.c`, `priv.h`

### 1.1 Shadow Source Interface

Every shadow method implements `struct nvbios_source` (from `priv.h`):

```c
struct nvbios_source {
    const char *name;
    void *(*init)(struct nvkm_bios *, const char *);
    void  (*fini)(void *);
    u32   (*read)(void *, u32 offset, u32 length, struct nvkm_bios *);
    u32   (*size)(void *);
    bool rw;
    bool ignore_checksum;
    bool no_pcir;
    bool require_checksum;
};
```

### 1.2 Method Priority Order

From `nvbios_shadow()` in `shadow.c`, methods are tried in this order:

| Priority | Method | Name | Skip Threshold | Notes |
|----------|--------|------|---------------|-------|
| 0 (first) | `nvbios_of` | OpenFirmware | 0 | PowerPC only, reads `NVDA,BMP` device-tree property |
| 1 | `nvbios_ramin` | PRAMIN | 0 | BAR0 PRAMIN window, preferred on NV50+ |
| 2 | `nvbios_prom` | PROM | 0 | Direct PROM read at BAR0+0x300000 |
| 3 | `nvbios_acpi_fast` | ACPI (fast) | 0 | ACPI `_ROM` method, large reads |
| 4 | `nvbios_acpi_slow` | ACPI (slow) | 4 | ACPI `_ROM` method, 4KB chunks (fallback) |
| 5 | `nvbios_pcirom` | PCIROM | 1 | PCI expansion ROM mapping |
| 6 | `nvbios_platform` | PLATFORM | 1 | Platform ROM (ioremap of pdev->rom) |

The `skip` field means: "don't try this method unless the current best score is less than `skip`."
A `skip=0` means always try. Higher-scored images win. Score is based on checksum validity and image chain completeness.

### 1.3 PRAMIN Shadow Method (BAR0 window)

Source: `shadowramin.c`

**Read function:** Reads from BAR0 offset `0x700000 + offset`, up to 1MB.

```
for (i = offset; i < offset + length; i += 4)
    *(u32 *)&bios->data[i] = nvkm_rd32(device, 0x700000 + i);
```

**Init (NV50+):**
1. Check if display is disabled:
   - GA100+: check chipset
   - GM100+: read `0x021c04`, if bit 0 set → display disabled
   - NV_C0+: read `0x022500`, if bit 0 set → display disabled
2. Check PRAMIN window is enabled and points to VRAM:
   - GV100+: read `0x625f04`
   - Others: read `0x619f04`
   - Bit 3 must be set (enabled), bits [1:0] must be 1 (VRAM)
3. Compute VBIOS address: `(reg_value & 0xFFFFFF00) << 8`
4. Fallback: `(reg_001700 << 16) + 0xF0000`
5. Save old BAR0 PRAMIN window (`reg 0x001700`), set new window: `addr >> 16`
6. On fini: restore old `0x001700` value

### 1.4 PROM Shadow Method (direct ROM read)

Source: `shadowrom.c`

**Read function:** Reads from BAR0 offset `0x300000 + offset`, up to 1MB.

```
for (i = offset; i < offset + length; i += 4)
    *(u32 *)&bios->data[i] = nvkm_rd32(device, 0x300000 + i);
```

**Init:** Disables ROM shadow via `nvkm_pci_rom_shadow(device->pci, false)` so hardware exposes the real ROM.
Excluded for NV40 chipset >= 0x4c.

### 1.5 PCI ROM Shadow Method

Source: `shadowpci.c`

Uses kernel PCI ROM APIs:
1. `pci_enable_rom(pdev)` — enables the expansion ROM (sets bit 0 of PCI config register 0x30)
2. `pci_map_rom(pdev, &size)` — maps the ROM into CPU address space
3. `memcpy_fromio()` — copies data

Also provides `nvbios_platform` which uses `ioremap(pdev->rom, pdev->romlen)` for platform devices.

### 1.6 ACPI Shadow Method

Source: `shadowacpi.c`

Calls ACPI `_ROM` method on the GPU's ACPI device handle.
- **Fast mode:** Requests entire region at once (violates spec but much faster on some systems like Lenovo W530)
- **Slow mode:** Reads in strict 4KB (0x1000) chunks per ACPI spec
- Fast mode has `require_checksum = true`, so if checksum fails, it falls back to slow mode

### 1.7 Scoring System

Each image gets a score. Higher is better:
- Base score: 1 (image found)
- +3 if checksum passes (or non-type-0 image)
- If checksum fails and `require_checksum` is false: +1 (or +2 if source is rw)
- Scores accumulate across chained images (multi-image ROMs)

### 1.8 ROM Image Validation

Source: `image.c`

Valid ROM signatures at offset 0x00:
- `0xAA55` — standard PCI expansion ROM
- `0xBB77` — alternate
- `0x4E56` — "NV" signature

The PCIR (PCI Data Structure) is then parsed to get image size, type, and whether it's the last image.

---

## 2. BIT Table (BIOS Information Table)

Source: `bit.c`, `base.c`, `include/nvkm/subdev/bios/bit.h`

### 2.1 BIT Signature

The BIT table is located by searching the entire VBIOS image for the 5-byte signature:

```
0xFF 0xB8 'B' 'I' 'T'    (bytes: FF B8 42 49 54)
```

Code from `base.c`:
```c
bios->bit_offset = nvbios_findstr(bios->data, bios->size, "\xff\xb8""BIT", 5);
```

The offset stored in `bios->bit_offset` points to the **first byte of the signature** (the `0xFF`).

### 2.2 BIT Header Layout

Starting at `bios->bit_offset`:

| Offset | Size | Field |
|--------|------|-------|
| +0 | 1 | `0xFF` (part of signature) |
| +1 | 1 | `0xB8` (part of signature) |
| +2 | 1 | `'B'` (0x42) |
| +3 | 1 | `'I'` (0x49) |
| +4 | 1 | `'T'` (0x54) |
| +5..+8 | ? | (header fields, implementation-specific) |
| +9 | 1 | **Entry size** (bytes per token entry) |
| +10 | 1 | **Entry count** (number of token entries) |
| +11 | 1 | (padding/reserved) |
| +12 | ... | **First token entry** (entries start here) |

### 2.3 BIT Token Entry Format

Each token entry is `entry_size` bytes long (read from offset +9).
Entries start at `bios->bit_offset + 12`.

| Entry Offset | Size | Field |
|-------------|------|-------|
| +0 | 1 (u8) | **Token ID** (ASCII character, e.g., `'i'`, `'d'`, `'D'`, `'I'`, `'C'`, etc.) |
| +1 | 1 (u8) | **Version** |
| +2 | 2 (u16 LE) | **Data length** (size of the data the pointer references) |
| +4 | 2 (u16 LE) | **Data offset/pointer** (absolute offset within the VBIOS image) |

Code from `bit.c`:
```c
bit->id      = nvbios_rd08(bios, entry + 0);
bit->version = nvbios_rd08(bios, entry + 1);
bit->length  = nvbios_rd16(bios, entry + 2);
bit->offset  = nvbios_rd16(bios, entry + 4);
```

**Important:** The `offset` is an **absolute byte offset from the start of the VBIOS image** (not relative to the BIT table). When you read the 16-bit value, you can directly index into the VBIOS data at that position.

### 2.4 Iterating BIT Entries

```
entries = read_u8(bit_offset + 10)     // number of entries
entry_size = read_u8(bit_offset + 9)   // bytes per entry
entry_ptr = bit_offset + 12            // first entry

for i in 0..entries:
    id = read_u8(entry_ptr + 0)
    version = read_u8(entry_ptr + 1)
    length = read_u16_le(entry_ptr + 2)
    offset = read_u16_le(entry_ptr + 4)
    entry_ptr += entry_size
```

### 2.5 Known BIT Token IDs

| Token ID | Char | Purpose |
|----------|------|---------|
| 0x69 | `'i'` | BIOS version info (version bytes at offset+0..+4) |
| 0x64 | `'d'` | Display-related data |
| 0x44 | `'D'` | Display path info |
| 0x49 | `'I'` | Init tables |
| 0x43 | `'C'` | Clock/PLL tables |
| 0x50 | `'P'` | Performance tables |
| 0x54 | `'T'` | Temperature/thermal tables |
| 0x55 | `'U'` | Voltage tables |
| 0x4D | `'M'` | Memory tables |
| 0x42 | `'B'` | BIOS data |

### 2.6 VBIOS Version from BIT 'i' Token

From `base.c`:
```c
if (!bit_entry(bios, 'i', &bit_i) && bit_i.length >= 4) {
    bios->version.major = nvbios_rd08(bios, bit_i.offset + 3);
    bios->version.chip  = nvbios_rd08(bios, bit_i.offset + 2);
    bios->version.minor = nvbios_rd08(bios, bit_i.offset + 1);
    bios->version.micro = nvbios_rd08(bios, bit_i.offset + 0);
    bios->version.patch = nvbios_rd08(bios, bit_i.offset + 4);
}
```

---

## 3. DCB (Device Control Block) Parsing

Source: `dcb.c`, `include/nvkm/subdev/bios/dcb.h`

### 3.1 Finding the DCB Table

The DCB table pointer is stored at a **fixed VBIOS offset 0x36** (for NV_04+ cards):

```c
if (device->card_type > NV_04)
    dcb = nvbios_rd16(bios, 0x36);
```

This is a 16-bit little-endian absolute offset within the VBIOS image.

**Note:** This is NOT obtained through a BIT token. The DCB pointer at offset 0x36 is a legacy fixed-location pointer that predates the BIT table system.

### 3.2 DCB Header Format (Version >= 0x30, including 4.x)

| Offset | Size | Field |
|--------|------|-------|
| +0 | 1 (u8) | **Version** (0x40 for DCB 4.0, 0x41 for 4.1) |
| +1 | 1 (u8) | **Header size** (bytes, 27 for v4.0) |
| +2 | 1 (u8) | **Entry count** |
| +3 | 1 (u8) | **Entry size** (8 bytes for v4.0+) |
| +4 | 2 (u16 LE) | **CCB (I2C) table pointer** |
| +6 | 4 (u32 LE) | **DCB Signature** = `0x4EDCBDCB` |
| +10 | 2 (u16 LE) | GPIO Assignment Table pointer |
| +12 | 2 (u16 LE) | Input Devices Table pointer (optional) |
| +14 | 2 (u16 LE) | Personal Cinema Table pointer (optional) |
| +16 | 2 (u16 LE) | Spread Spectrum Table pointer (optional) |
| +18 | 2 (u16 LE) | I2C Devices Table pointer (optional) |
| +20 | 2 (u16 LE) | Connector Table pointer |
| +22 | 1 (u8) | Flags |
| +23 | 2 (u16 LE) | HDTV Translation Table pointer (optional) |
| +25 | 2 (u16 LE) | Switched Outputs Table pointer (optional) |

All pointers are absolute byte offsets from the start of the VBIOS image.

**Validation:** For v3.0+, check that `read_u32(dcb + 6) == 0x4EDCBDCB`.

### 3.3 DCB Version Detection and Header Sizes

| Version | Signature Location | Header Size | Entry Size |
|---------|-------------------|-------------|------------|
| >= 0x30, < 0x42 | dcb+6, 0x4EDCBDCB | from dcb+1 | from dcb+3 |
| >= 0x20, < 0x30 | dcb+4, 0x4EDCBDCB | fixed 8 | fixed 8 |
| >= 0x15, < 0x20 | dcb-7, "DEV_REC" | fixed 4 | fixed 10 |
| < 0x15 | — | — | unusable |

### 3.4 DCB Device Entry — Display Path Information (First DWORD)

Each entry is 8 bytes (two 32-bit words). The first DWORD is common to all device types:

```
Bits [31:0] of DWORD 0 (Display Path Information):

  31  30  29  28  27  26  25  24  23  22  21  20  19  18  17  16  15  14  13  12  11  10   9   8   7   6   5   4   3   2   1   0
 [Rsvd ] [VD][ BBDR][ BDR][   Output Devices   ][  Loc  ][       Bus       ][   Connector   ][     Head      ][  EDID Port  ][ Type  ]
```

| Bits | Width | Field | Extraction |
|------|-------|-------|------------|
| 3:0 | 4 | **Type** (display type) | `conn & 0x0000000F` |
| 7:4 | 4 | **EDID Port** (I2C/CCB index) | `(conn >> 4) & 0x0F` |
| 11:8 | 4 | **Head bitmask** | `(conn >> 8) & 0x0F` |
| 15:12 | 4 | **Connector** (table index) | `(conn >> 12) & 0x0F` |
| 19:16 | 4 | **Bus** (logical exclusion) | `(conn >> 16) & 0x0F` |
| 21:20 | 2 | **Location** (on-chip/off-chip) | `(conn >> 20) & 0x03` |
| 22 | 1 | **BDR** (boot display removed) | `(conn >> 22) & 0x01` |
| 23 | 1 | **BBDR** (blind boot display removed) | `(conn >> 23) & 0x01` |
| 27:24 | 4 | **Output Devices** (DAC/SOR/PIOR mask) | `(conn >> 24) & 0x0F` |
| 28 | 1 | Reserved | |
| 29 | 1 | Reserved | |
| 30 | 1 | **Virtual Device** | `(conn >> 30) & 0x01` |
| 31 | 1 | Reserved | |

**Nouveau's extraction (from `dcb.c`):**
```c
u32 conn = nvbios_rd32(bios, dcb + 0x00);
outp->or         = (conn & 0x0F000000) >> 24;  // Output Resource (DAC/SOR/PIOR)
outp->location   = (conn & 0x00300000) >> 20;  // Location
outp->bus        = (conn & 0x000F0000) >> 16;  // Bus
outp->connector  = (conn & 0x0000F000) >> 12;  // Connector index
outp->heads      = (conn & 0x00000F00) >> 8;   // Head bitmask
outp->i2c_index  = (conn & 0x000000F0) >> 4;   // EDID port / I2C index
outp->type       = (conn & 0x0000000F);         // Display type
```

### 3.5 Display Type Values

| Value | Enum | Display Type |
|-------|------|-------------|
| 0x0 | `DCB_OUTPUT_ANALOG` | CRT (VGA) |
| 0x1 | `DCB_OUTPUT_TV` | TV encoder |
| 0x2 | `DCB_OUTPUT_TMDS` | TMDS (DVI/HDMI) |
| 0x3 | `DCB_OUTPUT_LVDS` | LVDS (laptop panel) |
| 0x4 | — | Reserved |
| 0x5 | — | SDI |
| 0x6 | `DCB_OUTPUT_DP` | DisplayPort |
| 0x8 | `DCB_OUTPUT_WFD` | WiFi Display |
| 0xE | `DCB_OUTPUT_EOL` | End of List (stop parsing) |
| 0xF | `DCB_OUTPUT_UNUSED` | Skip Entry |

### 3.6 Location Values

| Value | Meaning |
|-------|---------|
| 0 | On Chip (internal encoder: internal TV, internal TMDS, SOR) |
| 1 | On Board (external encoder: external DAC, external TMDS) |
| 2 | Reserved |

### 3.7 Output Resource Assignment (bits 27:24)

For DCB 4.0: Each bit maps to a specific DAC/SOR/PIOR:
- Bit 0 = DAC 0, SOR 0, or PIOR 0
- Bit 1 = DAC 1, SOR 1, or PIOR 1
- Bit 2 = DAC 2, SOR 2, or PIOR 2
- Bit 3 = DAC 3, SOR 3, or PIOR 3

Whether it refers to DAC, SOR, or PIOR depends on the device type and location:
- CRT/TV + Location=0 → DAC
- DFP (TMDS/LVDS/DP) + Location=0 → SOR
- Any type + Location=1 → PIOR

For DCB 4.1 (GM20x+) DFP entries: repurposed as Pad Macro Assignment.

### 3.8 DCB Device Entry — Device Specific Info (Second DWORD)

#### CRT Specific (Type=0): DWORD 1 is all reserved (set to 0).

#### DFP Specific (Type=2 TMDS, 3 LVDS, 5 SDI, 6 DP): DWORD 1 layout:

```
  31  30  29  28  27  26  25  24  23  22  21  20  19  18  17  16  15  14  13  12  11  10   9   8   7   6   5   4   3   2   1   0
 [  Rsvd  ][   MxLM   ][  MxLR ][  E  ][ Rsvd ][HDMI][ Rsv][        Ext Enc         ][ Rsvd ][  SL/DPL ][  Ctrl  ][  EDID  ]
```

| Bits | Width | Field | Description |
|------|-------|-------|-------------|
| 1:0 | 2 | **EDID source** | 0=DDC, 1=straps, 2=ACPI/SBIOS, 3=reserved |
| 3:2 | 2 | **Ctrl** (Power/Backlight) | 0=External, 1=Scripts, 2=VBIOS SBIOS callbacks |
| 5:4 | 2 | **Sub-link / DP Link** | Bit 0=Sub-link A / DP-A, Bit 1=Sub-link B / DP-B |
| 7:6 | 2 | Reserved | |
| 15:8 | 8 | **External Encoder** | External link type (0=none, see table) |
| 16 | 1 | Reserved | |
| 17 | 1 | **HDMI Enable** | 0=disable, 1=enable HDMI on this DFP |
| 19:18 | 2 | Reserved | |
| 20 | 1 | **E** (External Comms Port) | 0=primary, 1=secondary |
| 23:21 | 3 | **Max Link Rate** (DP only) | 0=1.62G, 1=2.7G, 2=5.4G, 3=8.1G |
| 27:24 | 4 | **Max Lane Mask** (DP only) | 0x1=1lane, 0x2/0x3=2lane, 0x4/0xF=4lane |
| 31:28 | 4 | Reserved | |

**Nouveau's extraction for DFP (v4.0+):**
```c
u32 conf = nvbios_rd32(bios, dcb + 0x04);
outp->link = (conf & 0x00000030) >> 4;          // Sub-link assignment
outp->sorconf.link = outp->link;
outp->extdev = (conf & 0x0000FF00) >> 8;        // External encoder (if location != 0)

// For DP:
// Link bandwidth: (conf & 0x00E00000)
//   0x00000000 → 1.62 Gbps (link_bw = 0x06)
//   0x00200000 → 2.7 Gbps  (link_bw = 0x0a)
//   0x00400000 → 5.4 Gbps  (link_bw = 0x14)
//   0x00600000 → 8.1 Gbps  (link_bw = 0x1e)
// Lane count: (conf & 0x0F000000) >> 24
//   0xF or 0x4 → 4 lanes
//   0x3 or 0x2 → 2 lanes
//   0x1        → 1 lane
```

### 3.9 Sentinel / End-of-Table Detection

```c
if (nvbios_rd32(bios, outp) == 0x00000000) break;  // all zeros = end
if (nvbios_rd32(bios, outp) == 0xFFFFFFFF) break;  // all ones = end
if (nvbios_rd08(bios, outp) == 0x0F) continue;      // DCB_OUTPUT_UNUSED = skip
if (nvbios_rd08(bios, outp) == 0x0E) break;         // DCB_OUTPUT_EOL = stop
```

### 3.10 DCB Output Hashing (for matching)

```c
hasht = (extdev << 8) | (location << 4) | type;
hashm = (heads << 8) | (link << 6) | or;
```

---

## 4. I2C / Communications Control Block

Source: `i2c.c`, `include/nvkm/subdev/bios/i2c.h`

### 4.1 Finding the I2C Table

The I2C (CCB) table pointer is stored in the DCB header:
- DCB v1.5+: `dcb + 2` (16-bit pointer)
- DCB v3.0+: `dcb + 4` (16-bit pointer) ← this is the CCB pointer field in the DCB 4.x header

```c
if (*ver >= 0x15) i2c = nvbios_rd16(bios, dcb + 2);
if (*ver >= 0x30) i2c = nvbios_rd16(bios, dcb + 4);  // overrides the above
```

### 4.2 CCB 4.0 Header (version >= 0x30)

| Offset | Size | Field |
|--------|------|-------|
| +0 | 1 (u8) | **Version** (0x40 for CCB 4.0, 0x41 for 4.1) |
| +1 | 1 (u8) | **Header size** (typically 5 bytes) |
| +2 | 1 (u8) | **Entry count** |
| +3 | 1 (u8) | **Entry size** (4 bytes) |
| +4 | 4 bits | **Primary communication port** index |
| +4 | 4 bits | **Secondary communication port** index (upper nibble) |

For CCB 4.1 (GM20x+):
| +0 | 1 | Version (0x41) |
| +1 | 1 | Header size (6 bytes) |
| +2 | 1 | Entry count |
| +3 | 1 | Entry size (4 bytes) |
| +4 | 1 | Primary communication port index |
| +5 | 1 | Secondary communication port index |

For legacy (pre-3.0): No dedicated header. 16 entries of 4 bytes each, starting immediately at the pointer.

### 4.3 CCB 4.0 Entry Format (4 bytes / 32 bits)

The upper byte (bits 31:24) is the **Access Method** type:

#### I2C Access Method (type = 5)

```
  31:24   23:21  20     19:17  16    15:5    4:0
 [  =5  ][Rsvd][DP   ][Rsvd ][Hyb][Rsvd+Spd][PhysPort]
```

| Bits | Width | Field |
|------|-------|-------|
| 3:0 | 4 | Physical NV5x Port (0=DDC0, 1=DDC1, 2=DDC2, 3=I2C) |
| 7:4 | 4 | Port Speed (0=default, 1=100kHz, 3=400kHz, 6=3.4MHz, 7=60kHz) |
| 8 | 1 | Hybrid Pad (0=normal GPIO I2C, 1=hybrid DPAux/I2C pad) |
| 12:9 | 4 | Physical DP Aux Port (only when Hybrid=1) |
| 23:13 | 11 | Reserved |
| 31:24 | 8 | Access Method = 5 |

#### DisplayPort AUX Channel Access Method (type = 6)

```
  31:24   23:13  12:9   8      7:4     3:0
 [  =6  ][Rsvd][I2C ][Hyb][Rsvd  ][PhysPort]
```

| Bits | Width | Field |
|------|-------|-------|
| 3:0 | 4 | Physical DP Port (0=AUXCH0, 1=AUXCH1, 2=AUXCH2, 3=AUXCH3) |
| 7:4 | 4 | Reserved |
| 8 | 1 | Hybrid Pad |
| 12:9 | 4 | Physical I2C Port (only when Hybrid=1) |
| 23:13 | 11 | Reserved |
| 31:24 | 8 | Access Method = 6 |

### 4.4 CCB 4.1 Entry Format (GM20x+)

```
  31:10   9:5       4:0
 [Rsvd][DPAUX Port][I2C Port]
```

| Bits | Width | Field |
|------|-------|-------|
| 4:0 | 5 | I2C Port index in PMGR (0x1F = unused) |
| 9:5 | 5 | DPAUX Port index in PMGR (0x1F = unused) |
| 27:10 | 18 | Reserved |
| 31:28 | 4 | I2C Port Speed |

### 4.5 I2C Entry Type Detection (Nouveau)

From `dcb_i2c_parse()` in `i2c.c`:

| CCB Version | Type Detection |
|-------------|---------------|
| >= 0x41 | Read 32-bit entry; if both I2C port (bits 4:0) and DPAUX port (bits 9:5) are 0x1F → UNUSED, else → PMGR |
| >= 0x30 | `type = read_u8(entry + 3)` (the access method byte) |
| < 0x30 | `type = read_u8(entry + 3) & 0x07`; value 7 → UNUSED |

### 4.6 I2C Type Enum Values

| Value | Enum | Description | How GPIO pins are extracted |
|-------|------|-------------|---------------------------|
| 0x00 | `DCB_I2C_NV04_BIT` | Legacy NV04 bit-bang | `drive = entry[0]`, `sense = entry[1]` |
| 0x04 | `DCB_I2C_NV4E_BIT` | NV4E bit-bang | `drive = entry[1]` |
| 0x05 | `DCB_I2C_NVIO_BIT` | NVIO I2C bit-bang | `drive = entry[0] & 0x0F`; if `entry[1] & 0x01` then `share = entry[1] >> 1` |
| 0x06 | `DCB_I2C_NVIO_AUX` | NVIO AUX channel | `auxch = entry[0] & 0x0F`; if `entry[1] & 0x01` then `share = auxch` |
| 0x80 | `DCB_I2C_PMGR` | PMGR-controlled (v4.1+) | `drive = entry_u16[0] & 0x1F`; `auxch = (entry_u16[0] >> 5) & 0x1F`; 0x1F = unused |
| 0xFF | `DCB_I2C_UNUSED` | Unused entry | — |

### 4.7 GPIO Pin Extraction by Type

**NV04_BIT (type 0x00):**
```
SCL (drive) = byte at entry + 0    (GPIO pin number for clock)
SDA (sense) = byte at entry + 1    (GPIO pin number for data)
```

**NV4E_BIT (type 0x04):**
```
SCL (drive) = byte at entry + 1
```

**NVIO_BIT (type 0x05):**
```
drive = (byte at entry + 0) & 0x0F    (lower nibble = port number)
share = (byte at entry + 1) >> 1      (if bit 0 of entry[1] is set)
```

**NVIO_AUX (type 0x06):**
```
auxch = (byte at entry + 0) & 0x0F   (AUX channel number)
share = auxch                          (if bit 0 of entry[1] is set)
```

**PMGR (type 0x80, CCB v4.1+):**
```
i2c_port  = bits [4:0] of 16-bit value at entry  (0x1F = unused)
dpaux_port = bits [9:5] of 16-bit value at entry  (0x1F = unused)
share = dpaux_port
```

---

## 5. DCB 4.x Official Specification Details

Source: NVIDIA open-gpu-doc, DCB 4.x Specification

### 5.1 DCB Flags (offset +22 in header)

| Bit | Meaning |
|-----|---------|
| 0 | Boot Display Count (0=1 display, 1=2 displays) |
| 5:4 | VIP location (00=none, 01=Pin Set A, 10=Pin Set B) |
| 6 | DR Port Pin Set A (1=routed to SLI finger) |
| 7 | DR Port Pin Set B (1=routed to SLI finger) |

### 5.2 Connector Table

#### Header (5 bytes):

| Offset | Size | Field |
|--------|------|-------|
| +0 | 1 | Version (0x40) |
| +1 | 1 | Header size (5 bytes) |
| +2 | 1 | Entry count |
| +3 | 1 | Entry size (4 bytes) |
| +4 | 1 | Platform type |

Platform types: 0x00=Normal add-in, 0x07=Desktop integrated DP, 0x08=Mobile add-in,
0x09=MXM module, 0x10=Mobile back, 0x11=Mobile back+left, 0x18=Mobile dock,
0x20=Crush/nForce

#### Entry (4 bytes / 32 bits):

```
  31  30  29:28  27  26  25  24  23  22  21  20  19  18  17  16  15  14  13  12  11:8      7:0
 [Rsv][LCDID ][SRA][ G][ F][ E][DID][DIC][DIB][DIA][DPD][DPC][ D][ C][DPB][DPA][ B][ A][Location][Type  ]
```

| Bits | Width | Field |
|------|-------|-------|
| 7:0 | 8 | **Type** (connector type, see table below) |
| 11:8 | 4 | **Location** |
| 12 | 1 | Hotplug A |
| 13 | 1 | Hotplug B |
| 14 | 1 | DP2DVI A |
| 15 | 1 | DP2DVI B |
| 16 | 1 | Hotplug C |
| 17 | 1 | Hotplug D |
| 18 | 1 | DP2DVI C |
| 19 | 1 | DP2DVI D |
| 20 | 1 | DPAux/I2C-A |
| 21 | 1 | DPAux/I2C-B |
| 22 | 1 | DPAux/I2C-C |
| 23 | 1 | DPAux/I2C-D |
| 24 | 1 | Hotplug E |
| 25 | 1 | Hotplug F |
| 26 | 1 | Hotplug G |
| 27 | 1 | Panel Self Refresh Frame Lock A |
| 29:28 | 3 | LCD ID (0-7 maps to LCD0-LCD7 GPIOs) |
| 31 | 1 | Reserved |

#### Connector Type Values:

| Value | Type |
|-------|------|
| 0x00 | VGA 15-pin |
| 0x01 | DVI-A |
| 0x10 | TV Composite |
| 0x11 | TV S-Video |
| 0x13 | TV HDTV Component (YPrPb) |
| 0x30 | DVI-I |
| 0x31 | DVI-D |
| 0x40 | LVDS SPWG Attached |
| 0x41 | LVDS OEM Attached |
| 0x46 | DisplayPort External |
| 0x47 | DisplayPort Internal |
| 0x48 | Mini DisplayPort External |
| 0x61 | HDMI-A |
| 0x63 | HDMI-C (Mini) |
| 0xFF | Skip Entry |

### 5.3 GPIO Assignment Table

#### Header (v4.1, 6 bytes):

| Offset | Size | Field |
|--------|------|-------|
| +0 | 1 | Version (0x41) |
| +1 | 1 | Header size (6 bytes) |
| +2 | 1 | Entry count |
| +3 | 1 | Entry size (5 bytes for v4.1, 4 for v4.0) |
| +4 | 2 | External GPIO Assignment Table Master Header pointer |

#### Entry (v4.1, 5 bytes / 40 bits):

```
DWORD 0 (bits 31:0):
  31     30    29    28:24       23:16          15:8         7    6    5:0
 [PWM ][Rsv][GSYNC][InHWSel][OutHWSel   ][ Function   ][ Init][ IO][ PinNum]

Byte 4 (bits 39:32):
  39    38    37    36    35:32
 [ OE ][ OT ][ FE ][ FT ][ LockPin ]
```

| Bits | Width | Field |
|------|-------|-------|
| 5:0 | 6 | GPIO Pin Number |
| 6 | 1 | I/O Type (0=GPIO, 1=dedicated lock pin) |
| 7 | 1 | Initialize State (0=OFF at boot, 1=ON at boot) |
| 15:8 | 8 | Function (see function list) |
| 23:16 | 8 | Output HW Select |
| 28:24 | 5 | Input HW Select |
| 29 | 1 | GSYNC Header connection |
| 30 | 1 | Reserved |
| 31 | 1 | PWM enable |
| 35:32 | 4 | Lock Pin Number (0xF = N/A) |
| 36 | 1 | Off Data (FT) |
| 37 | 1 | Off Enable (FE) |
| 38 | 1 | On Data (OT) |
| 39 | 1 | On Enable (OE) |

Key GPIO Functions:
- 7 = Hotplug A, 8 = Hotplug B, 81 = Hotplug C, 82 = Hotplug D
- 9 = Fan control
- 0/1/2 = LCD0 backlight/power/power status
- 16 = Thermal + External Power Detect
- 255 (0xFF) = Skip Entry

---

## 6. Complete Parsing Walkthrough

### Step 1: Read VBIOS ROM
Use PRAMIN (BAR0+0x700000), PROM (BAR0+0x300000), PCI ROM, or ACPI `_ROM`.

### Step 2: Validate ROM
Check for signature `0xAA55` at offset 0.

### Step 3: Find BIT Table
Scan for bytes `FF B8 42 49 54` ("\\xff\\xb8BIT").

### Step 4: Parse BIT Tokens
```
entry_size = vbios[bit_offset + 9]
entry_count = vbios[bit_offset + 10]
for each entry at (bit_offset + 12 + i * entry_size):
    id = vbios[entry + 0]
    version = vbios[entry + 1]
    data_length = u16_le(vbios[entry + 2..3])
    data_pointer = u16_le(vbios[entry + 4..5])
```

### Step 5: Find DCB
```
dcb_ptr = u16_le(vbios[0x36..0x37])
dcb_version = vbios[dcb_ptr]
dcb_hdr_size = vbios[dcb_ptr + 1]
dcb_entry_count = vbios[dcb_ptr + 2]
dcb_entry_size = vbios[dcb_ptr + 3]
assert u32_le(vbios[dcb_ptr + 6..9]) == 0x4EDCBDCB
```

### Step 6: Parse DCB Entries
```
for i in 0..entry_count:
    entry_offset = dcb_ptr + dcb_hdr_size + i * dcb_entry_size
    dword0 = u32_le(vbios[entry_offset..+4])
    dword1 = u32_le(vbios[entry_offset+4..+8])
    
    if dword0 == 0x00000000 or dword0 == 0xFFFFFFFF: break
    
    type       = dword0 & 0xF
    if type == 0xF: continue  // skip
    if type == 0xE: break     // EOL
    
    i2c_index  = (dword0 >> 4)  & 0xF
    heads      = (dword0 >> 8)  & 0xF
    connector  = (dword0 >> 12) & 0xF
    bus        = (dword0 >> 16) & 0xF
    location   = (dword0 >> 20) & 0x3
    or_mask    = (dword0 >> 24) & 0xF
```

### Step 7: Parse I2C Table
```
i2c_ptr = u16_le(vbios[dcb_ptr + 4..5])
i2c_ver = vbios[i2c_ptr]
i2c_hdr = vbios[i2c_ptr + 1]
i2c_cnt = vbios[i2c_ptr + 2]
i2c_len = vbios[i2c_ptr + 3]

for i in 0..i2c_cnt:
    entry = i2c_ptr + i2c_hdr + i * i2c_len
    // Parse based on version (see Section 4)
```

### Step 8: Parse Connector Table
```
conn_ptr = u16_le(vbios[dcb_ptr + 20..21])
conn_ver = vbios[conn_ptr]
conn_hdr = vbios[conn_ptr + 1]
conn_cnt = vbios[conn_ptr + 2]
conn_len = vbios[conn_ptr + 3]
platform = vbios[conn_ptr + 4]

for i in 0..conn_cnt:
    entry = conn_ptr + conn_hdr + i * conn_len
    dword = u32_le(vbios[entry..+4])
    conn_type = dword & 0xFF
    location  = (dword >> 8) & 0xF
    // ... etc
```
