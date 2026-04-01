# NVIDIA Ampere (GA102) Register Reference for Bare-Metal Programming
## Extracted from Linux kernel nouveau driver source code

---

## 1. I2C Bit-Bang Controller (GF119+ / Ampere)

### 1.1 Architecture Overview

Ampere GPUs (GA102) reuse the GF119+ I2C controller. The nouveau driver uses `gm200_i2c` as the I2C subdev for GM200+, which continues through Turing and Ampere. On Ampere with GSP-RM firmware (`nvkm_gsp_rm()`), the I2C subdev returns `-ENODEV` because GSP-RM handles I2C internally. For bare-metal (non-GSP) access, the same GF119 I2C register layout applies.

### 1.2 I2C Bus Registers (Bit-Bang) — `busgf119.c`

Each I2C bus port has a 32-bit control/status register. The base address is calculated from the DCB `drive` index:

```
Register address = 0x00d014 + (drive * 0x20)
```

Where `drive` is the I2C port number from the DCB I2C table (ccbE.drive field).

#### Register bit layout at `0x00d014 + (drive * 0x20)`:

| Bit(s) | Name           | Description |
|--------|----------------|-------------|
| 0      | SCL_OUT        | Drive SCL line: 1 = release (high/float), 0 = pull low |
| 1      | SDA_OUT        | Drive SDA line: 1 = release (high/float), 0 = pull low |
| 2      | (unknown)      | Set during init (init writes 0x7 = bits 0,1,2 all set) |
| 4      | SCL_SENSE      | Read-back: current SCL line state (1=high, 0=low) |
| 5      | SDA_SENSE      | Read-back: current SDA line state (1=high, 0=low) |

#### Functions mapping:

```c
// Drive SCL: write bit 0
nvkm_mask(device, bus->addr, 0x00000001, state ? 0x00000001 : 0);

// Drive SDA: write bit 1
nvkm_mask(device, bus->addr, 0x00000002, state ? 0x00000002 : 0);

// Sense SCL: read bit 4
!!(nvkm_rd32(device, bus->addr) & 0x00000010);

// Sense SDA: read bit 5
!!(nvkm_rd32(device, bus->addr) & 0x00000020);

// Init: write 0x7 (release both lines + set bit 2)
nvkm_wr32(device, bus->addr, 0x00000007);
```

### 1.3 I2C Bit-Bang Timing Parameters — `bit.c`

```c
T_TIMEOUT   = 2200000 ns  (2.2 ms total SCL stretch timeout)
T_RISEFALL  = 1000 ns     (1 µs rise/fall time delay)
T_HOLD      = 5000 ns     (5 µs hold time)
```

The `udelay` granularity is `(nsec + 500) / 1000` microseconds.

When using the Linux `i2c-algo-bit` framework (non-internal mode):
```c
bit->udelay  = 10;                        // 10 µs per half-clock → ~50 kHz
bit->timeout = usecs_to_jiffies(2200);    // 2.2 ms timeout
```

### 1.4 I2C Bit-Bang Protocol — `bit.c`

**START condition:**
1. If SCL or SDA are low, drive SCL low, drive SDA high, raise SCL (wait for stretch)
2. Drive SDA low (START)
3. Delay T_HOLD (5 µs)
4. Drive SCL low
5. Delay T_HOLD (5 µs)

**STOP condition:**
1. Drive SCL low
2. Drive SDA low
3. Delay T_RISEFALL (1 µs)
4. Drive SCL high
5. Delay T_HOLD (5 µs)
6. Drive SDA high (STOP)
7. Delay T_HOLD (5 µs)

**Write bit:**
1. Drive SDA to desired value
2. Delay T_RISEFALL (1 µs)
3. Raise SCL (with clock-stretch wait, up to 2200 iterations of 1 µs)
4. Delay T_HOLD (5 µs)
5. Drive SCL low
6. Delay T_HOLD (5 µs)

**Read bit:**
1. Drive SDA high (release for input)
2. Delay T_RISEFALL (1 µs)
3. Raise SCL (with clock-stretch wait)
4. Delay T_HOLD (5 µs)
5. Sample SDA (bit 5 of register)
6. Drive SCL low
7. Delay T_HOLD (5 µs)

### 1.5 DCB I2C Port Mapping — `base.c`

I2C busses are created from the VBIOS DCB I2C table:
```c
// Primary I2C bus: from DCB I2C table offset +4, bits [3:0]
// Secondary I2C bus: from DCB I2C table offset +4, bits [7:4]
NVKM_I2C_BUS_PRI → auxidx & 0x0f
NVKM_I2C_BUS_SEC → (auxidx & 0xf0) >> 4
```

