# NVIDIA Ampere (GA102/RTX 3080) — Nouveau Driver Register Reference

All register offsets, bitmasks, and logic extracted directly from the Linux kernel
nouveau driver source (`drivers/gpu/drm/nouveau/nvkm/`).

Sources: https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/nouveau/nvkm/

---

## 1. GFW_BOOT Wait Implementation (Device Init)

**Source**: `subdev/devinit/tu102.c` — used by GA100/GA102 via `tu102_devinit_post()`

GA102 uses `ga100_devinit_new()` which wires `.post = tu102_devinit_post`.
`tu102_devinit_post()` calls `tu102_devinit_wait()`.

### Exact Code Logic

```c
static int
tu102_devinit_wait(struct nvkm_device *device)
{
    unsigned timeout = 50 + 2000;           // 2050 iterations

    do {
        if (nvkm_rd32(device, 0x118128) & 0x00000001) {      // GFW_BOOT status
            if ((nvkm_rd32(device, 0x118234) & 0x000000ff) == 0xff)  // GFW ready
                return 0;
        }

        usleep_range(1000, 2000);           // 1-2ms per iteration
    } while (timeout--);

    return -ETIMEDOUT;
}
```

### Register Details

| Register     | Name (inferred)     | Mask         | Expected Value | Purpose                        |
|-------------|---------------------|-------------|----------------|--------------------------------|
| `0x118128`  | GFW_BOOT_STATUS     | `0x00000001` | bit 0 = 1      | GFW boot progress started      |
| `0x118234`  | GFW_BOOT_PROGRESS   | `0x000000ff` | `0xff`         | GFW boot fully complete         |

### Timeout Calculation
- **Max iterations**: 2050 (= 50 + 2000)
- **Sleep per iteration**: 1000–2000 µs (1–2 ms)
- **Total max wait**: ~2.05s to ~4.1s
- **Return**: `0` on success, `-ETIMEDOUT` on failure

### Pre-conditions
The `tu102_devinit_post()` function is called from the device preinit path:
```
nvkm_device_preinit()
  -> nvkm_devinit_post()
    -> tu102_devinit_post()
      -> tu102_devinit_wait()
```
BAR0 must be mapped (`device->pri` != NULL) before this is called.

### GA100 Display Disable Check
From `subdev/devinit/ga100.c`:
```c
static void
ga100_devinit_disable(struct nvkm_devinit *init)
{
    struct nvkm_device *device = init->subdev.device;
    u32 r820c04 = nvkm_rd32(device, 0x820c04);

    if (r820c04 & 0x00000001)
        nvkm_subdev_disable(device, NVKM_ENGINE_DISP, 0);
}
```

| Register    | Mask         | Meaning                          |
|------------|-------------|----------------------------------|
| `0x820c04` | `0x00000001` | bit 0 = 1 → display engine fused off |

### GM107+ Display Disable Check (pre-Ampere)
```c
u32 r021c04 = nvkm_rd32(device, 0x021c04);
if (r021c04 & 0x00000001)
    nvkm_subdev_disable(device, NVKM_ENGINE_DISP, 0);
```
| Register    | Mask         | Meaning                          |
|------------|-------------|----------------------------------|
| `0x021c04` | `0x00000001` | bit 0 = 1 → display fused off (GM107–TU1xx) |

### PLL Set for GA100+ (VPLL)
```c
// GA100+ VPLL programming (per head, head = type - PLL_VPLL0)
nvkm_wr32(device, 0x00ef00 + (head * 0x40), 0x02080004);
nvkm_wr32(device, 0x00ef18 + (head * 0x40), (N << 16) | fN);
nvkm_wr32(device, 0x00ef04 + (head * 0x40), (P << 16) | M);
nvkm_wr32(device, 0x00e9c0 + (head * 0x04), 0x00000001);
```

### PLL Set for TU102 (VPLL)
```c
// TU102 VPLL programming (per head)
nvkm_wr32(device, 0x00ef10 + (head * 0x40), fN << 16);
nvkm_wr32(device, 0x00ef04 + (head * 0x40), (P << 16) | (N << 8) | (M << 0));
nvkm_wr32(device, 0x00ef0c + (head * 0x40), 0x00000900);
nvkm_wr32(device, 0x00ef00 + (head * 0x40), 0x02000014);
```

---

## 2. PMC Enable / Interrupt Registers (Master Control)

### GA100/GA102 (Ampere) — `subdev/mc/ga100.c`

GA100+ uses a **different PMC_ENABLE register** at `0x000600` instead of the legacy `0x000200`.