Bus creation types from DCB:
- `DCB_I2C_NV04_BIT` → `bus_new_0(pad, id, drive, sense)` — legacy dual-register
- `DCB_I2C_NV4E_BIT` / `DCB_I2C_NVIO_BIT` → `bus_new_4(pad, id, drive)` — GF119+ single-register
- `DCB_I2C_NVIO_AUX` → `aux_new_6(pad, id, auxch)` — DP AUX channel
- `DCB_I2C_PMGR` → either `bus_new_4` or `aux_new_6` based on drive/auxch fields

### 1.6 I2C Pad Mode Switching (Shared I2C/AUX pads) — `padg94.c`

For GPUs with shared I2C/AUX pads (hybrid pads, e.g., DP connectors):

```
Pad base = 0x00e500 + ((pad_id - NVKM_I2C_PAD_HYBRID(0)) * 0x50)
```

| Mode | Register operations |
|------|-------------------|
| OFF  | `mask(0x00e50c + base, 0x00000001, 0x00000001)` — disable pad |
| I2C  | `mask(0x00e500 + base, 0x0000c003, 0x0000c001)` — I2C mode, then `mask(0x00e50c + base, 0x00000001, 0x00000000)` — enable |
| AUX  | `mask(0x00e500 + base, 0x0000c003, 0x00000002)` — AUX mode, then `mask(0x00e50c + base, 0x00000001, 0x00000000)` — enable |

### 1.7 Ampere-Specific I2C Notes

- GA102 uses `gm200_i2c` which is functionally identical to GF119 for I2C bus (same `gf119_i2c_pad_x_new` / `gf119_i2c_bus_new`)
- The pad_s_new function uses `gm200_i2c_pad_s_new` (different from GF119 for shared pads)
- AUX channel count: `aux = 8` (GM200+), vs `aux = 4` (GF119)
- GM200+ uses `gk104_aux_stat` / `gk104_aux_mask` instead of `g94_aux_stat` / `g94_aux_mask`
- GM200 adds `aux_autodpcd` function at register `0x00d968 + (aux * 0x50)`, bit 16
- **When GSP-RM is active (Ampere default), the kernel I2C subdev is disabled entirely** — I2C is handled by GSP firmware

---

## 2. GPIO Subsystem (GF119+ / Ampere)

### 2.1 GPIO Pin Registers — `gf119.c`

Each GPIO line has a 32-bit register:

```
GPIO pin register = 0x00d610 + (line * 4)
```

Where `line` is 0-31 (up to 32 GPIO lines).

#### Register bit layout at `0x00d610 + (line * 4)`:

| Bit(s) | Name           | Description |
|--------|----------------|-------------|
| [7:0]  | CONFIG         | Configuration byte (from VBIOS GPIO table `unk0` field) |
| 12     | OUTPUT_VAL     | Output value: 1 = high, 0 = low |
| 13     | DIRECTION      | Direction: 0 = output, 1 = input (note: driver XORs with 1) |
| 14     | INPUT_VAL      | Read-back: current pin state (1=high, 0=low) |

#### GPIO drive function:
```c
// dir: 1 = output, 0 = input
// out: 1 = high, 0 = low
u32 data = ((dir ^ 1) << 13) | (out << 12);
nvkm_mask(device, 0x00d610 + (line * 4), 0x00003000, data);
nvkm_mask(device, 0x00d604, 0x00000001, 0x00000001);  // trigger update
```

#### GPIO sense (read pin state):
```c
!!(nvkm_rd32(device, 0x00d610 + (line * 4)) & 0x00004000);
```

### 2.2 GPIO Special Routing Register

```
GPIO routing = 0x00d740 + (unk1 * 4)
```
Where `unk1` is from the VBIOS GPIO table entry bits [28:24]. The value written is the GPIO `line` number.

### 2.3 GPIO Reset from VBIOS — `gf119.c`

VBIOS GPIO table entry format (32-bit):
```
Bits [5:0]   = line     (GPIO line number)
Bit  [7]     = defs     (default state)
Bits [15:8]  = func     (function code, e.g., DCB_GPIO_PANEL_POWER)
Bits [23:16] = unk0     (config byte → written to 0x00d610 bits [7:0])
Bits [28:24] = unk1     (routing index, if non-zero)
```

### 2.4 GPIO Interrupt Registers (GK104+) — `gk104.c`

| Register         | Description |
|-----------------|-------------|
| `0x00dc00`      | Interrupt status 0 (lines 0-15 hi/lo) |
| `0x00dc08`      | Interrupt enable 0 |
| `0x00dc80`      | Interrupt status 1 (lines 16-31 hi/lo) |
| `0x00dc88`      | Interrupt enable 1 |

**Interrupt layout** (per 32-bit status register):
- Bits [15:0]: HI transitions (rising edge) for lines 0-15 / 16-31
- Bits [31:16]: LO transitions (falling edge) for lines 0-15 / 16-31