#### Device Enable/Disable (PMC_ENABLE)

```c
// GA100+: PMC_ENABLE moved from 0x000200 to 0x000600

static void ga100_mc_device_enable(struct nvkm_mc *mc, u32 mask)
{
    nvkm_mask(device, 0x000600, mask, mask);    // set bits → enable
    nvkm_rd32(device, 0x000600);                // flush
    nvkm_rd32(device, 0x000600);                // double flush
}

static void ga100_mc_device_disable(struct nvkm_mc *mc, u32 mask)
{
    nvkm_mask(device, 0x000600, mask, 0x00000000);  // clear bits → disable
    nvkm_rd32(device, 0x000600);
    nvkm_rd32(device, 0x000600);
}

static bool ga100_mc_device_enabled(struct nvkm_mc *mc, u32 mask)
{
    return (nvkm_rd32(device, 0x000600) & mask) == mask;
}
```

#### GA100 MC Init
```c
static void ga100_mc_init(struct nvkm_mc *mc)
{
    nvkm_wr32(device, 0x000200, 0xffffffff);    // legacy enable all
    nvkm_wr32(device, 0x000600, 0xffffffff);    // new enable all
}
```

| Register    | Name               | Purpose                           |
|------------|--------------------|------------------------------------|
| `0x000200` | PMC_ENABLE (legacy) | Legacy engine enable (still written on init) |
| `0x000600` | PMC_ENABLE (GA100+) | New engine enable register for Ampere+ |

**Important**: GA100/GA102 MC does **NOT** register interrupt handling — when running with GSP-RM (`nvkm_gsp_rm(device->gsp)`), `ga100_mc_new()` returns `-ENODEV` and MC is not instantiated. Interrupts are handled by the VFN (Virtual Function Network) subdev instead.

#### Pre-Ampere PMC_ENABLE (NV04–GP10x) for comparison
```c
// 0x000200 — used by all GPUs NV04 through GP10x
nvkm_mask(device, 0x000200, mask, mask);    // enable
nvkm_mask(device, 0x000200, mask, 0);       // disable
```

### Interrupt Registers

#### GP100/TU102 (used when MC exists, pre-GSP path)

PMC Interrupt registers:

| Register        | Name                | Purpose                              |
|----------------|---------------------|--------------------------------------|
| `0x000100`     | PMC_INTR_STATUS(0)  | Interrupt status leaf 0              |
| `0x000104`     | PMC_INTR_STATUS(1)  | Interrupt status leaf 1 (if exists)  |
| `0x000140`     | PMC_INTR_EN(0)      | Interrupt enable leaf 0 (nv04 style) |
| `0x000160 + n*4` | PMC_INTR_ALLOW(n) | Allow interrupts (GP100+ style)     |
| `0x000180 + n*4` | PMC_INTR_BLOCK(n) | Block interrupts (GP100+ style)     |

#### Interrupt Bits for Display (from gf100/gk104/gp100)

Across all generations GF100+, the display interrupt bit is consistent:

| Engine          | Interrupt Bit    | Hex Mask     |
|-----------------|-----------------|--------------|
| **DISP**        | bit 26          | `0x04000000` |
| FIFO            | bit 8           | `0x00000100` |
| GR              | bit 12          | `0x00001000` |
| PRIVRING        | bit 30          | `0x40000000` |
| BUS             | bit 28          | `0x10000000` |
| FB              | bits 13,25,0    | `0x08002000` |
| LTC             | bit 25          | `0x02000000` |
| PMU             | bit 24          | `0x01000000` |
| GPIO/I2C        | bit 21          | `0x00200000` |
| TIMER           | bit 20          | `0x00100000` |
| THERM           | bit 18          | `0x00040000` |
| FAULT (GP100+)  | bit 9           | `0x00000200` |
| TOP (GK104+)    | bit 12          | `0x00001000` |

**Display interrupt = bit 26 = `0x04000000`** — consistent from GF100 through GP100/TU102.

---

## 3. PMC_BOOT_0 Chip Identification

**Source**: `engine/device/base.c`, function `nvkm_device_ctor()`

### Reading BOOT_0

```c
boot0 = nvkm_rd32(device, 0x000000);    // PMC_BOOT_0
```

| Register   | Name       | Purpose           |
|-----------|------------|-------------------|
| `0x000000` | PMC_BOOT_0 | Chip identification register |
| `0x000004` | PMC_BOOT_1 | Endianness detection         |

### Chip ID Decoding Logic

```c
// For chips NV10+:
if ((boot0 & 0x1f000000) > 0) {
    device->chipset = (boot0 & 0x1ff00000) >> 20;   // bits [28:20] = chipset
    device->chiprev = (boot0 & 0x000000ff);          // bits [7:0]   = revision
}
```

#### Bitmask Breakdown of PMC_BOOT_0

```
  31  28  27  24  23  20  19       8  7        0
 +----+----+----+----+----+---------+----------+
 | ?? | flags  |  chipset |  (misc) | chiprev  |
 +----+--------+---------+---------+----------+
        \_______/\________/                \____/
     0x1f000000  0x1ff00000               0x000000ff
      (>0 test)  (>>20 = chipset)        (revision)
```

### Architecture Detection from Chipset

```c
switch (device->chipset & 0x1f0) {
    case 0x160: device->card_type = TU100; break;    // Turing
    case 0x170: device->card_type = GA100; break;    // Ampere
    case 0x190: device->card_type = AD100; break;    // Ada Lovelace
    case 0x1a0: device->card_type = GB10x; break;    // Blackwell
    case 0x1b0: device->card_type = GB20x; break;    // Blackwell
    // ...
}
```

### GA102 (RTX 3080) Specific Values

- **Chipset number**: `0x172`
- **Card type**: `GA100` (Ampere family)
- **PMC_BOOT_0 expected pattern**: `0x172000a1` (approximately; bits [28:20] = `0x172`, bits [7:0] = revision `0xa1`)
- **Chipset mask test**: `0x172 & 0x1f0 = 0x170` → maps to `GA100` card type
- **Internal name**: `"GA102"`

The full chipset mapping for Ampere:

| Chipset | Name  | PMC_BOOT_0 bits [28:20] |
|---------|-------|-------------------------|
| `0x170` | GA100 | `0x170`                 |
| `0x172` | GA102 | `0x172`                 |
| `0x173` | GA103 | `0x173`                 |
| `0x174` | GA104 | `0x174`                 |
| `0x176` | GA106 | `0x176`                 |
| `0x177` | GA107 | `0x177`                 |

### GA102 Chipset Configuration (subdevs/engines wired up)

```c
nv172_chipset = {
    .name    = "GA102",
    .acr     = ga102_acr_new,
    .bar     = tu102_bar_new,
    .bios    = nvkm_bios_new,
    .devinit = ga100_devinit_new,      // → uses tu102_devinit_post for GFW_BOOT
    .fault   = tu102_fault_new,
    .fb      = ga102_fb_new,
    .gpio    = ga102_gpio_new,
    .gsp     = ga102_gsp_new,
    .i2c     = gm200_i2c_new,
    .imem    = nv50_instmem_new,
    .ltc     = ga102_ltc_new,
    .mc      = ga100_mc_new,           // → PMC_ENABLE at 0x000600
    .mmu     = tu102_mmu_new,
    .pci     = gp100_pci_new,
    .privring = gm200_privring_new,
    .timer   = gk20a_timer_new,
    .top     = ga100_top_new,
    .vfn     = ga100_vfn_new,
    .ce      = ga102_ce_new,           // 5 copy engines (mask 0x1f)
    .disp    = ga102_disp_new,
    .dma     = gv100_dma_new,
    .fifo    = ga102_fifo_new,
    .gr      = ga102_gr_new,
    .nvdec   = ga102_nvdec_new,        // 2 NVDEC engines (mask 0x03)
    .sec2    = ga102_sec2_new,
};
```

### Endianness Detection (PMC_BOOT_1)

```c
u32 pmc_boot_1 = nvkm_rd32(device, 0x000004);
// 0x00000000 → GPU is little-endian
// 0x01000001 → GPU is big-endian
// To switch: nvkm_wr32(device, 0x000004, 0x01000001);
```

### Other Identification Registers

| Register    | Name           | Purpose                                |
|------------|----------------|----------------------------------------|
| `0x000000` | PMC_BOOT_0     | Chipset ID + revision                  |
| `0x000004` | PMC_BOOT_1     | Endianness register                    |
| `0x101000` | NV_PSTRAPS     | Strapping pins (crystal freq, etc.)    |

### Crystal Frequency from Straps

```c
strap = nvkm_rd32(device, 0x101000);
// For NV17+ (except NV20-NV24):
strap &= 0x00400040;

switch (strap) {
    case 0x00000000: device->crystal = 13500; break;  // 13.5 MHz
    case 0x00000040: device->crystal = 14318; break;  // 14.318 MHz
    case 0x00400000: device->crystal = 27000; break;  // 27 MHz
    case 0x00400040: device->crystal = 25000; break;  // 25 MHz
}
```