### 2.5 GPIO Update Register
```
0x00d604 bit 0 — write 1 to trigger GPIO output update
```

---

## 3. Display Engine — Ampere (GA102)

### 3.1 Display Engine Architecture

GA102 uses the NVDisplay architecture (same as GV100/TU102), NOT the older EVO architecture. Key differences from pre-GV100:
- **Window-based compositing** instead of overlay/base channels
- **Per-head** timing/sync registers at `0x616xxx + (head * 0x800)`
- **Core channel** at BAR0 `0x680000` (shadow at `0x680000 + 0x8000` for armed state)
- **Window channels** at BAR0 `0x690000 + ((chid-1) * 0x1000)`
- **SOR (Serial Output Resource)** registers at `0x61c000 + (sor_id * 0x800)`
- Display class state at `0x680000` (core) and `0x612xxx` (MMIO direct)

### 3.2 Display Ownership and Init — `tu102.c` (used by GA102)

GA102 uses `tu102_disp_init` for initialization:

```c
// 1. Claim display ownership
0x6254e8: bit 1 = display busy, bit 0 = release request
   Write bit 0 = 0 to release, poll bit 1 until clear

// 2. Lock pin capabilities
0x640008: Pin capabilities lock register (write 0x00000021 for TU102+)

// 3. SOR capabilities (for each SOR i)
0x61c000 + (i * 0x800): SOR capability readback
0x640000: bit (8+i) = SOR enable in caps
0x640144 + (i * 0x08): SOR capability shadow

// 4. Head capabilities (for each head id)
0x616300 + (id * 0x800): RG (Raster Generator) capability
0x640048 + (id * 0x020): RG capability shadow
0x616140 + (id * 0x800) + j: POSTCOMP capabilities (5 dwords, j=0..16)
0x640680 + (id * 0x20) + j:  POSTCOMP capability shadow

// 5. Window capabilities (for each window i)
0x640004: bit i = window enable in caps
0x630100 + (i * 0x800) + j: Window capability readback (6 dwords)
0x640780 + (i * 0x20) + j:  Window capability shadow
0x64000c: bit 8 = final window caps commit

// 6. IHUB capabilities
0x62e000 + (i * 4): IHUB cap readback (3 dwords for TU102, 4 for GV100)
0x640010 + (i * 4): IHUB cap shadow

// 7. Enable display
0x610078: bit 0 = display enable

// 8. Instance memory setup
0x610010: [2:0] = memory target (1=VRAM, 2=NCOH, 3=HOST), bit 3 = enable
0x610014: instance memory address >> 16

// 9. Interrupt setup
0x611cf0: CTRL_DISP interrupt mask (0x187 = AWAKEN|ERROR|SUPER1|SUPER2|SUPER3)
0x611db0: CTRL_DISP interrupt enable
0x611cec: EXC_OTHER interrupt mask (cursors + core)
0x611dac: EXC_OTHER interrupt enable
0x611ce8: EXC_WINIM interrupt mask
0x611da8: EXC_WINIM interrupt enable
0x611ce4: EXC_WIN interrupt mask
0x611da4: EXC_WIN interrupt enable
0x611cc0 + (head * 4): HEAD_TIMING interrupt mask (bit 2 = VBLANK)
0x611d80 + (head * 4): HEAD_TIMING interrupt enable
0x611cf4: OR interrupt mask
0x611db4: OR interrupt enable
```

### 3.3 Head (CRTC) Registers — GV100+ / Ampere

All head registers use stride `0x800` per head. **Direct MMIO registers** (active/live state):

| Register | Description |
|----------|-------------|
| `0x616300 + (h*0x800)` | RG capability / sync config |
| `0x616330 + (h*0x800)` | RGPOS: current vline (read locks hline) |
| `0x616334 + (h*0x800)` | RGPOS: current hline |

**Core channel head state** (EVO/NVDisplay shadow registers, stride `0x400` per head):

| Register (asy=`0x682xxx`) | Register (arm=`0x682xxx+0x8000`) | Description |
|--------------------------|----------------------------------|-------------|
| `0x682000 + (h*0x400)` | `0x68a000 + (h*0x400)` | HEAD_SET_CONTROL |
| `0x682004 + (h*0x400)` | `0x68a004 + (h*0x400)` | HEAD_SET_CONTROL (output depth) — bits [7:4]: 5=30bpp, 4=24bpp, 1=18bpp |
| `0x68200c + (h*0x400)` | `0x68a00c + (h*0x400)` | HEAD_SET_PIXEL_CLOCK (Hz) |
| `0x682064 + (h*0x400)` | `0x68a064 + (h*0x400)` | HEAD_SET_DISPLAY_TOTAL — [31:16]=vtotal, [15:0]=htotal |
| `0x682068 + (h*0x400)` | `0x68a068 + (h*0x400)` | HEAD_SET_SYNC_END — [31:16]=vsynce, [15:0]=hsynce |
| `0x68206c + (h*0x400)` | `0x68a06c + (h*0x400)` | HEAD_SET_BLANK_END — [31:16]=vblanke, [15:0]=hblanke |
| `0x682070 + (h*0x400)` | `0x68a070 + (h*0x400)` | HEAD_SET_BLANK_START — [31:16]=vblanks, [15:0]=hblanks |