### vGPU Detection (Turing+)

```c
boot1 = nvkm_rd32(device, 0x000004);
if (device->card_type >= TU100 && (boot1 & 0x00030000)) {
    // This is a vGPU — not supported
}
```

---

## 4. PCI Expansion ROM / VBIOS Reading

**Source**: `subdev/bios/shadow.c`, `shadowrom.c`, `shadowramin.c`, `shadowpci.c`

### BIOS Source Priority

The driver tries multiple sources in order (from `shadow.c`):

```c
struct shadow mthds[] = {
    { 0, &nvbios_of },          // OpenFirmware (DeviceTree)
    { 0, &nvbios_ramin },       // PRAMIN window (BAR0 + 0x700000)
    { 0, &nvbios_prom },        // PROM (BAR0 + 0x300000)
    { 0, &nvbios_acpi_fast },   // ACPI _ROM method
    { 4, &nvbios_acpi_slow },   // ACPI slow path (skip=4)
    { 1, &nvbios_pcirom },      // PCI ROM mapping
    { 1, &nvbios_platform },    // Platform ROM resource
    {}
};
```

Each method is scored; highest score wins. `skip` values mean "don't try this
method if the best score so far is >= skip".

### Method 1: PROM (BAR0 Shadow) — `shadowrom.c`

Reads VBIOS via BAR0 registers at offset `0x300000`:

```c
static u32
nvbios_prom_read(void *data, u32 offset, u32 length, struct nvkm_bios *bios)
{
    struct nvkm_device *device = data;
    if (offset + length <= 0x00100000) {         // max 1MB
        for (i = offset; i < offset + length; i += 4)
            *(u32 *)&bios->data[i] = nvkm_rd32(device, 0x300000 + i);
        return length;
    }
    return 0;
}
```

Before reading, ROM shadow must be disabled via PCI config:
```c
static void *
nvbios_prom_init(struct nvkm_bios *bios, const char *name)
{
    // Disabled for NV4C+ (NV40 family with chipset >= 0x4c)
    if (device->card_type == NV_40 && device->chipset >= 0x4c)
        return ERR_PTR(-ENODEV);
    nvkm_pci_rom_shadow(device->pci, false);   // disable shadow to expose ROM
    return device;
}
```

The ROM shadow control:
```c
void nvkm_pci_rom_shadow(struct nvkm_pci *pci, bool shadow)
{
    u32 data = nvkm_pci_rd32(pci, 0x0050);     // PCI config space offset
    if (shadow)
        data |= 0x00000001;      // enable shadow (hide ROM)
    else
        data &= ~0x00000001;     // disable shadow (expose ROM)
    nvkm_pci_wr32(pci, 0x0050, data);
}
```

| Offset in BAR0 | Purpose                |
|----------------|------------------------|
| `0x300000`     | PROM base — VBIOS ROM access (up to 1MB) |

| PCI Config Reg (via BAR0) | Bit      | Purpose              |
|--------------------------|----------|----------------------|
| PCI cfg + `0x0050`       | bit 0    | ROM shadow enable    |

### Method 2: PRAMIN — `shadowramin.c`

Reads VBIOS via the PRAMIN window at BAR0 + `0x700000`:

```c
static u32
pramin_read(void *data, u32 offset, u32 length, struct nvkm_bios *bios)
{
    if (offset + length <= 0x00100000) {         // max 1MB
        for (i = offset; i < offset + length; i += 4)
            *(u32 *)&bios->data[i] = nvkm_rd32(device, 0x700000 + i);
        return length;
    }
    return 0;
}
```

PRAMIN initialization for different generations:

```c
// For GA100+ (Ampere):
if (device->card_type >= GA100)
    addr = device->chipset == 0x170;  // special case for GA100

// For GM100+ (Maxwell 2+):
else if (device->card_type >= GM100)
    addr = nvkm_rd32(device, 0x021c04);   // display fuse register

// For Fermi/Kepler:
else if (device->card_type >= NV_C0)
    addr = nvkm_rd32(device, 0x022500);

// If addr & 1, display is disabled → can't use PRAMIN
if (addr & 0x00000001)
    return ERR_PTR(-ENODEV);

// Find PRAMIN window base:
if (device->card_type >= GV100)
    addr = nvkm_rd32(device, 0x625f04);   // Volta+
else
    addr = nvkm_rd32(device, 0x619f04);   // Maxwell/Pascal

// Check window is enabled and points to VRAM:
if (!(addr & 0x00000008))      // bit 3 = window enabled
    return ERR_PTR(-ENODEV);
if ((addr & 0x00000003) != 1)  // bits [1:0] = 1 means VRAM
    return ERR_PTR(-ENODEV);

// Calculate physical address:
addr = (addr & 0xffffff00) << 8;
if (!addr) {
    addr = (u64)nvkm_rd32(device, 0x001700) << 16;
    addr += 0xf0000;
}

// Set PRAMIN window:
nvkm_wr32(device, 0x001700, addr >> 16);
```