**Head-attached SOR registers** (per-head, for DP audio/watermark):

| Register | Description |
|----------|-------------|
| `0x616528 + (h*0x800)` | HDA device entry select — bits [6:4] = head index |
| `0x616550 + (h*0x800)` | DP watermark — bit 27 set, bits [5:0] = watermark value |
| `0x616560 + (h*0x800)` | DP audio enable — bit 31 = trigger, bit 0 = enable |
| `0x616568 + (h*0x800)` | DP audio symbol count (h) — bits [15:0] |
| `0x61656c + (h*0x800)` | DP audio symbol count (v) — bits [23:0] |
| `0x616578 + (h*0x800)` | DP MST VCPI slot — bits [13:8]=slot_nr, [5:0]=slot |
| `0x61657c + (h*0x800)` | DP MST PBN — [31:16]=aligned, [15:0]=pbn |
| `0x6165c0 + (h*0x800)` | HDMI_CTRL — bit 30=enable, [20:16]=max_ac_packet, [6:0]=rekey |

### 3.4 Display Head Count and Mask

```c
// Head count and mask
0x610060: bits [7:0] = head present mask
0x610074: bits [3:0] = max head count

// SOR count and mask
0x610060: bits [15:8] = SOR present mask
0x610074: bits [11:8] = max SOR count

// Window count and mask
0x610064: window present mask
0x610074: bits [25:20] = max window count
```

### 3.5 SOR (Serial Output Resource) Registers — GA102

SOR base offset: `soff = sor_id * 0x800`
Link offset: `loff = soff + ((link == 2) ? 0x80 : 0x00)`

#### SOR Power Control

| Register | Description |
|----------|-------------|
| `0x61c004 + soff` | SOR power control — bit 31=busy/trigger, bit 0=power up (normal), bit 16=power up (sleep) |
| `0x61c030 + soff` | SOR sequencer control — bit 28=busy, [11:8]=powerdown_pc, [3:0]=powerup_pc |
| `0x61c034 + soff` | DP lane power — bit 31=trigger/busy |
| `0x61c040 + soff + (pc*4)` | SOR power sequencer entries |

**Power up sequence:**
```c
// Wait for not busy
poll (0x61c004 + soff) bit 31 == 0

// Set power up
mask(0x61c004 + soff, 0x80000001, 0x80000001)  // trigger + power up

// Wait for not busy
poll (0x61c004 + soff) bit 31 == 0

// Wait for sequencer done
poll (0x61c030 + soff) bit 28 == 0
```

#### SOR Clock Configuration

GA102 uses a dedicated clock register (different from GF119):

```c
// GA102 SOR clock — ga102_sor_clock()
0x00ec08 + (sor_id * 0x10): write 0x00000000
0x00ec04 + (sor_id * 0x10): write div2 (1 for TMDS high-speed, 0 otherwise)
```

For comparison, GF119 clock:
```c
// GF119 SOR clock (used by TU102)
0x612300 + soff: bits [10:8]=div2, [2:0]=div1, [22:18]=speed
// For TMDS: speed = 0x14 (high-speed) or 0x0a (normal)
// For DP: speed = bw << 18
```

#### SOR State Readback (GV100+)

```c
// SOR state: 0x680300 + coff
//   coff = (armed ? 0x8000 : 0) + sor_id * 0x20
0x680300 + coff: SOR protocol and head assignment
   bits [11:8] = proto_evo:
     0 = LVDS (link=1)
     1 = TMDS (link=1, single-link)
     2 = TMDS (link=2, dual-link B)
     5 = TMDS (link=3, dual-link A+B)
     8 = DP (link=1)
     9 = DP (link=2)
   bits [7:0] = head mask (which heads this SOR drives)
```

#### SOR Routing (GM200+ / GA102) — Output to SOR mapping

```c
// Route table: 0x612308 + (output_or * 0x100) for link A
//              0x612388 + (output_or * 0x100) for link B
//   output_or = __ffs(outp->info.or)

// Write: bits [3:0] = sor_id + 1 (0 = disconnected), bit 4 = link select
nvkm_mask(device, 0x612308 + moff, 0x0000001f, link << 4 | sor);

// Read: bits [3:0] = sor_id + 1, bit 4 = link
data = nvkm_rd32(device, 0x612308 + (m * 0x80));
```

### 3.6 DP Link Configuration — GA102

GA102 DP uses `ga102_sor_dp_links()` with extended bandwidth support:

```c
// DP clock/bandwidth at 0x612300 + soff, bits [22:18]:
// GA102 has enumerated bandwidth values:
//   bw=0x06 (RBR  1.62G): clksor = 0x00000000
//   bw=0x0a (HBR  2.70G): clksor = 0x00040000
//   bw=0x14 (HBR2 5.40G): clksor = 0x00080000
//   bw=0x1e (HBR3 8.10G): clksor = 0x000c0000
//   bw=0x08 (UHBR10):     clksor = 0x00100000
//   bw=0x09 (UHBR13.5):   clksor = 0x00140000
//   bw=0x0c (UHBR20):     clksor = 0x00180000
//   bw=0x10 (UHBR10 alt?):clksor = 0x001c0000

// DP control register at 0x61c10c + loff:
//   bits [19:16] = lane enable mask (e.g., 0xF for 4 lanes)
//   bit 30 = MST enable
//   bit 14 = Enhanced Framing enable

// Sequence:
nvkm_mask(device, 0x612300 + soff, 0x007c0000, clksor);        // set clock
nvkm_msec(device, 40, NVKM_DELAY);                              // 40ms delay
nvkm_mask(device, 0x612300 + soff, 0x00030000, 0x00010000);     // ???
nvkm_mask(device, 0x61c10c + loff, 0x00000003, 0x00000001);     // enable link
nvkm_mask(device, 0x61c10c + loff, 0x401f4000, dpctrl);         // set DP params
```

#### DP Lane Power — `g94_sor_dp_power()`

```c
// Enable lanes: 0x61c130 + loff, bits [3:0] = lane mask
// Lane mapping for GA102: lanes[] = { 0, 1, 2, 3 } (identity mapping)
nvkm_mask(device, 0x61c130 + loff, 0x0000000f, lane_mask);
nvkm_mask(device, 0x61c034 + soff, 0x80000000, 0x80000000);  // trigger
poll (0x61c034 + soff) bit 31 == 0  // wait for done
```

#### DP Training Pattern — `gm107_sor_dp_pattern()`

```c
// Training pattern register: 0x61c110 + soff (link A) or 0x61c12c + soff (link B)
// Each byte controls one lane's pattern:
//   0x10 = idle (no pattern)
//   0x01 = TPS1
//   0x02 = TPS2
//   0x03 = TPS3
//   0x1b = TPS4
nvkm_mask(device, 0x61c110 + soff, 0x1f1f1f1f, data);
```

#### DP Drive Settings — `gm200_sor_dp_drive()`

```c
// Per-lane drive current (DC):
//   0x61c118 + loff: 4 bytes, one per lane (shifted by lane*8)

// Per-lane pre-emphasis (PE):
//   0x61c120 + loff: 4 bytes, one per lane (shifted by lane*8)

// TX pre-shoot (PU) / power-up:
//   0x61c130 + loff: bits [11:8] = TX_PU value (max of all lanes)

// Post-cursor2 (PC):
//   0x61c13c + loff: 4 bytes, one per lane (shifted by lane*8)
```

#### DP SCDC / TMDS High-Speed — `gm200_sor_hdmi_scdc()`

```c
// HDMI SCDC scrambling control
0x61c5bc + soff: bit 0 = scrambling enable, bit 1 = high-speed enable
```

### 3.7 HDMI Registers (GV100+ / Ampere)

HDMI registers use a per-head stride of `0x400`:

| Register | Description |
|----------|-------------|
| `0x6f0000 + (head*0x400)` | AVI InfoFrame control — bit 0 = enable |
| `0x6f0008 + (head*0x400)` | AVI InfoFrame header |
| `0x6f000c + (head*0x400)` | AVI InfoFrame subpack0 low |
| `0x6f0010 + (head*0x400)` | AVI InfoFrame subpack0 high |
| `0x6f0014 + (head*0x400)` | AVI InfoFrame subpack1 low |
| `0x6f0018 + (head*0x400)` | AVI InfoFrame subpack1 high |
| `0x6f0080 + (head*0x400)` | ACR (Audio Clock Regen) — write 0x82000000 |
| `0x6f00c0 + (head*0x400)` | GCP (General Control Packet) control — bit 0 = enable |
| `0x6f00cc + (head*0x400)` | GCP data — write 0x00000010 |
| `0x6f0100 + (head*0x400)` | VSI InfoFrame control — bit 0 = enable, bit 16 = ??? |
| `0x6f0108 + (head*0x400)` | VSI InfoFrame header |
| `0x6f010c..0x6f0124 + (head*0x400)` | VSI InfoFrame subpacks |

---

## 4. DP AUX Channel — Hardware Interface

### 4.1 AUX Channel Registers (G94+, used through Ampere)

AUX base offset: `base = aux_ch * 0x50`