| Register    | Purpose                                          |
|------------|--------------------------------------------------|
| `0x700000` | PRAMIN window base for VBIOS reads (up to 1MB)  |
| `0x001700` | PRAMIN window address register (shifted <<16)    |
| `0x619f04` | VBIOS instance pointer (Maxwell/Pascal)          |
| `0x625f04` | VBIOS instance pointer (Volta+, including Ampere)|
| `0x021c04` | Display fuse register (GM107–TU1xx)              |
| `0x820c04` | Display fuse register (GA100+)                   |
| `0x022500` | Display fuse register (Fermi/Kepler)             |

### Method 3: PCI ROM — `shadowpci.c`

Uses the Linux PCI subsystem to map the expansion ROM:

```c
pci_enable_rom(pdev);
priv->rom = pci_map_rom(pdev, &priv->size);
memcpy_fromio(bios->data + offset, priv->rom + offset, length);
```

### Method 4: NV04 ROM Disable

After init, ROM access is disabled:
```c
// In nv04_mc_init():
nvkm_wr32(device, 0x001850, 0x00000001);   // disable ROM access
```

| Register    | Value       | Purpose                |
|------------|-------------|------------------------|
| `0x001850` | `0x00000001` | Disable ROM access    |

---

## Summary: Complete Register Map for GA102 Hardware Access

### Identification & Boot
| Register     | Bits          | Purpose                              |
|-------------|---------------|--------------------------------------|
| `0x000000`  | `[28:20]`     | PMC_BOOT_0: chipset ID (GA102=0x172) |
| `0x000000`  | `[7:0]`       | PMC_BOOT_0: chip revision            |
| `0x000004`  | all           | PMC_BOOT_1: endianness               |
| `0x101000`  | `0x00400040`  | Strap pins (crystal frequency)       |

### GFW Boot Wait
| Register     | Bits/Mask      | Purpose                            |
|-------------|----------------|------------------------------------|
| `0x118128`  | `0x00000001`   | GFW boot status (bit 0 = started)  |
| `0x118234`  | `0x000000ff`   | GFW progress (0xff = complete)     |

### Device Enable (PMC)
| Register     | Purpose                                    |
|-------------|---------------------------------------------|
| `0x000200`  | PMC_ENABLE legacy (still written on init)   |
| `0x000600`  | PMC_ENABLE new (GA100+, read/write)         |

### Display Fuse Detection
| Register     | Bit          | Generation         |
|-------------|-------------|--------------------| 
| `0x021c04`  | bit 0       | GM107–TU1xx        |
| `0x820c04`  | bit 0       | GA100+ (Ampere)    |

### Interrupts (PMC, pre-GSP path)
| Register           | Purpose                            |
|-------------------|------------------------------------|
| `0x000100`        | PMC_INTR_STATUS leaf 0             |
| `0x000140`        | PMC_INTR_EN leaf 0 (legacy arm)    |
| `0x000160 + n*4`  | PMC_INTR_ALLOW(n) — GP100+ allow  |
| `0x000180 + n*4`  | PMC_INTR_BLOCK(n) — GP100+ block  |

Display interrupt bit: **bit 26 = `0x04000000`**

### VBIOS Access
| Register/Range  | Purpose                              |
|----------------|--------------------------------------|
| `0x300000`     | PROM: direct ROM read (1MB window)   |
| `0x700000`     | PRAMIN: instance memory read (1MB)   |
| `0x001700`     | PRAMIN window address (<<16)         |
| `0x625f04`     | VBIOS instance pointer (Volta+)      |
| `0x001850`     | ROM access disable                   |
| PCI cfg `0x0050` bit 0 | ROM shadow toggle              |

### VPLL Programming (GA100+, per head)
| Register                  | Value/Layout                        |
|--------------------------|-------------------------------------|
| `0x00ef00 + head*0x40`   | `0x02080004` (enable)               |
| `0x00ef18 + head*0x40`   | `(N << 16) \| fN`                   |
| `0x00ef04 + head*0x40`   | `(P << 16) \| M`                    |
| `0x00e9c0 + head*0x04`   | `0x00000001` (trigger)              |