| Register | Description |
|----------|-------------|
| `0x00e4c0 + base + [0..15]` | AUX TX data buffer (16 bytes, 4 dwords) |
| `0x00e4d0 + base + [0..15]` | AUX RX data buffer (16 bytes, 4 dwords) |
| `0x00e4e0 + base` | AUX address register (20-bit DP AUX address) |
| `0x00e4e4 + base` | AUX control register |
| `0x00e4e8 + base` | AUX status register |

#### AUX Control Register (`0x00e4e4 + base`):

| Bit(s) | Description |
|--------|-------------|
| 31     | Reset (write 1 then 0) |
| [21:20]| Ownership request (0x10=req1, 0x20=req2) |
| [25:24]| Ownership acknowledge (0x01=ack1, 0x02=ack2) |
| 16     | Transaction trigger (write 1, poll until 0) |
| [15:12]| Transaction type (0=I2C_WR, 1=I2C_RD, 4=NATIVE_WR, 5=NATIVE_RD, etc.) |
| 8      | Address-only transaction flag |
| [7:0]  | Size - 1 (0-15 for 1-16 bytes) |

#### AUX Status Register (`0x00e4e8 + base`):

| Bit(s) | Description |
|--------|-------------|
| 28     | Sink detected (HPD) |
| [19:16]| AUX reply code (0x0=ACK, 0x2=NACK, 0x8=DEFER) |
| 8      | Timeout |
| [11:9] | Error bits |
| [4:0]  | Received data size |

#### AUX Transaction Sequence:

```c
// 1. Acquire pad (switch to AUX mode if shared)

// 2. Init: wait for idle, set ownership
poll (0x00e4e4 + base) & 0x03010000 == 0  // wait idle
mask(0x00e4e4 + base, 0x00300000, 0x00100000)  // request ownership
poll (0x00e4e4 + base) & 0x03000000 == 0x01000000  // wait ack

// 3. Check sink present
if (!(rd32(0x00e4e8 + base) & 0x10000000)) → sink not detected

// 4. Disable auto-DPCD (GM200+)
mask(0x00d968 + (aux * 0x50), 0x00010000, 0x00000000)

// 5. For write: load TX buffer
wr32(0x00e4c0 + base + 0, data[0])
wr32(0x00e4c0 + base + 4, data[1])
...

// 6. Set address
wr32(0x00e4e0 + base, dpaux_addr)

// 7. Build control word
ctrl = (type << 12) | (size - 1)  // or 0x100 for address-only

// 8. Execute
wr32(0x00e4e4 + base, 0x80000000 | ctrl)  // reset
wr32(0x00e4e4 + base, 0x00000000 | ctrl)  // clear reset
wr32(0x00e4e4 + base, 0x00010000 | ctrl)  // trigger
poll (0x00e4e4 + base) & 0x00010000 == 0  // wait done (2ms timeout)

// 9. Read status
stat = mask(0x00e4e8 + base, 0, 0)
reply = (stat & 0x000f0000) >> 16  // AUX reply

// 10. For read: read RX buffer
data[0] = rd32(0x00e4d0 + base + 0)
...
rx_size = stat & 0x1f

// 11. Re-enable auto-DPCD, release ownership
mask(0x00d968 + (aux * 0x50), 0x00010000, 0x00010000)
mask(0x00e4e4 + base, 0x00310000, 0x00000000)  // release
```

### 4.2 DP Link Training Sequence — `dp.c`

#### Clock Recovery (CR) — TPS1:

1. Set sink link config: write BW to `DPCD 0x100`, lane count to `DPCD 0x101`
2. Execute VBIOS link compare script
3. Call `ior->func->dp->links()` to configure SOR DP link registers
4. Call `ior->func->dp->power()` to enable lanes
5. Set TPS1 pattern on source and sink (`DPCD 0x102`)
6. Loop (max 5 tries, or until max swing reached):
   - Read drive settings from sink (`DPCD 0x206-0x207`)
   - Program drive settings via `dp->drive()` and write to sink `DPCD 0x103`
   - Wait 100µs (or DPCD 0x0e interval * 4ms for DP1.4+)
   - Read lane status from `DPCD 0x202-0x205`
   - Check CR_DONE for all lanes

#### Channel Equalization (EQ) — TPS2/3/4:

1. Set appropriate pattern:
   - DP1.4 with TPS4 support: pattern 4
   - DP1.2 with TPS3 support: pattern 3
   - Otherwise: pattern 2
2. Loop (max 6 tries):
   - Program drive + post-cursor2 settings
   - Wait 400µs (or DPCD interval)
   - Read lane status
   - Check CHANNEL_EQ_DONE + SYMBOL_LOCKED + INTERLANE_ALIGN_DONE

3. Set idle pattern (pattern 0) — clears scrambling disable

#### Ampere-Specific DP Notes:

- AMPERE_IED_HACK: IED scripts from VBIOS are no longer used by UEFI/RM on Ampere, but the x86 option ROM still has them. The `BeforeLinkTraining` script is executed differently.
- EFI GOP on Ampere can leave unused DP links routed — the `DisableLT` script is used to clean up.
- GA102 supports UHBR (Ultra High Bit Rate) link speeds via enumerated clock values.

---

## 5. PDISPLAY Register Map Summary (GV100+ / Ampere)

### 5.1 Top-Level PDISPLAY Ranges

All addresses are BAR0 offsets (MMIO space base `0x610000`):

| Range | Description |
|-------|-------------|
| `0x610000-0x6100ff` | Display engine global control |
| `0x610010-0x610014` | Instance memory base |
| `0x610060` | Head/SOR present mask |
| `0x610064` | Window present mask |
| `0x610074` | Max head/SOR/window counts |
| `0x610078` | Display enable |
| `0x6104e0+` | DMA channel control |
| `0x610630` | Core channel status |
| `0x610664+` | DMA channel status |
| `0x610b20-0x610b2c` | Core channel push buffer config |
| `0x611800+(h*4)` | HEAD_TIMING interrupt status |
| `0x61184c` | EXC_WIN status |
| `0x611850` | EXC_WINIM status |
| `0x611854` | EXC_OTHER status |
| `0x611858` | AWAKEN_WIN status |
| `0x61185c` | AWAKEN_OTHER status |
| `0x611cc0+(h*4)` | HEAD_TIMING interrupt mask |
| `0x611ce4` | EXC_WIN mask |
| `0x611ce8` | EXC_WINIM mask |
| `0x611cec` | EXC_OTHER mask |
| `0x611cf0` | CTRL_DISP mask |
| `0x611cf4` | OR mask |
| `0x611c30` | Supervisor interrupt status |
| `0x611d80+(h*4)` | HEAD_TIMING interrupt enable |
| `0x611da4` | EXC_WIN enable |
| `0x611da8` | EXC_WINIM enable |
| `0x611dac` | EXC_OTHER enable |
| `0x611db0` | CTRL_DISP enable |
| `0x611db4` | OR enable |
| `0x611ec0` | Master interrupt status |
| `0x612004` | SOR present (GF119) |
| `0x612300+(s*0x800)` | SOR clock/link config |
| `0x612308+(o*0x100)` | SOR routing table (link A) |
| `0x612388+(o*0x100)` | SOR routing table (link B) |

### 5.2 SOR Register Space

Per-SOR registers at `0x61c000 + (sor_id * 0x800)`:

| Offset from SOR base | Description |
|----------------------|-------------|
| `0x000` | SOR capability |
| `0x004` | SOR power control |
| `0x008` | SOR config |
| `0x00c` | SOR config 2 |
| `0x030` | SOR sequencer control |
| `0x034` | DP lane power trigger |
| `0x040+(pc*4)` | Power sequencer program entries |

Per-link registers at `0x61c000 + (sor_id * 0x800) + (link_B ? 0x80 : 0x00)`:

| Offset from link base | Description |
|-----------------------|-------------|
| `0x10c` | DP link control (lanes, MST, EF, active symbol TU) |
| `0x110` | DP training pattern (link A) |
| `0x118` | DP drive current (4 lanes packed) |
| `0x120` | DP pre-emphasis (4 lanes packed) |
| `0x128` | DP watermark / active symbol |
| `0x12c` | DP training pattern (link B) |
| `0x130` | DP lane power enable / TX_PU |
| `0x13c` | DP post-cursor2 (4 lanes packed) |

### 5.3 Head Direct MMIO Space

Per-head registers at `0x616000 + (head_id * 0x800)`:

| Offset from head base | Description |
|----------------------|-------------|
| `0x100+(j*4)` | POSTCOMP capability (GV100) |
| `0x140+(j*4)` | POSTCOMP capability (TU102) |
| `0x300` | RG capability / sync |
| `0x330` | Current vline position |
| `0x334` | Current hline position |
| `0x528` | HDA device entry |
| `0x550` | DP watermark |
| `0x560` | DP audio enable |
| `0x568` | DP audio symbol H |
| `0x56c` | DP audio symbol V |
| `0x578` | DP MST VCPI slot |
| `0x57c` | DP MST PBN |
| `0x5c0` | HDMI control |

### 5.4 Core Channel (EVO/NVDisplay) Shadow Space

The core channel has two copies: **assemble** (asy) at base offset, and **armed** (arm) at +0x8000.

**Head state** (core channel, per-head stride 0x400):

| Base `0x682000 + (h*0x400)` | Description |
|------------------------------|-------------|
| `+0x000` | HEAD_SET_CONTROL |
| `+0x004` | HEAD_SET_CONTROL (depth: bits[7:4]) |
| `+0x008` | HEAD_SET_CONTROL_OUTPUT_RESOURCE |
| `+0x00c` | HEAD_SET_PIXEL_CLOCK_FREQUENCY (Hz) |
| `+0x014` | HEAD_SET_CONTROL_CURSOR |
| `+0x060` | HEAD_SET_DISPLAY_ID |
| `+0x064` | HEAD_SET_DISPLAY_TOTAL: [31:16]=vtotal, [15:0]=htotal |
| `+0x068` | HEAD_SET_DISPLAY_SYNC_END: [31:16]=vsynce, [15:0]=hsynce |
| `+0x06c` | HEAD_SET_DISPLAY_BLANK_END: [31:16]=vblanke, [15:0]=hblanke |
| `+0x070` | HEAD_SET_DISPLAY_BLANK_START: [31:16]=vblanks, [15:0]=hblanks |

**SOR state** (core channel, per-SOR stride 0x20):

| Base `0x680300 + (s*0x20)` | Description |
|-----------------------------|-------------|
| `+0x000` | SOR_SET_CONTROL: [11:8]=proto, [7:0]=head_mask |

### 5.5 Display Channel User Space

| Channel | MMIO User Base | Size |
|---------|---------------|------|
| Core    | `0x680000` | 0x10000 |
| Window[n] | `0x690000 + (n * 0x1000)` | 0x1000 |
| Cursor  | via `0x6104e0` control | — |
| WIMM    | same as window | — |

### 5.6 Supervisor / Modeset Sequence

The display engine uses a supervisor interrupt mechanism for mode changes:

1. **Supervisor 1** (pending bit 0): Pre-modeset
   - Disable outputs being turned off
   - Execute `super_1_0`: disable head SOR routing
2. **Supervisor 2** (pending bit 1): Mode change
   - `super_2_0`: Program clock/PLL for head
   - `super_2_1`: Configure SOR output protocol
   - `super_2_2`: Enable output
3. **Supervisor 3** (pending bit 2): Post-modeset
   - `super_3_0`: Complete modeset, update armed state

**Supervisor registers:**
```
0x6107a8: Supervisor status — write 0x80000000 to acknowledge
0x6107ac + (head * 4): Per-head supervisor mask
   bit 12 (0x1000) = head mode change
   bit 16 (0x10000) = SOR assignment change
0x611860: Write pending bits to acknowledge supervisor interrupt
```

### 5.7 HDA (Audio) Capability Detection

```c
// GA102: HDA capability per SOR
0x08a15c: bit n = SOR n has HDA capability
```

---

## 6. Ampere-Specific Summary

### What changed from Turing (TU102) to Ampere (GA102):

1. **SOR Clock**: GA102 uses new clock registers at `0x00ec04/0x00ec08 + (sor_id * 0x10)` instead of the GF119-style `0x612300 + soff` clock divider.

2. **DP Bandwidth**: GA102 supports UHBR (Ultra High Bit Rate) link speeds with enumerated clock selector values in `0x612300`, extending beyond the simple `bw << 18` used in TU102.

3. **GSP-RM**: On Ampere, the GPU Services Processor Resource Manager handles most display, I2C, and GPIO operations. Bare-metal code bypasses GSP-RM and programs registers directly (same register interface).

4. **IED Scripts**: Ampere no longer uses IED (Initialization/Exit/Disable) scripts from UEFI/RM, though x86 option ROM still has them. The `AMPERE_IED_HACK` in the driver adjusts script execution accordingly.

5. **Display Init**: GA102 uses `tu102_disp_init` (slightly different from `gv100_disp_init` in POSTCOMP register offsets and pin capabilities handling).

6. **EFI GOP Cleanup**: Ampere-specific code handles cases where EFI GOP leaves unused DP links routed (calls `DisableLT` script).

### Register Address Quick Reference for Bare-Metal:

| Subsystem | Base | Stride | Key Register |
|-----------|------|--------|-------------|
| I2C Bus | `0x00d014` | `0x20` per port | SCL=bit0, SDA=bit1, sense=bits4,5 |
| I2C Pad | `0x00e500` | `0x50` per pad | Mode control |
| AUX | `0x00e4c0` | `0x50` per channel | TX/RX/ctrl/status |
| GPIO | `0x00d610` | `0x04` per line | dir=bit13, out=bit12, in=bit14 |
| SOR | `0x61c000` | `0x800` per SOR | Power, DP, link control |
| Head MMIO | `0x616000` | `0x800` per head | Live raster state |
| Core Chan | `0x680000` | `0x400` per head | Shadow mode registers |
| HDMI | `0x6f0000` | `0x400` per head | InfoFrame, ACR, GCP |
| SOR Clock (GA102) | `0x00ec04` | `0x10` per SOR | Clock divider |
| Display Global | `0x610000` | — | Control, interrupts |
