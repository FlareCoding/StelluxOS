# StelluxOS GPU Driver — RTX 3080 (GA102) Minimal Bring-Up & First-Pixel Specification

> A reverse-engineering–derived, evidence-cited specification for bringing up an
> **NVIDIA GeForce RTX 3080 (GA102, Ampere)** from cold to a **lit pixel on a
> detected monitor**, then to **144 Hz**, on **StelluxOS** (a from-scratch hobby
> kernel). Every load-bearing fact is cited to a primary source — either the
> **NVIDIA open-gpu-kernel-modules @535.183.01** source tree or our **on-hardware
> instrumented captures** of that driver running on the actual card.

---

## 0.1 What this document is (and is not)

This is the **minimal, vertical** path — only the logic required to make *this*
card light *one* pixel on *one* monitor. It is deliberately **not** a general or
complete driver. Each section marks what is **ESSENTIAL** for first pixel vs.
**SKIPPABLE**. Where a value could not be observed, it is labeled rather than
invented.

It was produced by instrumenting and tracing NVIDIA's open kernel driver on the
live RTX 3080, decoding the traces against the open source, then drafting →
deep-diving → judging → evaluating each section across four review waves. The
companion raw evidence (traces, decoders, the instrumented source tree) lives
under `/home/flare/dev/gpu-repro/` (see §0.8).

## 0.2 Motivation & goals

- **M0 (done, firmware FB):** pixels already exist via the UEFI/boot framebuffer.
- **M1 — first pixel via our own init (this spec's target):** PCIe → GSP boot →
  RPC object tree → detect the DP panel → modeset → scan out a red square at
  **2560×1440 @ 60 Hz** on **RM `displayId 0x800` (head 3 / SOR 1 / DP_B)**.
- **M2 — 144 Hz:** the same method program with one timing/clock change **plus a
  DisplayPort re-train from 2 → 4 lanes @ HBR2** (the 4-lane link was already
  observed to train cleanly).

## 0.3 Target hardware (verified)

- **GA102 "GeForce RTX 3080 Lite Hash Rate"**, PCI `10de:2216` (rev a1), subsystem
  Gigabyte `1458:403f`, at `0000:0b:00.0`. No iGPU.
- **BAR0** = 16 MB MMIO registers (`0xfb000000`, map **uncached**); **BAR1** =
  256 MB VRAM aperture (`0xd0000000`, ReBAR off → window, not full 10 GB);
  **BAR3** = 32 MB (`0xe0000000`). DMA width **47-bit**. IOMMU on (group 27).
- **VBIOS** `94.02.71.40.C4`. **VRAM** 10240 MiB.
- **GSP firmware (OS-agnostic, reusable verbatim):**
  `/lib/firmware/nvidia/535.183.01/gsp_ga10x.bin` (runs on the GPU's GSP).
- **Monitors (two connected):**
  - **DP panel = AOC AG241QG4**, RM `displayId 0x800`, **head 3 / SOR 1 / DP_B**,
    DRM connector **93 (DP-2)** — 2560×1440 @ **59.95 Hz** (pclk 241.5 MHz) and
    **143.91 Hz** (pclk 592 MHz). **This is the first-pixel + 144 Hz target.**
  - HDMI monitor, RM `displayId 0x2000`, head 2 / SOR 0, DRM connector 96.
  - DRM connector **88 (DP-1) is DISCONNECTED**.

## 0.4 Methodology & evidence provenance

1. Built NVIDIA `open-gpu-kernel-modules` **@tag 535.183.01** (fully open, dual
   MIT/GPL — legally reusable in a non-GPL kernel) on the test box (kernel
   6.8.0-38).
2. **mmiotrace was found unusable on Ampere** — it single-CPUs the machine and
   faults every MMIO, which times out the GSP boot (`RmInitAdapter failed`). So we
   added **source-level trace points** (tags `RVGBOOT`, `RVGREG`, `RVGTRACE`,
   `RVGPAY`, `RVGRESP`, `RVGEVO`, `RVGEVOD`) and session-loaded the build on the
   real card (no reboot), capturing a lossless kernel-log stream.
3. Decoded the traces against the open source: boot registers, the 663/≈1145 GSP
   RPCs, the display object tree, the EVO method program, and (gap-fill capture)
   the **measured** method data words + RPC reply payloads.

The open **source is the authoritative logic**; the **traces confirm which paths
actually run on this GA102 + the dynamic values**.

## 0.5 How to read this spec

Every non-trivial claim carries a label and a `path:line` citation:
- **[EVIDENCE]** — backed by a cited source line or a measured trace value.
- **[INFERENCE]** — reasoned from cited evidence (the basis is stated).
- **[TODO] / [UNCERTAIN] / [EVIDENCE-GAP]** — not confirmed; what to check is named.

`SRC` = `/home/flare/dev/gpu-repro/open-gpu-kernel-modules`. `CAP` =
`traces/20260530-112551-open-capture`. `CAP2` = `traces/20260530-115116-open-capture`
(gap-fill: data words + replies). `AN` = `analysis/`.

> **Precedence rule:** where any section's prose conflicts with the **Canonical
> Reconciled Facts (§0.6)**, §0.6 governs (it integrates the cross-section
> reconciliations from the judge/evaluator waves).

## 0.6 Canonical reconciled facts (authoritative)

1. **Two monitors / target identity:** first-pixel + 144 Hz target = **RM
   `displayId 0x800` = head 3 = SOR 1 = protocol DP_B = DRM connector 93 (DP-2)**,
   the AOC AG241QG4. Head 2 = HDMI `displayId 0x2000` / SOR 0 / connector 96.
   DRM connector **88 = DP-1 is disconnected**. (`HEAD_SET_DISPLAY_ID` is the
   unambiguous, measured token.)
2. **No on-screen test pattern was rendered:** both `modetest -s` runs failed
   (the harness `awk '/connected/'` matched "dis**connected**" → picked the dead
   port 88). The captured EVO methods are the driver's **load-time** modeset on the
   connected heads — **valid data**, just not a deliberate pattern. **60 Hz** dwords
   are **measured**; **144 Hz** dwords are **EDID-derived [INFERENCE]**.
3. **GSP boot = one Falcon "DMA-load-and-go" routine invoked 3×** (FWSEC on the
   GSP Falcon `0x110000`; Booter Load on SEC2 `0x840000`; GSP-RM bootloader on the
   GSP from local FB). **1768** register writes (**1058** GSP + **710** SEC2);
   **1689** are the 256-byte DMA inner loop; only ~**79** control writes across
   ~**22** distinct offsets are the real implementation surface. `DMATRFCMD`
   = `0x614` (IMEM) / `0x600` (DMEM); reset-into-RISC-V `BCR_CTRL=0x111`.
   **You must service the GSP CPU sequencer** (`CORE_RESUME` etc.) during the
   INIT_DONE wait or boot never completes. Poll throughout (MSI optional).
4. **RPC transport:** shared block `0x81000` (page table + two 256 KB rings);
   `rpc_message_header_v.header_version` = **`0x03000000`**, signature
   **`0x43505256`**; doorbell = one MMIO write `BAR0 + 0x110c00` (= 0). Counts:
   **663** RPCs (minimal capture) / **≈1145** (full modeset = 1143 sync + 2 async),
   all `result=0`.
5. **GSP-assigned handles** (read from the `GET_GSP_STATIC_INFO` (fn=65) reply; do
   **not** hardcode): `hInternalClient=0xc2000005`, `hInternalSubdevice=0xabcd2080`
   (`hInternalDevice≈0xabcd0080` [INFERENCE]). Used for every `NV2080_CTRL_..._INTERNAL_*`.
6. **Display object tree:** `NV01_ROOT → NV01_DEVICE_0 → NV20_SUBDEVICE_0`;
   `NV04_DISPLAY_COMMON` parents to the **DEVICE** (sibling of subdevice);
   `NVC670_DISPLAY` parents to the DEVICE; the **core (`NVC67D`) + window
   (`NVC67E`)** channels parent to `NVC670_DISPLAY`. First pixel = **1 head + 1
   window**.
7. **Modeset (measured 60 Hz):** `HEAD_SET_RASTER_SIZE=0x05c90aa0` (hTotal 2720 ×
   vTotal 1481), `VIEWPORT_SIZE_IN/OUT=0x05a00a00` (2560×1440), pclk ≈241.5 MHz,
   `SOR_SET_CONTROL=0x908` (head 3, DP_B), `UPDATE=0x1`. **144 Hz** [EDID]:
   `RASTER_SIZE=0x06070a6a` (2666×1543), pclk `0x23493400` (592 MHz).
   Host only **requests** the clock (`HEAD_SET_PIXEL_CLOCK_FREQUENCY.HERTZ`);
   **GSP-RM programs the VPLL** (no host PLL MMIO).
8. **Scanout / draw a pixel:** window `SET_CONTEXT_DMA_ISO` (bind your framebuffer
   surface) + `SET_OFFSET`, `SET_SIZE/_IN/_OUT=2560×1440`, `SET_PARAMS=0xE6`
   (`X8R8G8B8`), `SET_STORAGE` (**no `MEMORY_LAYOUT` field on Ampere — tiling is
   conveyed by the surface "kind"; THE top build risk**), `SET_PRESENT_CONTROL=0`,
   `SET_COMPOSITION_CONTROL=0x00010000` (BYPASS), `UPDATE=0x1`. A **red** pixel is
   `0x00FF0000` in X8R8G8B8.
9. **DisplayPort:** 60 Hz trained **2 lanes @ HBR2**; 144 Hz needs **4 lanes @
   HBR2** (sink = DP1.2/HBR2/4-lane caps the GPU's HBR3; the 4×HBR2 link assessed
   `err=0`). GSP-RM runs CR/EQ training behind a single `DP_CTRL`.

## 0.7 Section index & draft→final provenance

| Final | Title | Merged from |
|---|---|---|
| **F01** | Context, Goals, Scope, Architecture & Glossary | S01 |
| **F02** | OS-Side Prerequisites (what StelluxOS must provide) | S02 |
| **F03** | Firmware Blobs & GPU Memory Layout (WPR2/libos/radix3) | S03 |
| **F04** | GSP Boot: Falcon/SEC2 Register-Level Bring-up | S04 + S05 + D01 |
| **F05** | GSP RPC Transport (queues, header, doorbell, send/recv) | S06 + D02 |
| **F06** | RM Object Model & Bring-up RPC Sequence | S07 + D03 + D04 |
| **F07** | Display Engine Objects & Channels | S08 + D04 |
| **F08** | Detection, EDID, Connectors/SOR, DisplayPort & Mode Validation | S09 + D05 + D08 |
| **F09** | Modeset & Scanout: EVO Method Program, 144 Hz, Draw-a-Pixel | S10 + D06 + D07 + D10 |
| **F10** | The Minimal First-Pixel Critical Path & StelluxOS Module Layout | D09 |
| **F11** | De-Risk Validation: First Pixel Proven on Hardware | capture 20260530-122852 |

(Some section bodies still reference the draft `Sxx`/`Dxx` ids above; use this map
to resolve them. The 30-step build sequence is in **F10**.)

## 0.8 Master evidence & artifact index

- **Instrumented source + firmware:** `SRC/` (built `kernel-open/*.ko`); trace
  tags grep-able under `SRC` as `RVGBOOT|RVGREG|RVGTRACE|RVGPAY|RVGRESP|RVGEVO|RVGEVOD`.
- **Captures:** `CAP/` (boot+RPC+EVO method headers) and `CAP2/` (adds
  `evo-data-trace.txt` data words + `rpc-resp-trace.txt` replies). Each has
  `boot-trace.txt`, `rpc-trace.txt`, `evo-trace.txt`, `payload-trace.txt`,
  `modetest-list.txt`, `VERDICT.txt`, `dmesg-stream.txt`.
- **Decoders:** `decode_rpc.py`, `decode_full.py`, `decode_evo.py`,
  `decode_evo_values.py`, `decode_evo_full.py`.
- **Decoded:** `AN/*-rpc-decoded.txt`, `*-decoded-full.txt`, `*-evo-decoded.txt`,
  `*-evo-values.txt`, `*-evo-full.txt` (channel-attributed measured values).
- **Capture harness:** `capture_open.sh` (session-loads the build, streams the log,
  restores the proprietary driver).

## 0.9 Status of open gaps (see F11 for the de-risk validation)

**RESOLVED on hardware (F11):**
- ✓ **Surface "kind"** — a **pitch-linear** dumb buffer (tight `pitch=10240`) scans
  out at 60 **and** 144 Hz; StelluxOS can start pitch-linear (was the top risk).
- ✓ **On-wire 144 Hz** — `RASTER_SIZE=0x06070a6a` + pclk `0x23493400` (592 MHz)
  captured, matching the computed values → now **[EVIDENCE]**.
- ✓ **Drew our own pixel** — red @60 / green @144 via `red_pixel.c`, visible.
- ✓ **Harness `awk` connector bug** fixed → resolves `connector=93 (DP-2)`.

**Also RESOLVED (F11.6 / F11.7):**
- ✓ **GSP CPU-sequencer** fully captured (417 opcodes, 9-opcode set) → F04 sequencer is now
  `[EVIDENCE]`; the `MAILBOX0=0xfe` sentinel is explained.
- ✓ **`IS_MODE_POSSIBLE` reply** decoded (`dispClkKHz=1,350,000`, `bIsPossible=TRUE`) → F08 IMP
  is now `[EVIDENCE]`.

**Remaining (one Phase-2 detail; does not block a first pixel):**
- When StelluxOS allocates VRAM directly, confirm the PTE/"kind" flags that tag a surface
  pitch-linear to the display engine (the scanout *mode* is proven via DRM dumb buffers).
- Minor: fn=65 `fb_length` / alloc-param **values** are past the trace windows (FB size is
  known from `nvidia-smi`; alloc params are mostly NULL/zero); WPR `frtsOffset` runtime value.

---


## F01 — Context, Goals, Scope, Architecture & Glossary

> **Status:** FINAL (reconciled). This section supersedes draft `S01-context-glossary.md` and folds in
> `CONTEXT_ADDENDUM.md` + `CONSOLIDATED CORRECTIONS`. Every load-bearing claim below was re-verified by
> opening the cited `file:line`; claims that did not survive verification were corrected, downgraded, or
> deleted (see `### Open questions / TODO` and the change log at the end of `### Evidence cited`).
>
> **Citation roots** (same convention as `CONTEXT_BRIEF.md` §5):
> - `SRC`   = `/home/flare/dev/gpu-repro/open-gpu-kernel-modules` (open-gpu-kernel-modules @ 535.183.01)
> - `CAP`   = `/home/flare/dev/gpu-repro/traces/20260530-112551-open-capture` (boot + load-time modeset; visible-attempt run)
> - `CAP2`  = `/home/flare/dev/gpu-repro/traces/20260530-115116-open-capture` (gap-fill: EVO data words + RPC replies)
> - `CMIN`  = `/home/flare/dev/gpu-repro/traces/20260530-110235-open-capture` (minimal bring-up, 663 RPCs)
> - `AN`    = `/home/flare/dev/gpu-repro/analysis`
> - `DRAFTS`= `/home/flare/dev/gpu-repro/spec-drafts` (cross-refs to D0x/S0x deep-dives + BRIEF/ADDENDUM/CORRECTIONS)
>
> **Label policy:** **[EVIDENCE]** = backed by a cited `file:line` I opened. **[INFERENCE]** = reasoned
> from cited evidence (says from what). **[TODO]** = unverified (says exactly what to check). Inference is
> never presented as evidence.
> **Implementer target:** **StelluxOS**, a non-GPL hobby kernel at `/home/flare/dev/StelluxOS` (already has
> SSH, a display manager, SMP, profiling).

---

### F01.1 — Context & motivation

- **[EVIDENCE]** The deliverable is the GPU/display driver portion of StelluxOS; the test machine holds an
  **NVIDIA RTX 3080 (GA102, Ampere)**. (`DRAFTS/CONTEXT_BRIEF.md:7-13`)
- **[EVIDENCE]** The card identity is confirmed **on-wire**, not just from `lspci`: a `GSP_RM_CONTROL`
  (fn=76) reply carries `221610de 403f1458 000000a1` → PCI `10de:2216`, subsystem Gigabyte `1458:403f`,
  revision `a1`. (`CAP2/rpc-resp-trace.txt:3`) The `GET_GSP_STATIC_INFO` (fn=65) reply (`:2`) is the
  static-info RPC, but its payload runs past the 32-dword `RVGRESP` dump, so the ids are read here from the
  fn=76 reply that echoes them.
- **[EVIDENCE]** VBIOS **94.02.71.40.C4** was emitted live during boot, so the captured card matches the spec
  target: `RVGBOOT 02 FWSEC ucode parsed from VBIOS (ver 94.02.71.40.C4)`. (`CAP/VERDICT.txt:9`,
  `CAP/boot-trace.txt:2`)
- **[EVIDENCE]** GSP firmware on disk `/lib/firmware/nvidia/535.183.01/gsp_ga10x.bin` (~38 MB) runs on the
  GPU's GSP and is OS-agnostic — **reusable verbatim by StelluxOS**. (`DRAFTS/CONTEXT_BRIEF.md:47-48`)
- **[INFERENCE]** Because GSP-RM firmware is the "real" RM and is reused unchanged, StelluxOS only has to
  implement the **host-side client** (boot, RPC transport, object/method marshaling) — not the resource
  manager itself. (reasoned from `DRAFTS/CONTEXT_BRIEF.md:32-35` + boot/RPC evidence in F01.5/F01.6)

---

### F01.2 — Goals & success criteria

- **[EVIDENCE]** Milestone, in order: from a cold machine — **(1) initialize the card, (2) detect a monitor,
  (3) draw a pixel / small square on one detected monitor**, then **(4) drive the display manager at 144 Hz**.
  (`DRAFTS/CONTEXT_BRIEF.md:11-13`)
- **[EVIDENCE]** The refresh rate is a timing/clock setting on the **same** core-channel method program:
  refresh = `pixelClk / (hTotal × vTotal)`, selected by `HEAD_SET_PIXEL_CLOCK_FREQUENCY` + the raster totals.
  (`SRC/src/common/sdk/nvidia/inc/class/clc67d.h:693` `HEAD_SET_PIXEL_CLOCK_FREQUENCY(a)=0x200C+a*0x400`;
  `clc67d.h:828` `HEAD_SET_RASTER_SIZE(a)=0x2064+a*0x400`)
- **[INFERENCE]** The host does **not** program a PLL: it only *requests* the pixel clock via
  `HEAD_SET_PIXEL_CLOCK_FREQUENCY.HERTZ` and **GSP-RM programs the VPLL** (gated by `GET_PCLK_LIMIT` +
  `IS_MODE_POSSIBLE`). (CORRECTION #10; `DRAFTS/D10`)
- **[INFERENCE — cross-ref F01.4]** 60 Hz → 144 Hz on the **DP** target is **not** purely a timing change:
  60 Hz runs on **2 lanes @ HBR2**, but 2560×1440@144 (592 MHz) needs **4 lanes @ HBR2**, so the DP link is
  additionally **re-trained** (more lanes). This reuses the existing GSP-RM link-training path (`DP_CTRL`) —
  no new host architecture — and 4×HBR2 was already **assessed to train** on this exact link, so 144 Hz is
  reachable with the retrain step. (`DRAFTS/D08:30,32,38-39`; folded by S09/S10)
- **Success = a committed scanout.** **[INFERENCE]** Drawing the first pixel means: one framebuffer surface
  bound to one window channel, one head + its output resource programmed in the core channel, and a single
  `UPDATE` committed (`NVC67D_UPDATE=0x200`, `NVC67E_UPDATE=0x200`). (reasoned from method offsets
  `SRC/.../class/clc67d.h:82`, `SRC/.../class/clc67e.h:55`; sequence in S10)

---

### F01.3 — Scope (the minimal vertical) — most important usefulness rule

- **[EVIDENCE]** This is **not** a general/complete driver. It is an **extremely vertical, minimal** bring-up
  for **this exact GA102 + this exact panel**; every section separates *"essential for first pixel on GA102"*
  from *"general/optional/skip,"* preferring the shortest correct path. (`DRAFTS/CONTEXT_BRIEF.md:15-19`)

**Minimal-vertical goal, stated crisply (the spine of the whole spec):**

1. **Boot GSP-RM to `INIT_DONE`** (FWSEC → FRTS/WPR2 → reset into RISC-V → Booter Load → RISC-V active → wait).
   No GSP-RM ⇒ no control plane ⇒ no display. **[EVIDENCE]**
   `SRC/src/nvidia/src/kernel/gpu/gsp/arch/ampere/kernel_gsp_ga102.c:164-281`
2. **Stand up the RPC transport** (command/status rings) and the 4 opening RPCs, then `GSP_RM_ALLOC`(103) /
   `GSP_RM_CONTROL`(76). **[EVIDENCE]** `SRC/.../message_queue_cpu.c:57,85,91-96`;
   `AN/20260530-110235-open-capture-rpc-decoded.txt:1-8`
3. **Detect the DisplayPort panel** — RM **`displayId 0x800`**, display-engine **head 3** — via connect-state +
   EDID + DP AUX/DPCD, enough to validate one mode. **[EVIDENCE]** head↔displayId binding
   `AN/20260530-115116-open-capture-evo-full.txt:4711`; EDID/AUX controls
   `SRC/.../ctrl/ctrl0073/ctrl0073specific.h:145`, `SRC/.../ctrl/ctrl0073/ctrl0073dp.h:151`
4. **Draw a pixel / small square at 2560×1440 @ 60 Hz**: bind one framebuffer to one window channel
   (`SET_CONTEXT_DMA_ISO`), program one head + SOR in the core channel, commit a single `UPDATE`.
   **[EVIDENCE]** window bind `SRC/.../class/clc67e.h:181` `SET_CONTEXT_DMA_ISO(b)=0x240+b*4`; core
   `SRC/.../class/clc67d.h:301,567`
5. **Then raise to 2560×1440 @ 144 Hz** — same C67D/C67E method program, substituting the DP panel's
   **143.91 Hz / 592 MHz** timing (`hTotal 2666 × vTotal 1543`), **plus a DP link re-train from 2→4 lanes
   @ HBR2** (the extra bandwidth was already assessed to train on this link). **[EVIDENCE]**
   `CAP/modetest-list.txt:75` (EDID-derived; see F01.4 + CORRECTION #1); link math `DRAFTS/D08:38-39`.

**Essential vs skippable for first pixel on GA102:**

- **Essential — GSP boot.** Implement the exact `kgspBootstrapRiscvOSEarly_GA102` order and reuse
  `gsp_ga10x.bin` verbatim. **[EVIDENCE]** `SRC/.../arch/ampere/kernel_gsp_ga102.c:164-281`;
  `DRAFTS/CONTEXT_BRIEF.md:47-48`
- **Essential — RPC transport + the `72→73→1→65` opener + `GSP_RM_ALLOC`/`GSP_RM_CONTROL`.** **[EVIDENCE]**
  `SRC/.../message_queue_cpu.c:85,91-96`; `AN/20260530-110235-open-capture-rpc-decoded.txt:1-8`
- **Essential — minimal object tree + one head + one SOR + one window/ISO surface + `UPDATE`.** (object-tree
  detail = S07/S08; classes grounded in glossary). **[EVIDENCE]** alloc catalog
  `AN/20260530-110540-open-capture-decoded-full.txt:40,43`
- **Essential — detection just enough to validate one mode:** connect-state, EDID, DP AUX/DPCD for the DP
  panel. **[EVIDENCE]** `SRC/.../ctrl/ctrl0073/ctrl0073specific.h:145`, `SRC/.../ctrl/ctrl0073/ctrl0073dp.h:151`
- **Skippable for first pixel:** **7 of 8** window channels, **all 8** window-immediate, **all 4** cursor
  channels (use **1** window + **1** head); the HDMI monitor (head 2 / displayId 0x2000); PIOR/DAC/DSI/eDP
  paths; multi-arch HAL generality; perf/clock-boost, NVENC/NVFBC, UVM, MIG, P2P/NVLink RPCs.
  **[INFERENCE]** from the scope rule `DRAFTS/CONTEXT_BRIEF.md:15-19` and the fact that only DP/SOR/head/window
  classes appear in the modeset trace (channel counts verified: 1×C67D, 8×C67E, 4×C67A in
  `AN/20260530-110540-open-capture-decoded-full.txt`).
- **144 Hz is a *post-first-pixel* delta:** same method set, different `HEAD_SET_PIXEL_CLOCK_FREQUENCY` +
  raster totals, **plus** a DP link re-train to **4 lanes @ HBR2** (reuses the existing GSP-RM `DP_CTRL`
  training path — no new host architecture, but an extra step on the DP target). **[EVIDENCE]**
  `clc67d.h:693`; `CAP/modetest-list.txt:75`; lanes `DRAFTS/D08:30,38-39`

---

### F01.4 — Target hardware & displays (verified; full PCI detail belongs to S02)

**GPU / BARs.**
- **[EVIDENCE]** GPU `10de:2216` (rev a1), subsystem Gigabyte `1458:403f`, PCI `0000:0b:00.0`; single GPU,
  **no iGPU**. (`DRAFTS/CONTEXT_BRIEF.md:38-39`; on-wire `CAP2/rpc-resp-trace.txt:3`)
- **[EVIDENCE]** **BAR0 = 16 MB MMIO registers** @ `0xfb000000` — all `0x11xxxx`/`0x84xxxx` boot writes land
  here; **BAR1 = 256 MB VRAM aperture** @ `0xd0000000` (prefetchable, ReBAR off); **BAR3 = 32 MB** @
  `0xe0000000`; I/O port `0xf000`; 512 KB expansion-ROM VBIOS; DMA 47-bit; IOMMU on (group 27).
  (`DRAFTS/CONTEXT_BRIEF.md:40-43`) BAR1 as the FB aperture is independently visible in
  `CAP/nvidia-smi-q.txt:99` ("BAR1 Memory Usage", Total 256 MiB).
- **[TODO]** BAR3's concrete role (regs-alt / USERD / unused for first pixel) — confirm in S02; do not assume.

**Target displays — TWO monitors are connected (CORRECTION #1; this is the most-corrected fact).**

The earlier draft/brief described a single "connector id 88" monitor. That is **wrong/stale**: the captures
show **two** connected sinks and connector 88 is **disconnected**. Verified picture:

| Role (this spec) | RM `displayId` | display head | DRM/`modetest` connector (in `CAP`) | preferred mode | 144-class mode |
|---|---|---|---|---|---|
| **DP panel (TARGET)** | **`0x800`** | **head 3** | **DP-2 = connector 93** (connected, 32 modes) | 2560×1440 @ 59.95 Hz, 241.5 MHz | 2560×1440 @ **143.91 Hz, 592 MHz** (`hTot 2666 × vTot 1543`) |
| HDMI monitor (skip) | `0x2000` | head 2 | HDMI-A-2 = connector 96 (connected, 26 modes) | 2560×1440 @ 60.00 Hz, 241.7 MHz | 2560×1440 @ 144.00 Hz, 581.64 MHz |
| (stale "connector 88") | — | — | DP-1 = connector 88 (**disconnected, 0 modes**) | — | — |

- **[EVIDENCE]** Head→displayId binding is captured directly in the core channel:
  `NVC67D_HEAD_SET_DISPLAY_ID[3] = 0x00000800` and `NVC67D_HEAD_SET_DISPLAY_ID[2] = 0x00002000`.
  (`AN/20260530-115116-open-capture-evo-full.txt:4711,4780`)
- **[EVIDENCE]** Per-head pixel clocks distinguish the two sinks: head 3 `PIXEL_CLOCK_FREQUENCY_MAX = 0x0e64ff60`
  (**241,500,000 Hz** → DP panel @ 59.95 Hz) and head 2 `= 0x0e680ca0` (**241,700,000 Hz** → HDMI @ 60.00 Hz);
  both share raster `RASTER_SIZE = 0x05c90aa0` (`hTot 2720 × vTot 1481`). (`AN/...-evo-full.txt:4674,4681,4743,4750`)
- **[EVIDENCE]** `modetest` connector table: `88 ... disconnected DP-1 ... 0` modes; `93 ... connected DP-2`
  (32 modes); `96 ... connected HDMI-A-2` (26 modes). (`CAP/modetest-list.txt:15,71,149`)
- **[EVIDENCE]** DP panel (connector 93 / DP-2) mode list: `#0 2560x1440 59.95 ... 241500 ... preferred`,
  `#1 2560x1440 143.91 ... 2666 ... 1543 ... 592000`. (`CAP/modetest-list.txt:74-75`)
- **[EVIDENCE]** HDMI (connector 96) mode list: `#0 2560x1440 60.00 ... 241700 ... preferred`,
  `#1 2560x1440 144.00 ... 581640`. (`CAP/modetest-list.txt:152-153`) — **the 581.64 MHz / 144.00 Hz mode
  belongs to the HDMI sink, not the DP target.** (Draft S01 conflated these.)
- **Three distinct namespaces, reconciled** (per CORRECTION #1): **(a)** DRM/`modetest` connector id — in
  these captures the live DP panel is **DP-2 = connector 93**, while **connector 88 (DP-1) is disconnected**;
  **(b)** RM `displayId` — DP = `0x800`, HDMI = `0x2000`; **(c)** display-engine **head** — DP = head 3,
  HDMI = head 2. The brief's "connector 88" is a stale DRM id and is **not** the live panel; treat
  `displayId 0x800` / head 3 as the canonical target identity. **[EVIDENCE]** (connector ids
  `CAP/modetest-list.txt:15,71`; displayId↔head `AN/...-evo-full.txt:4711,4780`)
- **[INFERENCE]** DP panel is an **AOC AG241QG4** driven on **SOR 1, protocol DP_B**, trained **2 lanes @ HBR2**
  for 60 Hz, needing **4 lanes @ HBR2** for 144 Hz (sink is DP1.2/HBR2/4-lane, capping the GPU's HBR3). This
  comes from the DP-link/detection deep-dives, not re-verified first-party in F01. (per
  `DRAFTS/D06,D08,D10`; cross-ref S09/S10) **[TODO]** re-verify SOR index + lane/HBR from a primary
  `DFP_GET_INFO`/DP-link-config payload (the EVO trace did not decode `SOR_SET_CONTROL` owner/protocol).

---

### F01.5 — How the evidence was produced (method & why it is trustworthy)

- **[EVIDENCE]** We did **not** blind-dump registers. We took NVIDIA's **open-gpu-kernel-modules @ 535.183.01**
  (dual MIT/GPL, the canonical GSP-RM client), **added our own trace points**, session-loaded it on the real
  RTX 3080 (no reboot), exercised the GPU, and captured a lossless kernel-log stream. (`DRAFTS/CONTEXT_BRIEF.md:21-30`;
  harness `/home/flare/dev/gpu-repro/capture_open.sh:80-134`)
- **[EVIDENCE] Why source instrumentation and not mmiotrace:** mmiotrace **cannot be used on Ampere** — it
  single-CPUs the machine and faults on every MMIO, timing out GSP boot (`RmInitAdapter failed`). Source-level
  instrumentation is therefore the only viable method. (`DRAFTS/CONTEXT_BRIEF.md:27-30`)
- **[EVIDENCE] The trace tags are real and greppable in the tree:** `RVGBOOT` boot markers
  (`SRC/.../kernel_gsp.c:2704`; `SRC/.../arch/ampere/kernel_gsp_ga102.c:190,204,207,253,270,277,281`); `RVGREG`
  register writes; `RVGTRACE`/`RVGPAY` RPC+payload (`SRC/src/nvidia/kernel/vgpu/nv/rpc.c`); `RVGEVO`/`RVGEVOD`
  display methods + data words (`SRC/src/nvidia-modeset/include/nvkms-dma.h:124,265`).
- **[EVIDENCE] The capture succeeded end-to-end:** `VERDICT=OK`, 16093 dmesg lines, **1768** boot
  register-writes, **4930** EVO methods, 1036 payload dumps, all **10** boot stages present.
  (`CAP/VERDICT.txt:1-17`) The gap-fill run adds **4988** EVO data words + **1143** RPC reply dumps.
  (`CAP2/VERDICT.txt:5-8`)
- **[EVIDENCE] Boot took ~2.06 s wall** (stage 01 `t=3077.319160` → stage 10 `t=3079.380289`) across 1768
  writes. (`CAP/VERDICT.txt:8-17`)
- **[CORRECTION — visible modeset provenance]** The captured EVO modeset is from the **module-load-time
  modeset** (`nvidia-drm modeset=1`), **not** from `modetest`. The harness selected connector **88 by a bug**
  (`awk '/connected/'` also matches the substring in "**dis**connected", grabbing the first row, DP-1/88), so
  both `modetest -s` steps **failed**: "failed to find mode … for connector 88". The driver still modeset both
  *connected* heads at load (head 2 ← `0x2000`, head 3 ← `0x800`), which is what the 4930 EVO methods record.
  **[EVIDENCE]** bug `/home/flare/dev/gpu-repro/capture_open.sh:102`; failures `CAP/modetest-set-pref.txt:1`,
  `CAP/modetest-set-144.txt:1`; load-time modeset evidence `AN/...-evo-full.txt:4711,4780`.
  **[TODO]** A `modetest`-driven SMPTE *test pattern* (claimed "visible" in the addendum) is **not** evidenced
  in these runs; re-run the harness against the live connector (93) to capture an intentional visible set, or
  treat "visible test pattern" as unconfirmed.
- **[INFERENCE]** Net: treat the open source tree as the **schema/logic reference** and the traces as
  **proof-of-execution + concrete values** for *this* GA102. (reasoned from `DRAFTS/CONTEXT_BRIEF.md:32-35`,
  `CAP/VERDICT.txt:1-17`)

---

### F01.6 — End-to-end architecture (layers named)

There are **two cooperating planes**: a **control/allocation plane** that crosses to GSP-RM firmware via RPC,
and a **display-method plane** where the host fills EVO pushbuffers consumed by the display engine. Both must
work for first pixel.

```
                    HOST (CPU)  ── StelluxOS will implement this entire column ──
 ┌──────────────────────────────────────────────────────────────────────────┐
 │ L0  StelluxOS GPU driver (PCI enumerate, map BAR0/BAR1/BAR3, IRQ, DMA)     │
 │       BARs/IDs: BRIEF:38-43 ; on-wire ids CAP2/rpc-resp-trace.txt:3        │
 │                                                                            │
 │ L1  RMAPI client surface: RmAlloc / RmControl (object model)              │
 │       RmControl NVOS54 = nvos.h:2171-2180 ; RmAlloc NVOS21/64 = nvos.h:463,480│
 │                                                                            │
 │ L2  NVKMS / EVO method builder (modeset + scanout streams)                │
 │       nvkms-evo3.c:24-27 ("EVO HAL ... display class 3.x / nvdisplay")     │
 │                                                                            │
 │ L3  RPC marshaling: RmAlloc→GSP_RM_ALLOC(103), RmControl→GSP_RM_CONTROL(76)│
 │       rpc.c:1677,1887 ; ids rpc_global_enums.h:85,112                      │
 │                                                                            │
 │ L4  Message-queue transport: command-ring + status-ring in sysmem         │
 │       message_queue_cpu.c:57,85,91-96,158                                  │
 └───────────────┬───────────────────────────────┬──────────────────────────┘
                 │ (control/alloc plane)          │ (display-method plane)
                 │ RPC over msg rings             │ EVO pushbuffer (DMA) + UPDATE
                 ▼                                ▼
 ┌──────────────────────────────────┐   ┌─────────────────────────────────────┐
 │ GPU: GSP (RISC-V Falcon)         │   │ GPU: nvdisplay / EVO engine          │
 │  L5 GSP-RM firmware (the real RM)│   │  consumes core(C67D)/window(C67E)/   │
 │      runs in WPR2, under libos   │   │  cursor(C67A) channel methods, then  │
 │      kernel_gsp.c:2704,2770      │   │  HEAD/SOR drive scanout              │
 │  boot stack (one-time):          │   │   clc67d.h:34,567,301 ; clc67e.h:34  │
 │   FWSEC→FRTS→WPR2→Booter→RISC-V  │   └───────────────────┬─────────────────┘
 │   kernel_gsp_ga102.c:164-281     │                       │
 └──────────────────────────────────┘                       ▼
                                                  ┌──────────────────────┐
                                                  │ OR → SOR → DP link    │
                                                  │ OR-type enum          │
                                                  │ ctrl0073specific.h:   │
                                                  │ 1101 (SOR) ; AUX/DPCD │
                                                  │ ctrl0073dp.h:151      │
                                                  └──────────┬───────────┘
                                                             ▼
                                                  ┌──────────────────────┐
                                                  │ DP panel (1440p)      │
                                                  │ displayId 0x800,      │
                                                  │ head 3  (TARGET)      │
                                                  │ EDID: ctrl0073-       │
                                                  │ specific.h:145        │
                                                  └──────────────────────┘
```

- **[EVIDENCE]** Control/alloc plane: host RmControl/RmAlloc are wrapped into `GSP_RM_CONTROL`(76) /
  `GSP_RM_ALLOC`(103) RPCs and forwarded to GSP-RM. (`SRC/src/nvidia/kernel/vgpu/nv/rpc.c:1677,1887`;
  ids `SRC/.../rpc_global_enums.h:85,112`)
- **[EVIDENCE]** RPC transport is a command-ring + status-ring pair (msgq) in system memory.
  (`SRC/.../message_queue_cpu.c:57,85,91-96,158`)
- **[EVIDENCE]** Observed control-plane opening order on this card: `GSP_SET_SYSTEM_INFO`(72, async) →
  `SET_REGISTRY`(73, async) → `SET_GUEST_SYSTEM_INFO`(1) → `GET_GSP_STATIC_INFO`(65) → `GSP_RM_CONTROL`(76)…;
  every **sync** call returned `result=0x0`. (`AN/20260530-110235-open-capture-rpc-decoded.txt:1-10`)
- **[EVIDENCE]** Every RPC reply carries header `header_version = 0x03000000` + signature `0x43505256`
  (`w=03000000 43505256 …` on all 1143 replies). (`CAP2/rpc-resp-trace.txt:1-6`)
- **[EVIDENCE]** RPC volume depends on what you do: **663** RPCs in the minimal bring-up (553 `GSP_RM_CONTROL`
  + 83 `GSP_RM_ALLOC` + opener/FREE), and **≈1145** in a full modeset (**1143** sync reply pairs + 2 async).
  Always say which capture. (`AN/20260530-110235-open-capture-rpc-decoded.txt` send-count 663/553/83;
  `CAP2/VERDICT.txt:8`)
- **[EVIDENCE]** Display-method plane: NVKMS builds **EVO** method streams for display class 3.x ("nvdisplay"),
  pushed into **core (C67D)** / **window (C67E)** / **cursor (C67A)** channels. (`SRC/.../nvkms-evo3.c:24-27`;
  emit `SRC/.../nvkms-dma.h:265`) Trust the **channel-attributed** decode `…-evo-full.txt` over the older
  bucket-labelled `…-evo-decoded.txt` (CORRECTION #4/#5). (`AN/20260530-115116-open-capture-evo-full.txt`)
- **[INFERENCE]** The display-method plane reaches the panel through **OR → SOR → DP link**; SOR is the digital
  encoder (DP), with AUX/DPCD + EDID handled by `NV04_DISPLAY_COMMON` controls. (reasoned from OR-type enum
  `SRC/.../ctrl0073specific.h:1099-1105`, AUX `ctrl0073dp.h:151`, EDID `ctrl0073specific.h:145`)
- **[TODO]** Exact pushbuffer *kickoff* (PUT-pointer doorbell vs GSP-mediated submit; BAR0 vs sysmem) — confirm
  in `SRC/src/nvidia-modeset/src/nvkms-push.c` + `nvkms-dma.h`. Belongs to the modeset/scanout section (S10).

**One-time GSP boot stack (register level), summarized — full detail in S03/S04/S05.**
- **[EVIDENCE]** `kgspBootstrapRiscvOSEarly_GA102`: run **FWSEC** to set up **FRTS** (carve **WPR2**) → reset
  into RISC-V → run **Booter Load** (authenticate GSP-RM into WPR2) → RISC-V active → wait `INIT_DONE`.
  (`SRC/.../arch/ampere/kernel_gsp_ga102.c:164,190,204,207,253,270,277,281`)
- **[EVIDENCE]** The opening boot writes are Falcon DMA-controller programming loading ucode:
  `off=0x110110` (DMATRFBASE) `val=0x00cfb000`, `0x110114` (DMATRFMOFFS), `0x11011c` (DMATRFFBOFFS),
  `0x110118` (DMATRFCMD) `val=0x614`. (`CAP/boot-trace.txt:15-19`; defs
  `SRC/src/common/inc/swref/published/turing/tu102/dev_falcon_v4.h:71,73,75,92`)
- **[EVIDENCE]** All **1768** boot writes hit **only** GSP Falcon `0x11xxxx` (**1058**) and SEC2 Falcon
  `0x84xxxx` (**710**) — `1058 + 710 = 1768`, no other blocks. (first-party counts over `CAP/boot-trace.txt`)
- **[EVIDENCE]** `DMATRFCMD` values decode as 256-byte transfers (`SIZE_256B=6`): `0x614` = IMEM, `0x600` =
  DMEM — i.e., the boot is one DMA-load loop reused for code/data. (`dev_falcon_v4.h:75,87-88`;
  corroborates CORRECTION #8)

---

### F01.7 — GLOSSARY (every term an implementer will meet)

> Each term cites the `file:line` that defines the symbol or proves the mechanism. Where an *acronym
> expansion* is industry-standard but not literally spelled out in the tree, the expansion is **[INFERENCE]**
> and the *role* is grounded in source.

#### Boot / firmware / security

- **GSP** — *GPU System Processor* **[INFERENCE]** (expansion); an on-die microcontroller (a Falcon with a
  RISC-V core on Ampere) running NVIDIA firmware. Modeled host-side as `KernelGsp`; bring-up entry
  `kgspInitRm`. **[EVIDENCE]** `SRC/.../kernel_gsp.c:2704` (`RVGBOOT 01 kgspInitRm START`).
- **GSP-RM** — the **Resource-Manager firmware image that runs on the GSP** (the "real" RM on Ampere). Loaded
  + authenticated into WPR2; signals readiness via `INIT_DONE`. **[EVIDENCE]** `SRC/.../kernel_gsp.c:2770`;
  readiness `SRC/.../arch/ampere/kernel_gsp_ga102.c:281`. On-disk image `gsp_ga10x.bin`
  (`DRAFTS/CONTEXT_BRIEF.md:47-48`).
- **Falcon** — NVIDIA's family of small control microcontrollers ("FAst Logic CONtroller" **[INFERENCE]**)
  embedded in engines (GSP, SEC2, …). Programmed via a fixed register block; e.g. DMA-transfer regs
  `DMATRFBASE/MOFFS/CMD/FBOFFS` and `FALCON_OS`. **[EVIDENCE]** `SRC/.../dev_falcon_v4.h:42,71,73,75,92`.
- **RISC-V core** — on Ampere the GSP Falcon contains a RISC-V CPU that executes GSP-RM; boot verifies it is
  enabled and "reset into RISC-V." **[EVIDENCE]** `RVGBOOT 06 reset into RISC-V done`
  `SRC/.../arch/ampere/kernel_gsp_ga102.c:207`; `RVGBOOT 08 RISC-V active` `:270`.
- **SEC2** — the GPU's **Security engine #2** (also a Falcon). Executes the Booter/Scrubber ucodes during GSP
  bring-up; register block `0x84xxxx`. **[EVIDENCE]** `KernelSec2` use in
  `SRC/.../kernel_gsp_booter.c:154`; 710 writes to `0x84xxxx` (first-party count over `CAP/boot-trace.txt`).
- **FWSEC** — **FirmWare SECurity** ucode **[INFERENCE]** (expansion), shipped inside the VBIOS, extracted from
  ROM and parsed at boot; here it triggers FRTS. **[EVIDENCE]** "executing FWSEC ucode for FRTS"
  `SRC/.../arch/turing/kernel_gsp_frts_tu102.c:25`.
- **FRTS** — **FW Run-Time Security** **[INFERENCE]** (expansion); FWSEC carves a protected ~**1 MB** data
  region in FB so FRTS data + GSP-RM code/data/heap can coexist in WPR2. **[EVIDENCE]** size 1 MB
  `SRC/.../kernel_gsp_frts_tu102.c:43-55`; `RVGBOOT 05 FWSEC-FRTS done (WPR2 carved)`
  `SRC/.../arch/ampere/kernel_gsp_ga102.c:204`.
- **WPR2** — **Write-Protected Region 2**: an FB range guarded by the FB MMU into which GSP-RM is loaded +
  authenticated; boot fails early if WPR2 is already up. **[EVIDENCE]** protect register
  `NV_PFB_PRI_MMU_WPR2_ADDR_HI` referenced `SRC/.../kernel_gsp_frts_tu102.c:38`; "Fail early if WPR2 is up"
  `SRC/.../kernel_gsp.c:2647`.
- **Booter** — a pair of **SEC2 ucodes** (Booter Load / Booter Unload) that load, verify, and boot GSP-RM into
  WPR2 (Load) and tear it down (Unload). **[EVIDENCE]** `RVGBOOT 07 Booter Load done`
  `SRC/.../arch/ampere/kernel_gsp_ga102.c:253`; `SRC/.../kernel_gsp_booter.c:154`.
- **libos** — the lightweight runtime hosting GSP-RM tasks on the GSP; host side owns the log-decode
  structures. **[EVIDENCE]** libos/radix usage in `SRC/.../kernel_gsp.c` (`kgspCreateRadix3_IMPL:3457`).
- **radix3** — a **3-level radix page table** the host builds to describe a firmware image's pages to the GSP
  MMU/libos. **[EVIDENCE]** `kgspCreateRadix3_IMPL` `SRC/.../kernel_gsp.c:3457`.
- **devinit / GFW_BOOT** — **devinit** is the VBIOS device-init sequence (clocks, straps, FB) run from ROM;
  **GFW_BOOT** is the scratch-register handshake that signals devinit/FWSEC-from-ROM complete, which RM waits
  for. **[EVIDENCE]** `RVGBOOT 03 GFW_BOOT ok (VBIOS devinit/FWSEC-from-ROM complete)` `CAP/boot-trace.txt:3`;
  FRTS/GFW machinery `SRC/.../kernel_gsp_frts_tu102.c`.

#### PCI / addressing

- **BAR0** — 16 MB PCI BAR exposing the GPU's **MMIO register window** (`0xfb000000`); all `0x11xxxx`/`0x84xxxx`
  boot writes land here. **[EVIDENCE]** `DRAFTS/CONTEXT_BRIEF.md:40`; offsets `CAP/boot-trace.txt:5-22`.
- **BAR1** — 256 MB PCI BAR exposing a **VRAM (FB) aperture** (`0xd0000000`, prefetchable, ReBAR off).
  **[EVIDENCE]** `DRAFTS/CONTEXT_BRIEF.md:40`; "BAR1 Memory Usage" `CAP/nvidia-smi-q.txt:99`.
- **BAR3** — 32 MB PCI BAR (`0xe0000000`); secondary aperture. **[EVIDENCE]** `DRAFTS/CONTEXT_BRIEF.md:41`.
  **[TODO]** exact role (regs-alt/USERD/unused) — confirm in S02.

#### RM object model & RPC

- **RM (Resource Manager)** — NVIDIA's GPU resource manager. On GSP architectures it is split into a thin
  **host client** and the **GSP-RM** firmware. **[EVIDENCE]** RPC enum tags units `RM`/`GSP`
  `SRC/.../rpc_global_enums.h:81,85,112`.
- **RmAlloc** — RMAPI call that **creates an object** (class + parent handle + new handle + class-specific
  params). Parameter structs `NVOS21_PARAMETERS` / `NVOS64_PARAMETERS` (alloc-with-rights). Crosses to GSP as
  `GSP_RM_ALLOC`. **[EVIDENCE]** `SRC/src/common/sdk/nvidia/inc/nvos.h:458-463,469-480`.
- **RmControl** — RMAPI call that **invokes a control command** on an existing object (`hClient`,`hObject`,
  `cmd`,`params`). Crosses to GSP as `GSP_RM_CONTROL`. **[EVIDENCE]** NVOS54 (below).
- **NVOS54** — the RM-control parameter struct `NVOS54_PARAMETERS { hClient; hObject; cmd; flags; params;
  paramsSize; status; }`, function code `NV04_CONTROL = 0x36`. The literal wire-shape for every RmControl.
  **[EVIDENCE]** `SRC/.../nvos.h:2163,2171-2180`.
- **RPC** — the host↔GSP-RM **Remote Procedure Call** protocol: a function id + serialized params over the
  message-queue rings; replies carry `header_version 0x03000000` + signature `0x43505256`; sync calls in these
  captures returned `result=0x0`. **[EVIDENCE]** ids `SRC/.../rpc_global_enums.h`; replies
  `CAP2/rpc-resp-trace.txt:1-6`; results `AN/20260530-110235-open-capture-rpc-decoded.txt:4-10`.
- **GSP_RM_CONTROL** — RPC **function id 76**; carries an RmControl to GSP-RM. **[EVIDENCE]** id
  `SRC/.../rpc_global_enums.h:85`; emit `rpcWriteCommonHeader(... GSP_RM_CONTROL ...)` `SRC/.../rpc.c:1677`.
- **GSP_RM_ALLOC** — RPC **function id 103**; carries an RmAlloc to GSP-RM. **[EVIDENCE]** id
  `SRC/.../rpc_global_enums.h:112`; emit `rpcWriteCommonHeader(... GSP_RM_ALLOC ...)` `SRC/.../rpc.c:1887`.
- **class id** — the numeric **type code** of an RM object/class (passed as `hClass` on alloc), e.g.
  `NV01_ROOT=0x0`, `NV01_DEVICE_0=0x80`, `NV20_SUBDEVICE_0=0x2080`, `NV04_DISPLAY_COMMON=0x73`,
  `NVC670_DISPLAY=0xc670`. **[EVIDENCE]** `SRC/.../class/cl0000.h:42`, `cl0080.h:36`, `cl2080.h:36`,
  `clc670.h:32`; on-wire `0x73` in `AN/20260530-110540-open-capture-decoded-full.txt:43`.
- **handle** — a 32-bit, **client-scoped identifier** (`NvHandle`) naming an object instance; `hClient` names
  the client/root, `hObject`/`hObjectParent` name objects within it. **[EVIDENCE]** `SRC/.../nvos.h:2173-2174`.
  Object-tree shape: **`NV04_DISPLAY_COMMON` is parented to the DEVICE** (`hParent=0xcaf00000`), a **sibling of
  the subdevice** (`hObject=0xcaf00001`), not under it (CORRECTION #4). **[EVIDENCE]**
  `AN/...-decoded-full.txt:40,43`.
- **GSP-assigned internal handles** — the client/object handles GSP-RM uses for its own
  `NV2080_CTRL_CMD_INTERNAL_*` controls; **do not hardcode**. **[EVIDENCE]** seen on-wire as the
  `hClient`/`hObject` of internal fn=76 controls: `hInternalClient=0xc2000005`,
  `hInternalSubdevice=0xabcd2080` (`CAP2/rpc-resp-trace.txt:3-6`, `c2000005 abcd2080`). **[INFERENCE]** they
  are assigned by GSP-RM and surfaced via `GET_GSP_STATIC_INFO`(65) (CORRECTION #7 / `DRAFTS/D03`; the fn=65
  payload itself runs past the 32-dword `RVGRESP` cutoff). `hInternalDevice≈0xabcd0080` **[INFERENCE]**
  (per `DRAFTS/D03`).

#### Display: engine, channels, methods

- **NVKMS** — **NVIDIA Kernel Mode-Setting**, the `nvidia-modeset` module that owns mode validation and builds
  EVO method streams. **[EVIDENCE]** `SRC/.../nvkms-evo3.c:24-27` (EVO HAL for display class 3.x / "nvdisplay").
- **EVO** — NVIDIA's **display channel + method programming model** (display-engine front-end); on Ampere it is
  the class-3.x "nvdisplay" generation driven through per-channel pushbuffers. **[EVIDENCE]**
  `SRC/.../nvkms-evo3.c:24-27`; push routines `SRC/.../nvkms-dma.h:24`.
- **pushbuffer** — the **DMA buffer of method words** the host fills and the channel consumes. **[EVIDENCE]**
  `SRC/.../nvkms-dma.h:24` ("dma push buffer inlined routines").
- **method** — a single display command = **method offset + data word(s)**; the offset (in dwords) goes into
  `NV_UDISP_DMA_METHOD_OFFSET` (bits 13:2). **[EVIDENCE]** `SRC/.../nvkms-dma.h:247`; emit `RVGEVO M ch/off/cnt`
  `:265`, data `RVGEVOD off/data` `:124`.
- **core channel** — the per-display **master modeset channel**, class `NVC67D_CORE_CHANNEL_DMA = 0xC67D`;
  carries `HEAD_SET_*`, `SOR_SET_CONTROL`, then a committing `UPDATE`. **[EVIDENCE]** class `SRC/.../clc67d.h:34`;
  `UPDATE=0x200` `:82`; `SOR_SET_CONTROL(a)=0x300+a*0x20` `:301`; `HEAD_SET_CONTROL(a)=0x2008+a*0x400` `:567`;
  `HEAD_SET_PIXEL_CLOCK_FREQUENCY(a)=0x200C+a*0x400` `:693`; `HEAD_SET_RASTER_SIZE(a)=0x2064+a*0x400` `:828`.
  **One** core channel was allocated. **[EVIDENCE]** `AN/...-decoded-full.txt` (1× C67D).
- **window channel** — the **surface/scanout channel**, class `NVC67E_WINDOW_CHANNEL_DMA = 0xC67E`; binds the
  framebuffer via `SET_CONTEXT_DMA_ISO`, sets storage/format, then `UPDATE`. **Eight** were allocated.
  **[EVIDENCE]** class `SRC/.../clc67e.h:34`; `SET_SIZE=0x224` `:128`; `SET_STORAGE=0x228` `:131`;
  `SET_PARAMS=0x22C` `:139`; `SET_PLANAR_STORAGE(b)=0x230+b*4` `:177`; `SET_CONTEXT_DMA_ISO(b)=0x240+b*4` `:181`;
  `UPDATE=0x200` `:55`; count `AN/...-decoded-full.txt` (8× C67E).
- **window-immediate channel** — low-latency companion to a window, class
  `NVC67B_WINDOW_IMM_CHANNEL_DMA = 0xC67B` (×8). **[EVIDENCE]** `SRC/.../clc67b.h:34`.
- **cursor channel** — the **PIO** cursor channel, class `NVC67A_CURSOR_IMM_CHANNEL_PIO = 0xC67A` (×4 ⇒ 4
  heads). **[EVIDENCE]** `SRC/.../clc67a.h:32`; 4× allocated `AN/...-decoded-full.txt:808-826`.
- **ISO surface** — an **isochronous (scanout) memory surface** **[INFERENCE]** (expansion); the framebuffer
  the window channel scans out, bound by `SET_CONTEXT_DMA_ISO`. **[EVIDENCE]** `SRC/.../clc67e.h:181`.
- **head** — a **display pipe / timing generator** inside the display engine; per-head methods are stride-`0x400`
  in the core channel (head 2 `HEAD_SET_DISPLAY_ID` @ `0x2820`, head 3 @ `0x2c20`, Δ=0x400). This GA102 exposes
  **4 heads**. **[EVIDENCE]** `numHeads` `SRC/.../clc670.h:37`; offsets `AN/...-evo-full.txt:4780,4711`.
- **displayId** — the RM-level **bitmask id of a connected display** (target of `HEAD_SET_DISPLAY_ID`); **DP
  panel = `0x800`, HDMI = `0x2000`**. Distinct from a DRM/`modetest` connector id. **[EVIDENCE]**
  `AN/...-evo-full.txt:4711,4780`.
- **CRTC** — the **DRM/KMS name for a head**; userspace tools (`modetest`) speak CRTCs/connectors. **[INFERENCE]**
  CRTC ≈ NVIDIA "head" (1:1 here). StelluxOS can ignore it and program **heads** directly. (reasoned from
  `CAP/modetest-list.txt` vs `clc670.h:37`)

#### Display: output path & detection

- **OR (Output Resource)** — the **encoder/output block** that turns head pixels into a wire signal; type enum
  `NONE=0/DAC=1/SOR=2/PIOR=3/DSI=5`. **[EVIDENCE]** `SRC/.../ctrl0073specific.h:1099-1105`.
- **SOR (Serial Output Resource)** — the **digital encoder** OR used for DP/HDMI/DVI; `OR_TYPE_SOR=2`,
  controlled in the core channel via `SOR_SET_CONTROL`, count reported as `numSors`. The DP panel's OR.
  **[EVIDENCE]** type `ctrl0073specific.h:1101`; method `clc67d.h:301`; field `clc670.h:38`. **[INFERENCE]** DP
  panel = **SOR 1** (per `DRAFTS/D08`; not re-verified first-party — see F01.4 TODO).
- **PIOR** — a **legacy** OR type (`OR_TYPE_PIOR=3`); not expected on this GA102 DP path. **[EVIDENCE]**
  `ctrl0073specific.h:1102`. **[TODO]** confirm PIOR/DAC/DSI absent in the decoded ctrl stream (expected: SOR
  only).
- **EDID** — **Extended Display Identification Data**, the monitor's capability blob (read via
  `NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2 = 0x730245`); the `modetest` mode list is EDID-derived. **[EVIDENCE]**
  `SRC/.../ctrl0073specific.h:145`; mode list `CAP/modetest-list.txt:74-75`.
- **DP AUX** — the **DisplayPort auxiliary channel**, a low-speed sideband for link/EDID/DPCD transactions
  (`NV0073_CTRL_CMD_DP_AUXCH_CTRL = 0x731341`). **[EVIDENCE]** `SRC/.../ctrl0073dp.h:151`.
- **DPCD** — **DisplayPort Configuration Data**, the sink's register space read/written over AUX (caps, link
  config, power). **[EVIDENCE]** referenced in `SRC/.../ctrl0073dp.h` (DP control interface, `:41,151`).

---

### Evidence cited

> Every `file:line` below was opened and verified during this reconciliation. Roots per the header.

**Artifacts — captures/analysis**
- `CAP/VERDICT.txt:1-17` — `VERDICT=OK`; 16093 dmesg, 1768 boot writes, 4930 EVO, 1036 payloads; 10 boot
  stages w/ timestamps (boot ≈2.06 s); VBIOS `94.02.71.40.C4` (`:9`).
- `CAP/boot-trace.txt:1-25` — RVGBOOT markers + RVGREG writes; DMATRF programming at `:15-19`
  (`0x110110=0x00cfb000`, `0x110118=0x614`).
- `CAP/boot-trace.txt` (first-party counts) — 1768 total `RVGREG wr`; **1058** `off=0x11` + **710** `off=0x84`
  (= 1768; no other blocks). *(Upgrades draft S01's TODO to EVIDENCE.)*
- `CAP/modetest-list.txt:15,71,149` — connector table: 88 DP-1 **disconnected**, 93 DP-2 connected, 96
  HDMI-A-2 connected.
- `CAP/modetest-list.txt:74-75` — DP panel (conn 93) modes 59.95 Hz/241500 + 143.91 Hz/592000 (2666×1543).
- `CAP/modetest-list.txt:152-153` — HDMI (conn 96) modes 60.00 Hz/241700 + 144.00 Hz/581640.
- `CAP/modetest-set-pref.txt:1`, `CAP/modetest-set-144.txt:1` — both `modetest -s` **failed** ("…for connector 88").
- `CAP/nvidia-smi-q.txt:99-100` — "BAR1 Memory Usage", Total 256 MiB.
- `/home/flare/dev/gpu-repro/capture_open.sh:90,99-113,102` — load-time modeset; modetest step; **awk bug**
  selecting connector 88.
- `CAP2/VERDICT.txt:1-8` — `VERDICT=OK`; 4988 EVO data words; **1143** RPC replies.
- `CAP2/rpc-resp-trace.txt:1-6` — replies `w=03000000 43505256 …`; PCI ids `221610de 403f1458 000000a1`;
  internal handles `c2000005 abcd2080`; firmware string "535.183.01".
- `AN/20260530-110235-open-capture-rpc-decoded.txt:1-10` — opening order 72→73→1→65→76, sync `result=0x0`;
  send-counts **663** total / **553** fn=76 / **83** fn=103.
- `AN/20260530-110540-open-capture-decoded-full.txt:40,43,808-826` — `NV20_SUBDEVICE_0` + `NV04_DISPLAY_COMMON`
  both `hParent=0xcaf00000` (sibling); 4× cursor; channel counts 1×C67D / 8×C67E / 4×C67A.
- `AN/20260530-115116-open-capture-evo-full.txt:4674,4681,4711,4743,4750,4780` — head 3 displayId `0x800` @
  pclk 241.5 MHz; head 2 displayId `0x2000` @ 241.7 MHz; shared raster `0x05c90aa0`.

**Source tree (`SRC`)**
- `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c:2647,2704,2770,3457` — WPR2 fail-early, RVGBOOT 01, boot GSP-RM
  in WPR2, radix3.
- `src/nvidia/src/kernel/gpu/gsp/arch/ampere/kernel_gsp_ga102.c:164,190,204,207,253,270,277,281` — GA102
  bootstrap + RVGBOOT 04–10.
- `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_frts_tu102.c:25,38,43-55` — FWSEC-for-FRTS, WPR2
  register include, FRTS 1 MB.
- `src/nvidia/src/kernel/gpu/gsp/kernel_gsp_booter.c:154` — SEC2/Booter usage.
- `src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c:57,85,91-96,158` — command/status rings (msgq).
- `src/common/inc/swref/published/turing/tu102/dev_falcon_v4.h:42,71,73,75,87-88,92` — FALCON_OS, DMATRF
  offsets, SIZE_256B.
- `src/nvidia/kernel/inc/vgpu/rpc_global_enums.h:81,85,112` — fn 72/76/103.
- `src/nvidia/kernel/vgpu/nv/rpc.c:1677,1887` — GSP_RM_CONTROL/ALLOC `rpcWriteCommonHeader` emit (verified).
- `src/common/sdk/nvidia/inc/nvos.h:458-463,469-480,2163,2171-2180` — NVOS21/NVOS64 (RmAlloc), NV04_CONTROL=0x36,
  NVOS54 (RmControl).
- `src/common/sdk/nvidia/inc/class/cl0000.h:42`, `cl0080.h:36`, `cl2080.h:36` — NV01_ROOT/NV01_DEVICE_0/NV20_SUBDEVICE_0.
- `src/common/sdk/nvidia/inc/class/clc670.h:32,37-39` — NVC670_DISPLAY, numHeads/numSors/numDsis.
- `src/common/sdk/nvidia/inc/class/clc67d.h:34,82,301,567,693,828` — core channel, UPDATE, SOR_SET_CONTROL,
  HEAD_SET_CONTROL/PIXEL_CLOCK/RASTER_SIZE.
- `src/common/sdk/nvidia/inc/class/clc67e.h:34,55,128,131,139,177,181` — window channel, UPDATE, SET_SIZE/
  STORAGE/PARAMS/PLANAR_STORAGE/CONTEXT_DMA_ISO.
- `src/common/sdk/nvidia/inc/class/clc67a.h:32`, `clc67b.h:34` — cursor PIO, window-imm.
- `src/common/sdk/nvidia/inc/ctrl/ctrl0073/ctrl0073specific.h:145,1099-1105` — GET_EDID_V2, OR-type enum.
- `src/common/sdk/nvidia/inc/ctrl/ctrl0073/ctrl0073dp.h:41,151` — DP_AUXCH_CTRL.
- `src/nvidia-modeset/src/nvkms-evo3.c:24-27` — NVKMS/EVO HAL (nvdisplay 3.x).
- `src/nvidia-modeset/include/nvkms-dma.h:24,124,247,265` — pushbuffer routines, RVGEVOD/RVGEVO emit,
  method-offset field.

**Briefs / drafts (secondary, cross-ref only)**
- `DRAFTS/CONTEXT_BRIEF.md:7-48` (mission/hardware/method/BARs); `CONTEXT_ADDENDUM.md` (measured values);
  `CORRECTIONS.md:1-49` (#1 two-monitor, #2 RPC counts, #3 header_version, #4 DISPLAY_COMMON parent, #5 EVO
  decoder caveat, #6 EDID-derived 144 Hz, #7 internal handles, #8 boot DMA loop).
- `DRAFTS/D03,D06,D08,D10` and `S07,S09,S10` — internal handles, core-modeset/DP-link/pixel-clock specifics
  (SOR index, lanes/HBR) **not** re-verified first-party in F01.

**Reconciliation change log (what was corrected/removed vs draft S01):**
1. **Connector identity rewritten.** Draft "Monitor on connector id 88 … modes incl. 144 Hz" is **false** —
   connector 88 (DP-1) is **disconnected, 0 modes** (`CAP/modetest-list.txt:15`). Replaced with the verified
   two-monitor table + three-namespace reconciliation (CORRECTION #1).
2. **144 Hz citation un-conflated.** Draft cited the HDMI sink's `581640`/144.00 Hz (`:152-153`) as the
   target's 144 Hz. Target DP panel's 144-class mode is **143.91 Hz / 592 MHz** (`:74-75`).
3. **"Visible modeset via modetest" downgraded.** Both `modetest -s` runs failed (disconnected 88 via the
   `awk '/connected/'` bug); the EVO trace is the **load-time** modeset. "Visible SMPTE test pattern" → [TODO].
4. **Boot engine split upgraded** from brief-quoted to first-party EVIDENCE (1058+710=1768).
5. **`RVGEVO` emit line corrected** from `:251-252` to `nvkms-dma.h:265` (data `:124`, offset field `:247`).
6. **Added on-wire confirmations** (PCI ids, internal handles, header_version) and the
   `NV04_DISPLAY_COMMON`-parented-to-DEVICE fact (CORRECTION #4).
7. **144 Hz framing corrected (final QA pass).** Removed the claim that 144 Hz is "purely a timing/clock
   change / not extra architecture": per CORRECTION #1 + `DRAFTS/D08:30,38-39` it additionally requires a DP
   link **re-train 2→4 lanes @ HBR2** (same GSP-RM `DP_CTRL` path; 4×HBR2 already assessed to train). Added
   CORRECTION #10 (host requests pclk via `HERTZ`; GSP-RM programs the VPLL — no host PLL MMIO).
8. **On-wire id/handle citations tightened (final QA pass).** The device id and internal handles are read
   from the **fn=76** replies that carry them (`CAP2/rpc-resp-trace.txt:3-6`); the `GET_GSP_STATIC_INFO`
   (fn=65) origin is labeled [INFERENCE] because that reply's payload exceeds the 32-dword `RVGRESP` dump.

---

### Open questions / TODO

- **[TODO]** Fold in the DP panel's **SOR index (claimed SOR 1) + protocol (DP_B)** from S09/S10 — the EVO
  trace here did not decode `SOR_SET_CONTROL` owner/protocol, so F01 carries these as [INFERENCE]. (Note: the
  **lane/HBR training is already measured** in `DRAFTS/D08` from primary `DFP_GET_INFO`/`DP_GET_LINK_CONFIG`
  replies — 2-lane HBR2 @ 60 Hz, 4-lane @ 144 Hz; fold those primaries into F08/F09.)
- **[TODO]** Capture an **intentional visible modeset on the live connector (93 / displayId 0x800)** — the
  current harness targeted disconnected connector 88 (`capture_open.sh:102`); fix the `awk` connected-match and
  re-run to confirm a real on-screen test pattern (and to capture 144 Hz **on-wire** data words, currently
  EDID-derived only — CORRECTION #6).
- **[TODO]** Confirm **BAR3's** concrete role for this card (regs-alt / USERD / unused for first pixel) — S02.
- **[TODO]** Confirm the EVO pushbuffer **kickoff/doorbell** path (PUT pointer; BAR0 vs GSP-mediated) —
  `SRC/.../nvkms-push.c`, `nvkms-dma.h`. (Owner: S10.)
- **[TODO]** Confirm **PIOR/DAC/DSI are absent** in the decoded control stream (expected: SOR only) to justify
  skipping them.
- **[TODO]** Acronym expansions marked **[INFERENCE]** (GSP, FWSEC, FRTS, "Falcon", ISO, EVO) are
  industry-standard but not literally spelled in the cited files; grep `SRC/src/common/inc` for a spelled-out
  form before stating as fact.
- **[TODO]** `hInternalDevice≈0xabcd0080` is **[INFERENCE]** (only `hInternalClient=0xc2000005` /
  `hInternalSubdevice=0xabcd2080` were seen on-wire in `CAP2/rpc-resp-trace.txt`); confirm from a fn=65 static-info
  decode.


## F02 — OS-Side Prerequisites

**What StelluxOS must implement before any GPU/GSP code runs**, for the RTX 3080
(GA102, `10de:2216` @ `0000:0b:00.0`). This is the host-kernel primitive layer the
open driver (535.183.01) leans on *underneath* GSP bootstrap and the first-pixel
modeset. Every item carries an honest label — **[EVIDENCE]** (cited file:line),
**[INFERENCE]** (reasoned, says from what), **[TODO]** (unconfirmed) — and an
**ESSENTIAL / OPTIONAL** verdict for the first-pixel milestone.

Shorthand: `SRC=/home/flare/dev/gpu-repro/open-gpu-kernel-modules`,
`CAP=/home/flare/dev/gpu-repro/traces/20260530-112551-open-capture`.
This section judged & reconciled the S02 draft against source; deltas are flagged
inline as **[CORRECTED]**. Display/RPC-count corrections from `CORRECTIONS.md`
(2 monitors, ≈1145 vs 663 RPCs, displayId 0x800, etc.) live in the display/RPC
sections and are out of scope here; this section only owns OS primitives.

Identity reconfirmed **[EVIDENCE]**: `Bus Id 00000000:0B:00.0`,
`Device Id 0x221610DE`, `Sub System Id 0x403F1458`, `PCIe Gen4 x16`
(`$CAP/nvidia-smi-q.txt:56-71`); BAR1 = 256 MiB (`$CAP/nvidia-smi-q.txt:99-102`).

---

### 1. PCIe config space — device enable, bus-master, capability walk

`nv_pci_probe` performs canonical Linux bring-up in this order **[EVIDENCE]**:

- **Enable device** — `pci_enable_device(pci_dev)` (`$SRC/kernel-open/nvidia/nv-pci.c:436`);
  aborts on failure (`:436-441`). Flips PCI_COMMAND memory/IO decode on.
- **IRQ-existence policy gate** — aborts only if
  `pci_dev->irq == 0 && !pci_find_capability(PCI_CAP_ID_MSIX) && nv_treat_missing_irq_as_error()`
  (`$SRC/kernel-open/nvidia/nv-pci.c:443-451`). **[CORRECTED]** the draft omitted the
  third clause: this is a *tunable policy gate* (`nv_treat_missing_irq_as_error`), not a
  hardware requirement — reinforces that it is skippable (see §3).
- **Claim register BAR window** — `request_mem_region(start,size,name)` for the regs
  BAR (`$SRC/kernel-open/nvidia/nv-pci.c:551-562`).
- **Enable bus-master (DMA/BME)** — `pci_set_master(pci_dev)`
  (`$SRC/kernel-open/nvidia/nv-pci.c:653`). Without BME the GSP cannot DMA the sysmem
  RPC queues or boot images (§4/§5). Teardown: `pci_clear_master` (`:964`) +
  `udelay(1)` BME-settle (`:967-972`).
- **Config accessors used**: `PCI_COMMAND` word R/W (`:212-213`, `:267`), BAR/bridge
  prefetch dword reads (`:487-495`), and a manual capability-list walk in
  `nv_find_pci_capability` via `PCI_STATUS`/`PCI_CAPABILITY_LIST`/`PCI_CAP_LIST_ID`/
  `PCI_CAP_LIST_NEXT` (`$SRC/kernel-open/nvidia/nv-pci.c:1028-1056`). BAR enumeration
  records `{offset,cpu_address,size}` for memory BARs and sets `nv->regs=bars[REGS]`,
  `nv->fb=bars[FB]` (`:610-623`).

**StelluxOS minimum [INFERENCE from above]:** config-space read/write 8/16/32 keyed
by (domain,bus,slot,func); enumerate `10de:2216` @ `0000:0b:00.0`; in PCI_COMMAND set
**Memory Space Enable + Bus Master Enable**. I/O space is not needed (boot path is all
MMIO; the card's I/O port `0xf000` is unused for bring-up). Capability walk is needed
only if you opt into MSI/MSI-X (§3).

**Verdict: ESSENTIAL** — Memory + Bus-Master enable are non-negotiable. The
IRQ-existence check and capability walk are **SKIPPABLE** for a polled bring-up.

---

### 2. BAR mapping — BAR0 regs (uncached), BAR1 VRAM, BAR3 inst-mem

Three memory BARs **[EVIDENCE]** (`$SRC/src/nvidia/arch/nvalloc/unix/include/nv.h:201-204`):
`NV_GPU_NUM_BARS=3`, `…_REGS=0` (BAR0), `…_FB=1` (BAR1), `…_IMEM=2` (BAR3).

**BAR0 (16 MB MMIO regs @ 0xfb000000) — the control surface, mapped UNCACHED.**
**[CORRECTED — citation/protection]** The persistent register mapping (the one every
register access and all ~1768 boot writes use) is created by `nv_os_map_kernel_space`:

```177:182:src/nvidia/arch/nvalloc/unix/src/osinit.c
    aperture->map = osMapKernelSpace(aperture->cpu_address,
                                     aperture->size,
                                     NV_MEMORY_UNCACHED,
                                     NV_PROTECT_READ_WRITE);
    aperture->map_u = (nv_phwreg_t)aperture->map;
```

called over the full BAR0 `aperture->size` at `$SRC/.../osinit.c:1104-1108` (NULL-checked
→ fatal `RM_INIT_REG_SETUP_FAILED`). So the surface is **uncached, READ_WRITE, 16 MB**.
The draft's cite (`osapi.c:3301-3304`, mode `NV_PROTECT_READABLE`) is **only a temporary
one-page chip-id probe** — it maps one page, reads `NV_PMC_BOOT_0/1/42`, then
`osUnmapKernelSpace` (`$SRC/.../osapi.c:3301-3316`); an identical throwaway probe is at
`$SRC/.../osinit.c:1165-1180`. They corroborate the *uncached* mode but are **not** the
register surface. Mapping chain **[EVIDENCE]**: `osMapKernelSpace`
(`$SRC/.../os.c:285-316`) → `os_map_kernel_space` switch (`$SRC/kernel-open/nvidia/os-interface.c:924-972`,
UNCACHED→`nv_ioremap_nocache`@`:962-964`) → `ioremap()`
(`$SRC/kernel-open/common/inc/nv-linux.h:512-527`).

Accessors are **volatile 32-bit** loads/stores **[EVIDENCE]**
(`$SRC/src/nvidia/arch/nvalloc/unix/include/nv-priv.h:35,39`):

```35:39:src/nvidia/arch/nvalloc/unix/include/nv-priv.h
#define NV_PRIV_REG_WR32(b,o,d)   (*((volatile NvV32*)&(b)->Reg032[(o)/4])=(NvV32)(d))

#define NV_PRIV_REG_RD32(b,o)     ((b)->Reg032[(o)/4])
```

wrapped by `osDevWriteReg032`/`osDevReadReg032` (`$SRC/.../os.c:1641-1669`, `:1707-1730`;
the project's `RVGREG wr off=… val=…` trace hook is at `:1665`), both bounds-checking
`thisAddress < pMapping->gpuNvLength` (the 16 MB length). The register base is handed to
RM as `regBaseAddr = nv->regs->map` (`$SRC/.../osinit.c:603`). GPU-lost probe is
`readl(nv->regs->map)==0xFFFFFFFF` (`$SRC/kernel-open/nvidia/nv.c:4649`).

**BAR1 (256 MB VRAM aperture @ 0xd0000000, prefetchable, ReBAR off).** Reported to RM as
`fbPhysAddr=fb->cpu_address`, `fbBaseAddr=0 // not mapped` at attach **[EVIDENCE]**
(`$SRC/.../osinit.c:600-601`) — no permanent kernel mapping at attach. CPU access to VRAM
is on-demand and **write-combining** (UNCACHED fallback if WC disabled) via the FB-offset
mmap caching path **[EVIDENCE]** (`$SRC/kernel-open/nvidia/nv-mmap.c:559-564`; the regs
path there forces UNCACHED at `:541`). ReBAR resize logic exists but is gated/optional
(`$SRC/.../nv-pci.c:163-274`) and off on this board.

**BAR3 (32 MB @ 0xe0000000) — instance-memory aperture (IMEM).** Reported by physical
address: `instPhysAddr=bars[IMEM].cpu_address`, `instBaseAddr=0 // not mapped`
**[EVIDENCE]** (`$SRC/.../osinit.c:605-610`). No CPU ioremap at boot.

**[INFERENCE]** For a CPU-drawn square, StelluxOS maps a slice of BAR1 **write-combining**
and writes pixels into VRAM; BAR3 needs only its phys addr reported.

**Verdict:** BAR0 ioremap (uncached, RW, 16 MB) = **ESSENTIAL**. BAR1 on-demand WC map =
**ESSENTIAL for a CPU-drawn pixel** (map on demand, not at attach). BAR3 CPU mapping =
**OPTIONAL** (report phys addr only).

---

### 3. Interrupts — required, or can we poll? (poll wins for first pixel)

Linux-side setup prefers **MSI-X → MSI → INTx virtual-wire** **[EVIDENCE]**, gated by the
`NV_REG_ENABLE_MSI` registry knob: `nv_init_msix`/`nv_init_msi`
(`$SRC/kernel-open/nvidia/nv-msi.c:67-139`, `:28-65`; `pci_enable_msi` at `:33`), selected
at `$SRC/kernel-open/nvidia/nv.c:1231-1237`; if none exist and `interrupt_line==0` it errors
**"No interrupts of any type are available. Cannot use this GPU."** (`$SRC/.../nv.c:1248-1249`);
INTx fallback is `request_threaded_irq(nv->interrupt_line, nvidia_isr, nvidia_isr_kthread_bh, …)`
(`:1262-1264`). IRQ registration happens **before** `rm_init_adapter` (`:1314`).

**The GSP bring-up handshake itself is polled, not interrupt-driven [EVIDENCE]:**
- Sync RPC = send then **poll** for the reply: `_issueRpcAndWait` → `rpcRecvPoll`
  (`$SRC/src/nvidia/kernel/vgpu/nv/rpc.c:208`; stub decl `:144`; `RVGTRACE` send hook `:189`).
- `kgspWaitForRmInitDone` = `rpcRecvPoll(…, GSP_INIT_DONE)`
  (`$SRC/src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c:3775,3790`) — RVGBOOT stage 9→10
  (`$SRC/.../gsp/arch/ampere/kernel_gsp_ga102.c:277-281`).
- The poll loop is `gpuSetTimeout(...); for(;;){ gpuCheckTimeout; _kgspRpcDrainEvents; …
  osSpinLoop(); }`, timeout stretched to 1.5× default **[EVIDENCE]**
  (`$SRC/.../gsp/kernel_gsp.c:1834,1844-1893`).
- Command-queue producer also spins for free space under a 1 s timeout
  (`$SRC/.../gsp/message_queue_cpu.c:557-575`).
- The interrupt **status** is itself a BAR0 register readable by MMIO:
  `*(nv->regs->map + (NV_RM_DEVICE_INTR_ADDRESS >> 2))` (`$SRC/kernel-open/nvidia/nv.c:2126-2127`).
  **[CORRECTED — framing]** that specific read is the `NV_ESC_QUERY_DEVICE_INTR` ioctl
  handler, not literally the kernel ISR; but it proves the intr-status word lives in BAR0
  and is **pollable via MMIO** (the real ISR reads the same region).

**[INFERENCE]** For first pixel, StelluxOS can **skip MSI/MSI-X entirely and poll** the GSP
message queue (optionally the BAR0 intr-status word). The "must have an IRQ" checks
(`nv-pci.c:443-451`, `nv.c:1244-1252`) are driver policy, not a GSP requirement.

**Verdict: OPTIONAL for first pixel** — polling is the shorter correct path. MSI becomes
useful later for the 144 Hz vblank/flip loop (see §Open questions).

---

### 4. DMA — coherent sysmem, physical addresses, 47-bit mask, barriers

**DMA mask = 47-bit on this card [EVIDENCE — upgraded from draft TODO].** Probe starts at
32-bit (`pci_dev->dma_mask = 0xffffffffULL`, `$SRC/kernel-open/nvidia/nv-pci.c:582`). RM then
widens it to **47** through two sequential setters inside `RmInitAdapter`
(`$SRC/.../osinit.c:1526`). **First**, because this card takes the GSP firmware path
(`nv->request_firmware`), an early pre-set runs so the GSP-image fetch that immediately
follows it (`RmFetchGspRmImages`, `osinit.c:1592`) can DMA with a wide mask:

```1588:1590:src/nvidia/arch/nvalloc/unix/src/osinit.c
    if (nv->request_firmware)
    {
        nv_set_dma_address_size(nv, NV_GSP_GPU_MIN_SUPPORTED_DMA_ADDR_WIDTH);
```

with `#define NV_GSP_GPU_MIN_SUPPORTED_DMA_ADDR_WIDTH 47` (`$SRC/.../osinit.c:158`).
**Second and last**, `RmInitAdapter` calls `osInitNvMapping` (`osinit.c:1601`), which runs the
precise HAL setter `nv_set_dma_address_size(nv, gpuGetPhysAddrWidth_HAL(pGpu, ADDR_SYSMEM))`
(`$SRC/.../osinit.c:653`) — this is the **final, persistent** mask.
**[CORRECTED — ordering]** the HAL setter at `:653` runs *after* the firmware pre-set at
`:1590` (it is reached via the `osInitNvMapping` call at `:1601`, which is below `:1590` in
`RmInitAdapter`), so the HAL value — not the firmware constant — is authoritative. For GA102
it independently resolves to **47**: the NVOC dispatch routes every non-Hopper chip to
`gpuGetPhysAddrWidth_TU102` (`$SRC/src/nvidia/generated/g_gpu_nvoc.c:601-609`), which returns
`NV_CHIP_EXTENDED_SYSTEM_PHYSICAL_ADDRESS_BITS`
(`$SRC/src/nvidia/src/kernel/gpu/arch/turing/kern_gpu_tu102.c:102-111`) `= 47`
(`$SRC/src/common/inc/swref/published/turing/tu102/hwproject.h:27`). Both setters thus agree
on **47**, matching CONTEXT_BRIEF §3. The setter computes `mask=(1<<bits)-1`, sets
`addressable_range.limit = start+mask`, and calls `dma_set_mask` + `dma_set_coherent_mask`
**[EVIDENCE]** (`$SRC/kernel-open/nvidia/nv.c:2764,2766,2776,2782`; OS shim
`osDmaSetAddressSize`→`nv_set_dma_address_size` at `$SRC/.../os.c:894-900`).

**Coherent/consistent sysmem [EVIDENCE].** `nv_alloc_coherent_pages` →
`dma_alloc_coherent(dev, num_pages*PAGE_SIZE, &bus_addr, gfp_mask)`, recording
`virt_addr`, `phys_addr=virt_to_phys(...)`, `dma_addr=bus_addr`, flag `coherent`
(`$SRC/kernel-open/nvidia/nv-vm.c:288-342`; free `:344-362`). Streaming maps go through
`nv_dma_map_pages` (`$SRC/kernel-open/nvidia/nv-dma.c:512`; contig fast-path
`nv_dma_map_contig→dma_map_page` `:58-94`), each validated against the 47-bit limit by
`nv_dma_is_addressable` (`$SRC/kernel-open/nvidia/nv-dma.c:45-56`).

**Physical/DMA address poked through BAR0 [EVIDENCE].** The GPU learns where sysmem buffers
live via phys/DMA addresses written to BAR0 Falcon mailboxes: libos init-args addr →
`GPU_REG_WR32(NV_PGSP_FALCON_MAILBOX0/1, lo/hi)`
(`$SRC/.../gsp/arch/turing/kernel_gsp_tu102.c:335-339`); WPR-meta addr → Booter-Load via
`memdescGetPhysAddr(pWprMetaDescriptor, AT_GPU, 0)`
(`$SRC/.../gsp/arch/ampere/kernel_gsp_ga102.c:245`). So StelluxOS must obtain a buffer's
**device DMA address** and write it into BAR0 registers.

**Cache coherency / memory barriers [EVIDENCE].** RPC queue memory is `ADDR_SYSMEM` +
`NV_MEMORY_CACHED` (`$SRC/.../gsp/message_queue_cpu.c:252-254`); x86 DMA is cache-coherent,
so `nv_dma_cache_invalidate` is a **no-op except on AArch64**
(`$SRC/kernel-open/nvidia/nv-dma.c:945-975`). What is mandatory is **store ordering before
submit** — a store fence keeps the queue-pointer update from passing the payload write:

```604:606:src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c
    portAtomicMemoryFenceStore();

    nRet = msgqTxSubmitBuffers(pMQI->hQueue, pCQE->elemCount);
```

with the explicit WAW producer/consumer comment at `:596-602` (fence sits between the
payload `portMemCopy` at `:591` and submit at `:606`). Full fences also at
`message_queue_cpu.c:381,572` and after writing libos args
(`$SRC/.../gsp/kernel_gsp.c:3743`). Plus `osFlushCpuWriteCombineBuffer()`
(`$SRC/.../os.c:1539-1542`) to drain WC writes (e.g. to BAR1/pushbuffers).

**Verdict: ESSENTIAL.** Need (a) a `dma_alloc_coherent`-equivalent returning
`{cpu_va, dma_addr}` for cached sysmem, (b) a **47-bit-capable** DMA address space,
(c) a **store fence + full fence + WC-flush** primitive set, (d) a phys/DMA-addr query
whose result is what gets written to BAR0 mailboxes/PDEs.

---

### 5. Large/contiguous allocations, radix3, IOMMU / identity-map

Boot artifacts are `memdesc`s in `ADDR_SYSMEM`. Contiguity/cache differ per buffer
**[EVIDENCE]** (verified each cite; **[CORRECTED]** one row):

| Buffer | Contiguous? | Cache | Cite (`$SRC/.../gpu/gsp/…`) |
|---|---|---|---|
| WPR meta (0x1000) | contig (`NV_TRUE`) | CACHED | `arch/turing/kernel_gsp_tu102.c:123-126` |
| libos init args | contig | **UNCACHED** | `arch/turing/kernel_gsp_tu102.c:149-154` |
| GSP arguments (0x1000) | contig | CACHED | `arch/turing/kernel_gsp_tu102.c:177-180` |
| GSP-RM signature | contig | CACHED | `kernel_gsp.c:3272-3274` |
| GSP boot ucode | contig | CACHED | `kernel_gsp.c:3175-3179` |
| FWSEC ucode code/data | contig (`MEMDESC_FLAGS_PHYSICALLY_CONTIGUOUS`) | UNCACHED | `kernel_gsp_fwsec.c:786-792` |
| Booter ucode | contig | UNCACHED | `kernel_gsp_booter.c:340-342` |
| **radix3 GSP-RM image table** | **NON-contig** | CACHED | `kernel_gsp.c:3479-3565` |
| **RPC message queues** | **NON-contig** | CACHED | `message_queue_cpu.c:252-254` |

**[CORRECTED]** the draft listed **libos init args** as CACHED; source is **UNCACHED**
(`NV_MEMORY_UNCACHED`, `kernel_gsp_tu102.c:153`). All other rows verified exact.

**No giant contiguous allocation is required [EVIDENCE].** The big ~38 MB
`gsp_ga10x.bin` image and the RPC queues are deliberately **non-contiguous**; a **radix3
(3-level) page table** is built so the GSP MMU can gather scattered sysmem pages:

```3529:3534:src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c
        memdescCreate(ppMemdescRadix3, pGpu, allocSize,
            LIBOS_MEMORY_REGION_RADIX_PAGE_SIZE,
            NV_MEMORY_NONCONTIGUOUS,
            ADDR_SYSMEM,
            NV_MEMORY_CACHED,
            flags),
```

The builder uses a `radix3[4]` working array (1 root PDE + intermediate PDEs + PTEs) and
fills PDEs from `memdescGetPhysAddrs(..., AT_GPU, ...)` (`$SRC/.../kernel_gsp.c:3479-3565`).
Therefore StelluxOS needs only **small physically-contiguous buffers** (≤ a few pages each:
WPR meta, libos/gsp args, ucode blobs) plus a **scatter-able page pool + a radix3 builder**
for the 38 MB image and the RPC queues.

**IOMMU / identity-map [EVIDENCE + INFERENCE].** IOMMU is on (group 27, CONTEXT_BRIEF §3);
the driver works because it uses the streaming/coherent DMA API, programming IOVAs that the
47-bit mask covers (validated `nv-dma.c:45-56`). On x86 the DMA start offset is 0 —
`nv_get_dma_start_address` returns 0 on non-PPC (`$SRC/kernel-open/nvidia/nv.c:4606-4608`),
so `addressable_range.start=0` and with no IOMMU translation **dma_addr == phys_addr**. A
DMA32 fallback (`NV_GFP_DMA32`) exists if the mask were narrower than sysmem top
(`$SRC/kernel-open/nvidia/nv-vm.c:249-256`) — **not** triggered at 47-bit.
**[INFERENCE]** StelluxOS may either (a) leave the IOMMU off / use an identity map and
program the GPU with physical addrs directly, or (b) program the IOMMU and feed IOVAs —
both work provided the address fits 47 bits and the value written to BAR0 mailboxes/PDEs is
the **DMA** address the GPU will issue.

**Verdict:** small-contig buffers + scatter pool + radix3 builder = **ESSENTIAL**;
giant-contig alloc = **NOT needed**; IOMMU either-way (**identity map is simplest** for
first pixel).

---

### 6. Timers / delays — the boot polls with timeouts

Primitives RM expects from the OS **[EVIDENCE]** (`$SRC/.../os.c`):
- `osDelay(ms)`/`osDelayUs(us)`/`osDelayNs(ns)` → `os_delay`/`os_delay_us` (`:171-185`;
  `osDelayNs` clamps to ≥1 µs via `NV_MAX(1, ns/1000)` at `:183`).
- `osGetCurrentTick` (ns), `osGetPerformanceCounter` (hi-res), `osGetTimestamp`
  (`:111-156`) — the timeout machinery's backing clock.

Timeout-bounded polling drives the whole boot **[EVIDENCE]**:
- GSP RPC wait: `gpuSetTimeout` + `gpuCheckTimeout` + `osSpinLoop` loop, 1.5× default
  (`$SRC/.../gsp/kernel_gsp.c:1825-1893`).
- TX-queue space: 1 s timeout, ~10 ms retry (`$SRC/.../gsp/message_queue_cpu.c:557-575`).
- **GFW_BOOT** (RVGBOOT stage 3 = VBIOS devinit done) register-progress poll:
  `kgspWaitForGfwBootOk_TU102 → gpuWaitForGfwBootComplete_HAL`
  (`$SRC/.../gsp/arch/turing/kernel_gsp_tu102.c:991-999`), testing field
  `_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT _PROGRESS _COMPLETED`
  (`$SRC/.../gsp/arch/turing/kernel_gsp_frts_tu102.c:497-503`).
- Small fixed delays too, e.g. `udelay(1)` after BME disable (`$SRC/.../nv-pci.c:967-972`).

Total boot ≈ 2.07 s with seconds-scale stage waits (CONTEXT_BRIEF §4a), so timeouts must
tolerate multi-second waits.

**Verdict: ESSENTIAL** — need a **monotonic nanosecond clock + busy/sleep delay**. Wrong
timeouts make the GSP poll loop either spin forever or abort early (the `RmInitAdapter
failed` failure class noted for mmiotrace in CONTEXT_BRIEF §2).

---

### Minimal-path summary (essential vs skippable for first pixel)

**ESSENTIAL**
1. Config space: enable **Memory Space + Bus Master** on `0000:0b:00.0` (§1).
2. **ioremap BAR0** — 16 MB, **uncached, READ_WRITE** — + volatile `RD32/WR32`; all boot
   writes and every control register go here (§2).
3. **dma_alloc_coherent-equivalent** (cached sysmem `{cpu_va, dma_addr}`), **47-bit** DMA
   address space, **store fence + full fence + WC flush**, phys/DMA-addr query (§4).
4. Small **physically-contiguous** buffers (WPR meta, libos/gsp args, ucode blobs) + a
   **scatter page pool + radix3 builder** for the 38 MB image & RPC queues (§5).
5. **Monotonic ns clock + delay** for timeout-bounded GSP polling (~2 s boot) (§6).
6. On-demand **BAR1 write-combining** map to write the pixel/square into VRAM (§2).

**SKIPPABLE / OPTIONAL for first pixel**
- **MSI/MSI-X and any IRQ handler** — poll the GSP message queue (and BAR0 intr-status
  word) instead; the "must have an IRQ" checks are policy, not GSP need (§3).
- **Permanent BAR1/BAR3 kernel mappings** (driver leaves them unmapped at attach) (§2).
- **ReBAR / BAR resize** (off on this board), capability walk (unless using MSI),
  NUMA/coherent-NVLink, ATS, P2P, dma-buf import, IBM-NPU/PPC, Xen/SR-IOV.
- A **giant contiguous allocation** — radix3 makes it unnecessary (§5).
- I/O-port (`0xf000`) access — boot is all MMIO (§1).

---

### Evidence cited

PCIe / config (`$SRC/kernel-open/nvidia/nv-pci.c`):
- `:212-213,267` — PCI_COMMAND word R/W (ReBAR path)
- `:436` — `pci_enable_device`; `:436-441` abort
- `:443-451` — IRQ-existence policy gate (incl. `nv_treat_missing_irq_as_error`)
- `:487-495` — BAR/bridge prefetch dword reads
- `:551-562` — `request_mem_region` (regs BAR)
- `:582` — initial 32-bit `dma_mask`
- `:610-623` — BAR enumeration; `regs`/`fb` assignment
- `:653` — `pci_set_master` (BME)
- `:964,967-972` — `pci_clear_master` + `udelay(1)` settle
- `:1028-1056` — `nv_find_pci_capability` (PCI_STATUS/CAP_LIST/CAP_LIST_ID/NEXT)
- `:163-274` — ReBAR resize (gated/optional)

BAR mapping:
- `$SRC/src/nvidia/arch/nvalloc/unix/include/nv.h:201-204` — BAR index defs
- `$SRC/.../include/nv-priv.h:35,39` — `NV_PRIV_REG_WR32/RD32`
- `$SRC/.../src/osinit.c:170-182` — `nv_os_map_kernel_space` → BAR0 uncached RW map **[persistent]**
- `$SRC/.../src/osinit.c:1104-1108` — persistent regs map call + NULL-check
- `$SRC/.../src/osinit.c:600-612` — attach: fb/dev/inst phys+len, `regBaseAddr=regs->map`
- `$SRC/.../src/os.c:285-316` — `osMapKernelSpace`
- `$SRC/.../src/os.c:1641-1669` — `osDevWriteReg032` + `RVGREG` hook (`:1665`)
- `$SRC/.../src/os.c:1707-1730` — `osDevReadReg032`
- `$SRC/.../src/osapi.c:3301-3316` — temp 1-page chip-id probe (UNCACHED, READABLE); PMC reads `:3312-3314`
- `$SRC/.../src/osinit.c:1165-1180` — second temp chip-id probe
- `$SRC/kernel-open/nvidia/os-interface.c:924-972` — `os_map_kernel_space` mode switch
- `$SRC/kernel-open/common/inc/nv-linux.h:512-570` — `nv_ioremap`/`_nocache`/`_cache`/`_wc`
- `$SRC/kernel-open/nvidia/nv-mmap.c:541,559-564` — regs UC / FB write-combining mmap
- `$SRC/kernel-open/nvidia/nv.c:4649` — `readl(regs->map)` GPU-lost probe
- `$CAP/nvidia-smi-q.txt:56-71` — Bus/Device/SubSystem id, Gen4 x16; `:99-102` — BAR1 256 MiB

Interrupts / polled path:
- `$SRC/kernel-open/nvidia/nv-msi.c:28-65` (`:33` `pci_enable_msi`), `:67-139` (msix; `:113` enable), `:141-166` (msix `request_threaded_irq`)
- `$SRC/kernel-open/nvidia/nv.c:1231-1237` — MSI-X→MSI selection; `:1248-1249` — "No interrupts…" fatal; `:1262-1264` — INTx fallback; `:1314` — `rm_init_adapter`
- `$SRC/kernel-open/nvidia/nv.c:2126-2127` — BAR0 intr-status read (QUERY_DEVICE_INTR ioctl)
- `$SRC/src/nvidia/kernel/vgpu/nv/rpc.c:144,189,208` — `rpcRecvPoll` decl / `RVGTRACE` / call in `_issueRpcAndWait`
- `$SRC/.../gpu/gsp/kernel_gsp.c:1834,1844-1893` — polled RPC wait loop (1.5× timeout)
- `$SRC/.../gpu/gsp/kernel_gsp.c:3775,3790` — `kgspWaitForRmInitDone` → poll `GSP_INIT_DONE`
- `$SRC/.../gpu/gsp/arch/ampere/kernel_gsp_ga102.c:277-281` — RVGBOOT 09/10

DMA / fences:
- `$SRC/.../src/osinit.c:158` — `NV_GSP_GPU_MIN_SUPPORTED_DMA_ADDR_WIDTH 47`; `:1588-1590` firmware DMA pre-set (runs **first**, before `RmFetchGspRmImages` `:1592`); `:1601` `osInitNvMapping` call; `:653` HAL path (**final/persistent** mask, runs **last**)
- `$SRC/src/nvidia/generated/g_gpu_nvoc.c:601-609` — NVOC HAL dispatch: non-GH100 (incl. GA102) → `gpuGetPhysAddrWidth_TU102`
- `$SRC/src/nvidia/src/kernel/gpu/arch/turing/kern_gpu_tu102.c:102-111` — `gpuGetPhysAddrWidth_TU102` returns `NV_CHIP_EXTENDED_SYSTEM_PHYSICAL_ADDRESS_BITS`
- `$SRC/src/common/inc/swref/published/turing/tu102/hwproject.h:27` — `NV_CHIP_EXTENDED_SYSTEM_PHYSICAL_ADDRESS_BITS 47` (GH100=52, n/a here)
- `$SRC/kernel-open/nvidia/nv.c:2764,2766,2776,2782` — `nv_set_dma_address_size` (mask/limit/dma_set_mask/coherent)
- `$SRC/.../src/os.c:894-900` — `osDmaSetAddressSize` → `nv_set_dma_address_size`
- `$SRC/kernel-open/nvidia/nv-vm.c:288-362` — `nv_alloc_coherent_pages` (`dma_alloc_coherent`)
- `$SRC/kernel-open/nvidia/nv-dma.c:45-56` — `nv_dma_is_addressable` (47-bit limit)
- `$SRC/kernel-open/nvidia/nv-dma.c:58-94,512` — `dma_map_page` / `nv_dma_map_pages`
- `$SRC/kernel-open/nvidia/nv-dma.c:945-975` — cache-invalidate AArch64-only (x86 no-op)
- `$SRC/.../gpu/gsp/arch/turing/kernel_gsp_tu102.c:335-339` — libos args phys → BAR0 MAILBOX0/1
- `$SRC/.../gpu/gsp/arch/ampere/kernel_gsp_ga102.c:245` — WPR-meta phys → Booter Load
- `$SRC/.../gpu/gsp/message_queue_cpu.c:252-254` — RPC queues SYSMEM/noncontig/CACHED
- `$SRC/.../gpu/gsp/message_queue_cpu.c:596-606` — `portAtomicMemoryFenceStore` (WAW) before submit
- `$SRC/.../gpu/gsp/message_queue_cpu.c:381,572` — full memory fences
- `$SRC/.../gpu/gsp/kernel_gsp.c:3743` — full fence after libos args
- `$SRC/.../src/os.c:1539-1542` — `osFlushCpuWriteCombineBuffer`

Contiguity / radix3 / IOMMU:
- `$SRC/.../gpu/gsp/kernel_gsp.c:3479-3565` — radix3 builder (`NV_MEMORY_NONCONTIGUOUS`, `radix3[4]`)
- `$SRC/.../gpu/gsp/kernel_gsp.c:3175-3179,3272-3274` — boot ucode / signature contig CACHED
- `$SRC/.../gpu/gsp/arch/turing/kernel_gsp_tu102.c:123-126,149-154,177-180` — WPR meta CACHED / libos args **UNCACHED** / GSP args CACHED
- `$SRC/.../gpu/gsp/kernel_gsp_fwsec.c:786-792` — FWSEC code/data contig UNCACHED
- `$SRC/.../gpu/gsp/kernel_gsp_booter.c:340-342` — Booter ucode contig UNCACHED
- `$SRC/kernel-open/nvidia/nv.c:4606-4608` — `nv_get_dma_start_address` returns 0 (x86)
- `$SRC/kernel-open/nvidia/nv-vm.c:249-256` — DMA32 fallback gate (untriggered at 47-bit)

Timers / delays:
- `$SRC/.../src/os.c:111-156` — `osGetCurrentTick`/`osGetPerformanceCounter`/`osGetTimestamp`
- `$SRC/.../src/os.c:171-185` — `osDelay`/`osDelayUs`/`osDelayNs` (≥1 µs clamp `:183`)
- `$SRC/.../gpu/gsp/kernel_gsp.c:1825-1893` — polled RPC timeout loop
- `$SRC/.../gpu/gsp/message_queue_cpu.c:557-575` — TX-space poll (1 s / ~10 ms)
- `$SRC/.../gpu/gsp/arch/turing/kernel_gsp_tu102.c:991-999` — `kgspWaitForGfwBootOk`
- `$SRC/.../gpu/gsp/arch/turing/kernel_gsp_frts_tu102.c:497-503` — GFW_BOOT progress field

---

### Open questions / TODO

- **[RESOLVED]** Numeric value of `gpuGetPhysAddrWidth_HAL(ADDR_SYSMEM)` for GA102 at
  `osinit.c:653` = **47**. GA102 (non-Hopper) dispatches to `gpuGetPhysAddrWidth_TU102`
  (`g_gpu_nvoc.c:601-609`) → `NV_CHIP_EXTENDED_SYSTEM_PHYSICAL_ADDRESS_BITS`
  (`kern_gpu_tu102.c:102-111`) = `47` (`turing/tu102/hwproject.h:27`). This HAL setter at
  `:653` (reached via `osInitNvMapping` at `:1601`) is the **final** writer and runs *after*
  the firmware pre-set at `:1590`; both equal 47, so the final mask matches CONTEXT_BRIEF §3.
  (Earlier drafts had the ordering reversed — corrected in §4.)
- **[TODO]** Does the later **144 Hz vblank/flip** loop need a real interrupt (vblank) or
  can it be polled? Inspect `_kgspRpcDrainEvents` event sources (`kernel_gsp.c`) and the
  display-channel completion path (cross-ref the modeset section). First pixel does not
  need it; sustained 144 Hz might.
- **[TODO]** Confirm BAR3/IMEM runtime use by GSP — it is passed as `instPhysAddr` with
  `instBaseAddr=0` (`osinit.c:605-610`); audit `gpuAttachArg->instPhysAddr` consumers to
  see whether first pixel ever touches BAR3 (expected: no CPU mapping needed).
- **[TODO]** Verify the no-IOMMU / identity-map path end-to-end: confirm the GPU is always
  fed the **DMA** address (not raw phys) by auditing `memdescGetPhysAddr(..., AT_GPU, ...)`
  vs `AT_CPU` usage in boot ucode / mailbox / PDE programming. On x86 with IOMMU-off and
  `dma_start=0` they coincide, but the StelluxOS abstraction must keep "what the GPU
  issues" == "what is written to BAR0".
- **[TODO — capture-tooling gap, do not fabricate]** Exact runtime WPR `frtsOffset`/heap
  values are not in the write-only trace (per `CORRECTIONS.md`); not required for the OS
  primitive layer, but note when the boot section needs them.


## F03 — Firmware Blobs & GPU Memory Layout

Final, source-verified inventory of every firmware binary and every FB/sysmem
layout fact an implementer must build to bring **GA102 (RTX 3080)** GSP from cold
to "RISC-V active + INIT_DONE". Each non-trivial claim is labeled **[EVIDENCE]**
(a cited primary source), **[INFERENCE]** (reasoned from cited evidence), or
**[TODO]** (could not confirm from the files). Every cited line below was
re-opened and checked against the tree during this pass.

Path shorthands:
`SRC = /home/flare/dev/gpu-repro/open-gpu-kernel-modules`,
`CAP = /home/flare/dev/gpu-repro/traces/20260530-115116-open-capture`.
All `path:line` refer to `SRC/...` unless prefixed with `CAP/`.

**Scope / cross-refs.** This section owns the *binaries* and the *FB/sysmem
memory map*. It deliberately stops at INIT_DONE (boot stage 10).
- **D01** owns the exhaustive, byte-level GSP+SEC2 boot **register** reference
  (the 41 distinct offsets, the DMA inner loop, BROM/PKC quartet, BCR/STARTCPU).
  F03 cross-refs D01 rather than re-deriving register semantics.
- **S02** owns VBIOS devinit / GFW_BOOT and Falcon/SEC2 register semantics.
- **S04** owns the RPC control plane that begins right after stage 06.

---

### F03.1 Boot-order spine — where the blobs and layout are consumed

`kgspInitRm_IMPL` (`src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c:2682`) drives the
whole sequence; the GA102 HAL `kgspBootstrapRiscvOSEarly_GA102`
(`.../arch/ampere/kernel_gsp_ga102.c:164`) does the actual bring-up. The
instrumented `RVGBOOT` markers in `CAP/boot-trace.txt` map 1:1 onto the code:

| Stage | Code site | Concern of this section |
|------|-----------|--------------------------|
| 01 | `kernel_gsp.c:2704` | `kgspInitRm` start |
| 02 | `kernel_gsp.c:2751` | VBIOS extracted, **FWSEC parsed** (ver `94.02.71.40.C4`) |
| 03 | `kernel_gsp.c:2833` | GFW_BOOT ok (VBIOS devinit done — S02) |
| 04 | `kernel_gsp_ga102.c:190` | bootstrap start; reg-write trace ON |
| 05 | `kernel_gsp_ga102.c:204` | **FWSEC-FRTS done → WPR2 carved** |
| 06 | `kernel_gsp_ga102.c:207` | reset into RISC-V |
| 07 | `kernel_gsp_ga102.c:253` | **Booter Load done → GSP-RM authenticated into WPR2** |
| 08–10 | `kernel_gsp_ga102.c:270,277,281` | RISC-V active → wait → INIT_DONE |

[EVIDENCE] Verified that all 10 RVGBOOT stage strings live at exactly those lines
(`kernel_gsp.c:2704,2751,2833`; `kernel_gsp_ga102.c:190,204,207,253,270,277,281`).
The split is: blobs are prepared in **sysmem** during stages 01–04 (inside
`kgspInitRm`, `kernel_gsp.c:2719–2822`); FWSEC carves **WPR2 in FB** at stage 05;
Booter (on SEC2) DMAs the prepared images from sysmem into WPR2 and authenticates
GSP-RM at stage 07. The FB layout itself is computed in `_kgspBootGspRm`
(`kernel_gsp.c:2643`) → `kgspCalculateFbLayout_HAL` (`:2656`) **before** any of
the three Falcon loads, because it needs the post-GFW FB size.

#### The 4-blob / 3-invocation framing [EVIDENCE — applies CORRECTION #8; see D01]

First boot consumes **four distinct firmware binaries**, but the host only drives
the Falcon DMA controller **three times** — the same "DMA-load-and-go" routine
re-pointed at a different base/ucode/aperture each time (full register proof in
**D01.3/D01.4**):

| # | Host invocation | Falcon base | Source aperture | Loads which blob |
|---|-----------------|-------------|-----------------|------------------|
| 1 | FWSEC-FRTS | GSP `0x110000` | sysmem | **FWSEC** (carves WPR2) |
| 2 | Booter Load | SEC2 `0x840000` | sysmem | **Booter Load** ucode |
| 3 | GSP-RM bootloader (CPU-sequencer replay) | GSP `0x110000` | **LOCAL_FB** | **GSP-RM boot bin** (from FB) |

The **4th blob — the GSP-RM ELF (`gsp_ga10x.bin` `.fwimage`)** — is never loaded
by a host invocation: it is DMA'd from sysmem (as a radix3 page table) into WPR2
*by the Booter ucode itself* (on SEC2) during invocation 2, then authenticated.
Invocation 3 reloads the boot binary (SK+BL) that was placed at `bootBinOffset`
in FB and issues CORE_RESUME to start GSP-RM proper.
[EVIDENCE] D01.0 counts **1689 of 1768** boot MMIO writes as the 256-byte DMA
inner loop and only **~79** "control" writes across **~22** distinct offsets —
that ~22-offset set is the entire register surface an implementer must get right;
`DMATRFCMD = 0x614` (IMEM, secure) / `0x600` (DMEM). See F03.8 and D01.

---

### F03.2 The four firmware blobs (what, where from, constraints)

#### 2.1 `gsp_ga10x.bin` — the GSP-RM firmware image — **ESSENTIAL**

- **On-disk path** `/lib/firmware/` + `nvidia/<NV_VERSION_STRING>/gsp_ga10x.bin`,
  i.e. `nvidia/535.183.01/gsp_ga10x.bin`. [EVIDENCE] The path macro is
  `NV_FIRMWARE_PATH_FOR_FILENAME(f) = "nvidia/" NV_VERSION_STRING "/" f`
  (`kernel-open/nvidia/nv.c:27`); chip-family→filename selection picks
  `gsp_ga10x.bin` for GA10X/AD10X/GH100 (`.../inc/nv-firmware.h:86-87`), with the
  GA10X family string `"ga10x"` (`nv-firmware.h:57`). Matches CONTEXT_BRIEF §3
  (38 MB file present on the target).
- **It is an ELF64 container, not a raw image.** [EVIDENCE] Sections are looked up
  by name in `_kgspFwContainerGetSection` (`kernel_gsp.c:3611`); ELF magic
  `0x464C457F`, class64 (`0x2`), little-endian (`0x1`) are checked at
  `kernel_gsp.c:3630-3632,3649-3653`. Section names (`g_kernel_gsp_nvoc.h`):
  - `.fwversion`  = `GSP_VERSION_SECTION_NAME` (`:208`)
  - `.fwimage`    = `GSP_IMAGE_SECTION_NAME` (`:209`) → the actual GSP-RM image
  - `.fwsignature_ga10x` = `GSP_SIGNATURE_SECTION_NAME_PREFIX` `".fwsignature_"`
    (`:211`) + the chip-family string
  - `.fwlogging`  = `GSP_LOGGING_SECTION_NAME` (`:210`), optional log-decode ELF
- **Version constraint (hard gate).** [EVIDENCE] The text in `.fwversion` must
  match the driver's `NV_VERSION_STRING` byte-for-byte (length+1 and
  `portStringCompare`), else `NV_ERR_INVALID_DATA`
  (`_kgspFwContainerVerifyVersion`, `kernel_gsp.c:3307`, check at `:3335-3336`,
  fail at `:3351`). The firmware file and kernel driver are a **matched pair**
  (here both 535.183.01).
- **Signature constraint.** [EVIDENCE] `.fwsignature_ga10x` is copied to a
  **256-byte-aligned** sysmem buffer ("alignment needed for Booter DMA",
  `_kgspCreateSignatureMemdesc`, `kernel_gsp.c:3257`, note at `:3270`,
  `NV_ALIGN_UP(...,256)` at `:3273`); its PA/size are handed to Booter via
  `GspFwWprMeta.sysmemAddrOfSignature/sizeOfSignature`. Booter (on SEC2) verifies
  GSP-RM against it before unlocking it in WPR2.

#### 2.2 Booter Load (SEC2 HS ucode) — **ESSENTIAL** (Booter Unload optional)

- **Source: compiled-in `bindata` archives, not files.** [EVIDENCE]
  `kgspAllocateBooterLoadUcodeImage_IMPL` (`kernel_gsp_booter.c:435`) /
  `...UnloadUcodeImage_IMPL` (`:453`), called from `kgspInitRm` at
  `kernel_gsp.c:2787` / `:2798`. Both go through `s_allocateUcodeFromBinArchive`
  (`kernel_gsp_booter.c:173`).
- **HS / boot-from-HS, PKC signature selected by fuse version.** [EVIDENCE] The
  ucode is built as `KGSP_FLCN_UCODE_BOOT_FROM_HS` because the GSP Falcon's
  `bBootFromHs` is true (branch at `kernel_gsp_booter.c:317`; GA102 sets
  `bBootFromHs = NV_TRUE` at `kernel_gsp_ga102.c:61`). The correct signature is
  patched in by `s_patchBooterUcodeSignature`, which selects
  `sigIndex = numSigs - 1 - fuseVer` (`kernel_gsp_booter.c:132`, `:165`). prod vs
  dbg archive keys (`image_prod/header_prod/sig_prod`) chosen by
  `kgspIsDebugModeEnabled` (`:216-227`).
- **Runs on SEC2, handoff via SEC2 mailboxes.** [EVIDENCE]
  `kgspExecuteBooterLoad_TU102` (`kernel_gsp_booter_tu102.c:86`) targets
  `GPU_GET_KERNEL_SEC2` (`:96`), resets SEC2 (`:113`), and passes the **WprMeta
  sysmem PA** as `mailbox0 = NvU64_LO32(addr)`, `mailbox1 = NvU64_HI32(addr)`
  (`:106-107`) where `addr = PA(pWprMetaDescriptor)` (caller
  `kernel_gsp_ga102.c:244-245`). A **non-zero MAILBOX0 on return = failure**
  (`s_executeBooterUcode_TU102`, `kernel_gsp_booter_tu102.c:76-80`).
- **Booter Unload** tears WPR2 down at unload/suspend
  (`kgspExecuteBooterUnloadIfNeeded_TU102`, `kernel_gsp_booter_tu102.c:129`);
  **not needed to reach a pixel.**
- [EVIDENCE] On GA102 baremetal `bPartitionedFmc` is false, so separate Booter
  ucodes ARE required — the `if (bPartitionedFmc) {} else {…}` at
  `kernel_gsp.c:2776-2806`. The partitioned-FMC alternative
  `GSP_FMC_BOOT_PARAMS` (`.../inc/gsp/gspifpub.h:89-94`) is the GH100 path, not
  GA102 — see TODO.

#### 2.3 FWSEC ucode (extracted from the VBIOS in ROM) — **ESSENTIAL**

- Neither a file nor bindata: parsed out of the VBIOS image read from the GPU's
  PROM (see F03.3). [EVIDENCE] Produces `pKernelGsp->pFwsecUcode`
  (`kernel_gsp.c:2734`). Two commands are issued to it: **FRTS** (carves WPR2,
  boot path) and **SB** (restore PreOsApps, unload path) — `kgspExecuteFwsecFrts`
  / `kgspExecuteFwsecSb` (`kernel_gsp_frts_tu102.c:533`, `:553`).

#### 2.4 GSP-RM boot binary (bootloader / SK) — **ESSENTIAL**

- **Source: compiled-in bindata.** [EVIDENCE] `kgspPrepareBootBinaryImage_IMPL`
  (`kernel_gsp.c:3146`, called from `kgspInitRm` `:2809`); storage via
  `kgspGetGspRmBootUcodeStorage_GA102` (`kernel_gsp_ga102.c:292`), keys
  `ucode_image_prod` / `ucode_desc_prod` (`:309-310`; dbg variants `:304-305`).
- [EVIDENCE] Copied to **4 KB-aligned** sysmem (`NV_ALIGN_UP(bufSize,0x1000)`,
  `kernel_gsp.c:3170`; alloc/copy `:3174-3202`); its descriptor is parsed into
  `RM_RISCV_UCODE_DESC` (`:3214`). Fields consumed later: `monitorCodeOffset`,
  `monitorDataOffset`, `manifestOffset` (→ WprMeta, `kernel_gsp_tu102.c:616-618`)
  and `appVersion` (→ `NV_PFALCON_FALCON_OS` after Booter, `kernel_gsp_ga102.c:256`).
  This is the "BOOT BIN (SK + BL)" placed at `bootBinOffset` in the FB map.

> **Blob summary (first pixel on GA102):** `gsp_ga10x.bin` (file) + FWSEC
> (from ROM) + Booter Load (bindata) + GSP-RM boot binary (bindata).
> **Skippable:** Booter Unload, `.fwlogging`, Scrubber ucode (GA102 has none —
> F03.5.4).

---

### F03.3 VBIOS extraction + FWSEC parse (version `94.02.71.40.C4`)

#### 3.1 Extract VBIOS from PROM — `kgspExtractVbiosFromRom_TU102`
(`.../arch/turing/kernel_gsp_vbios_tu102.c:444`)
- [EVIDENCE] Reads the PROM via `NV_PROM_DATA(offset)` with **raw OS reg reads**
  (`osDevReadReg008/032`) explicitly to bypass the `0xbadf` sanity filter of the
  normal register path (`kernel_gsp_vbios_tu102.c:59-87`).
- [EVIDENCE] Max scan size **1 MB** (`s_getBaseBiosMaxSize_TU102` returns
  `0x100000`, `:425/:430`); finds the PCI ROM header and walks the
  expansion-ROM chain (`s_romImgFindPciHeader_TU102:204`, used `:498`;
  `s_locateExpansionRoms:278`, used `:514`) to produce
  `KernelGspVbiosImg{pImage, biosSize, expansionRomOffset}`.
- [INFERENCE] StelluxOS must expose an **unfiltered** MMIO read for the
  expansion-ROM/PROM aperture (BAR0 region; CONTEXT_BRIEF §3 lists the 512 KB
  expansion ROM) — the open path deliberately avoids the `0xbadf` filter.

#### 3.2 Parse FWSEC out of the VBIOS — `kgspParseFwsecUcodeFromVbiosImg_IMPL`
(`kernel_gsp_fwsec.c:1081`)
- [EVIDENCE] Find the **BIT** header: `BIT_HEADER_ID = 0xB8FF`,
  `BIT_HEADER_SIGNATURE = 0x00544942` ("BIT\0") (`:43-44`, matched `:413-414`).
- [EVIDENCE] VBIOS version comes from the BIT **BIOSDATA** token
  (`BIT_TOKEN_BIOSDATA = 0x42`, `:70`, parsed `:508-526`); formatted by
  `_kgspVbiosVersionToStr` (`kernel_gsp.c:2615`) → `94.02.71.40.C4`. Confirmed
  on-wire: `CAP/boot-trace.txt:2` prints `ver 94.02.71.40.C4`.
- [EVIDENCE] Walk BIT **FALCON_DATA** token (`BIT_TOKEN_FALCON_DATA = 0x70`,
  `:82`, `:529`) → Falcon ucode table → the entry whose AppID is FWSEC:
  `FIRMWARE_SEC_LIC = 0x05` (`:117`), prod `FWSEC_PROD = 0x85` (`:119`), dbg
  `FWSEC_DBG = 0x45` (`:118`); prod-vs-dbg via `kgspIsDebugModeEnabled` (`:1110`),
  selected at `:595-596`.
- [EVIDENCE] Two descriptor layouts are handled: **V2** (`FALCON_UCODE_DESC_V2`,
  60 B, `:134-159`, boot-with-loader) and **V3** (`FALCON_UCODE_DESC_V3`, 44 B,
  `:162-179`, boot-from-HS with PKC). V3 carries `PKCDataOffset`, `EngineIdMask`,
  `UcodeId`, `SignatureVersions` (`:165,172,173,175`); RSA3K signature size is
  **384 B** (`BCRT30_RSA3K_SIG_SIZE`, `:181`; set `:902`). The chosen layout fills
  `s_vbiosFillFlcnUcodeFromDescV2` (`:680`) or `...V3` (`:850`); version select at
  `:626-635`.
- [INFERENCE] On GA10X FWSEC is expected to be the **V3 / boot-from-HS** variant
  (PKC, fuse-versioned signature selection used by the HS execute path
  `s_executeFwsec_TU102:272`). The exact desc version for `94.02.71.40.C4` is in
  the parsed VBIOS, which is not dumped by the trace — **TODO**.

---

### F03.4 WPR2 carve via FWSEC-FRTS (stage 05)

**WPR2 (Write-Protected Region 2)** is an FB range made CPU-inaccessible and
locked by the HW MMU; verified GSP-RM code/data/heap + FRTS data live inside it.
It is carved by running the VBIOS FWSEC ucode with the **FRTS** command on the GSP
Falcon.

- **Trigger.** [EVIDENCE] `kgspBootstrapRiscvOSEarly_GA102` runs FWSEC-FRTS only
  if `kgspGetFrtsSize_HAL > 0` (`kernel_gsp_ga102.c:194-201`). On GA102 FRTS size
  is hard-coded **1 MB** (`kgspGetFrtsSize_TU102` = `WPR_SIZE_1MB_IN_4K * 0x1000`,
  `kernel_gsp_frts_tu102.c:48-56`).
- **The command.** [EVIDENCE] `cmd = 0x15`
  (`FALCON_APPLICATION_INTERFACE_DMEM_MAPPER_V3_CMD_FRTS`,
  `kernel_gsp_frts_tu102.c:101`) is patched into FWSEC DMEM together with a
  `FWSECLIC_FRTS_CMD` (`:127-131`) describing the target:
  `frtsRegionOffset4K = frtsOffset >> 12` (`:317`),
  `frtsRegionSize = 0x100` (1 MB in 4 K; set `:318` from `blockSizeIn4K` =
  `NV_PGC6_AON_FRTS_INPUT_WPR_SIZE_..._WPR_SIZE_1MB_IN_4K` `:313` — note the
  `FWSECLIC_FRTS_REGION_SIZE_1MB_IN_4K` `:125` macro is the same `0x100` but is
  defined-unused), `frtsRegionMediaType = FB(2)`
  (`FWSECLIC_FRTS_REGION_MEDIA_FB`, `:124`, set `:319`). FWSEC then runs as an HS
  ucode (`kgspExecuteHsFalcon_HAL`, `:437`).
- **Verification after FRTS (the WPR2 proof).** [EVIDENCE]
  (`kernel_gsp_frts_tu102.c:446-482`): FRTS error-code scratch
  `NV_PBUS_VBIOS_SCRATCH(0x0E)` must be 0 (`:454-456`);
  `NV_PFB_PRI_MMU_WPR2_ADDR_HI` must be non-zero (`:463-465`); and
  `NV_PFB_PRI_MMU_WPR2_ADDR_LO` must equal `frtsOffset >> ADDR_LO_ALIGNMENT`
  (`:472-475`). The same `MMU_WPR2_ADDR_HI != 0` test is the canonical
  `kgspIsWpr2Up` (`kernel_gsp_tu102.c:978-988`), used as a pre-boot guard
  ("fail early if WPR2 is up", `kernel_gsp.c:2648`).
- **Ordering.** [EVIDENCE] FRTS must carve WPR2 **before** Booter Load, because
  Booter DMAs GSP-RM into WPR2 and the boot binary + FRTS data must coexist there
  (rationale `kernel_gsp.c:2712-2715`; sequence `kernel_gsp_ga102.c:194-253`).

---

### F03.5 FB layout and the `GspFwWprMeta` structure

The implementer populates `GspFwWprMeta` in **sysmem** and lets Booter DMA it to
FB at the end of WPR2. [EVIDENCE] Definition
`src/nvidia/arch/nvalloc/common/inc/gsp/gsp_fw_wpr_meta.h:58-199`, asserted to be
**exactly 256 bytes** (`ct_assert(sizeof(*pWprMeta) == 256)`,
`kernel_gsp_tu102.c:525`).

#### 5.1 The FB map (top of FB downward)
From the header diagram (`gsp_fw_wpr_meta.h:38-56`) and the computed offsets
(`kgspCalculateFbLayout_TU102`, `kernel_gsp_tu102.c:509-666`):

```
fbSize (end of FB)
  | VGA WORKSPACE                       |  vgaWorkspaceOffset .. fbSize
vbiosReservedOffset  (= min(VBIOS-MMU-lock, vgaWorkspaceOffset))
  | (alignment gap)                     |
gspFwWprEnd          (128K / 0x20000 aligned)        <-- top of WPR2
  | FRTS data (1 MB)                    |  frtsOffset .. gspFwWprEnd
  | BOOT BIN (SK + BL)                  |  bootBinOffset (4K aligned)
  | GSP FW ELF (radix3 image)           |  gspFwOffset   (64K aligned)
  | GSP FW (WPR) HEAP                   |  gspFwHeapOffset (1M aligned)
  | GspFwWprMeta (256 B, Booter-placed) |
gspFwWprStart        (1M aligned; HW req is 128K)    <-- bottom of WPR2
  | GSP FW (non-WPR) HEAP (1 MB)        |  nonWprHeapOffset
gspFwRsvdStart = nonWprHeapOffset
```

[EVIDENCE] Note `gspFwWprStart` is **code-aligned to 1 MB** even though the HW
requirement is only 128 KB, so the extra padding lands inside WPR rather than
between the non-WPR heap and WPR (comment + code, `kernel_gsp_tu102.c:594-599`).
The header diagram's "128K aligned" annotation (`gsp_fw_wpr_meta.h:53`) is the
requirement; the code uses 1 MB.

#### 5.2 Exact offset math (GA102, top-down) [EVIDENCE]
`kgspCalculateFbLayout_TU102` (`kernel_gsp_tu102.c`):
- `fbSize = kmemsysGetUsableFbSize()` — runtime, post-GFW (`:533`).
- `vgaWorkspaceOffset/Size` from `kdispGetVgaWorkspaceBase`, else
  `fbSize - DRF_SIZE(NV_PRAMIN)` (`:540-555`).
- `vbiosReservedOffset = min(mmuLockLo, vgaWorkspaceOffset)` if VBIOS MMU-lock
  valid, else `vgaWorkspaceOffset` (`:561-564`).
- `sizeOfRadix3Elf = pGspFw->imageSize` (`:567`).
- `gspFwWprEnd = ALIGN_DOWN(vbiosReservedOffset - kgspGetWprEndMargin(), 0x20000)`
  (`:570`).
- `frtsSize = 1MB`; `frtsOffset = gspFwWprEnd - frtsSize` (`:572-573`).
- `sizeOfBootloader = gspRmBootUcodeSize`;
  `bootBinOffset = ALIGN_DOWN(frtsOffset - sizeOfBootloader, 0x1000)` (`:576-577`).
- `gspFwOffset = ALIGN_DOWN(bootBinOffset - sizeOfRadix3Elf, 0x10000)` (`:583`).
- `wprHeapSize = kgspGetFwHeapSize(..., fbSize - gspFwOffset)` (`:585`);
  `gspFwHeapOffset = ALIGN_DOWN(gspFwOffset - wprHeapSize, 0x100000)`;
  `gspFwHeapSize = ALIGN_DOWN(gspFwOffset - gspFwHeapOffset, 0x100000)` (`:588-589`).
- `gspFwWprStart = ALIGN_DOWN(gspFwHeapOffset - sizeof(*pWprMeta), 0x100000)` (`:599`).
- `nonWprHeapSize = kgspGetNonWprHeapSize()` (1 MB, F03.5.4);
  `nonWprHeapOffset = ALIGN_DOWN(gspFwWprStart - nonWprHeapSize, 0x100000)`;
  `gspFwRsvdStart = nonWprHeapOffset` (`:602-605`).

#### 5.3 Fields the implementer must populate (sysmem `GspFwWprMeta`)
[EVIDENCE] field line refs in `gsp_fw_wpr_meta.h`; set sites in
`kernel_gsp_tu102.c`:
- **sysmem source pointers** (Booter DMA src): `sysmemAddrOfRadix3Elf`/
  `sizeOfRadix3Elf` (`:72-73`), `sysmemAddrOfBootloader`/`sizeOfBootloader`
  (`:75-76`), `sysmemAddrOfSignature`/`sizeOfSignature` (`:88-89`) — set from
  memdesc PAs at `kernel_gsp_tu102.c:608-613,622-623`.
- **bootloader sub-offsets** ← `RM_RISCV_UCODE_DESC`: `bootloaderCodeOffset`/
  `bootloaderDataOffset`/`bootloaderManifestOffset` (`:79-81`) ←
  `monitorCodeOffset`/`monitorDataOffset`/`manifestOffset`
  (`kernel_gsp_tu102.c:616-618`).
- **FB-layout offsets**: `gspFwRsvdStart`, `nonWprHeapOffset/Size`,
  `gspFwWprStart`, `gspFwHeapOffset/Size`, `gspFwOffset`, `bootBinOffset`,
  `frtsOffset/Size`, `gspFwWprEnd`, `fbSize`, `vgaWorkspaceOffset/Size`
  (`gsp_fw_wpr_meta.h:104-135`).
- **control/identity**: `bootCount = 0`, `verified = 0`, `revision = 1`
  (`GSP_FW_WPR_META_REVISION`), `magic = 0xdc3aae21371a60b3`
  (`GSP_FW_WPR_META_MAGIC`) — set at `kernel_gsp_tu102.c:636-639`; constants at
  `gsp_fw_wpr_meta.h:201-203`. Booter writes back
  `verified = 0xa0a0a0a0a0a0a0a0` (`GSP_FW_WPR_META_VERIFIED`, `:198,201`) once
  GSP-RM passes the signature check.
- **flags** (`gsp_fw_wpr_meta.h:191`): `GSP_FW_FLAGS_RECOVERY_MARGIN_PRESENT`
  (`:228`) is set/cleared by `kgspGetWprEndMargin` (`kernel_gsp.c:4237` set /
  `:4242` clear); **0 on a clean first boot** (normal path returns margin 0).

#### 5.4 Heap / size constants (GA102 baremetal, via NVOC HAL dispatch) [EVIDENCE]
- **non-WPR heap = 1 MB**: `kgspGetNonWprHeapSize_ed6b8b` returns `1048576`
  (`g_kernel_gsp_nvoc.h:776-778`), bound for GA102 (non-GH100) at
  `g_kernel_gsp_nvoc.c:536`; the 2 MB `_d505ea` (`:780-782`) is GH100-only.
- **min WPR heap = 84 MB** baremetal: `kgspGetMinWprHeapSizeMB_907c84` returns
  `bVgpu ? 549 : 84` (`g_kernel_gsp_nvoc.h:964-966`), bound at
  `g_kernel_gsp_nvoc.c:742`.
- **WPR heap formula** (`_kgspCalculateFwHeapSize`, `kernel_gsp.c:4076`,
  body `:4105-4115`):
  `osCarveout + baseRm + alignUp(96KB·fbSizeGB, 1MB) + alignUp(96MB, 1MB)`, then
  clamped. GA102 params: `fwHeapParamOsCarveoutSize = 20 MB`
  (`GSP_FW_HEAP_PARAM_OS_SIZE_LIBOS3`, `gsp_fw_heap.h:29`; bound
  `g_kernel_gsp_nvoc.c:314`), `fwHeapParamBaseSize = 8 MB`
  (`GSP_FW_HEAP_PARAM_BASE_RM_SIZE_TU10X`, `gsp_fw_heap.h:36`; bound `:298`);
  per-GB = `96 KB` (`gsp_fw_heap.h:45`); client-alloc = `48KB·2048 = 96 MB`
  (`gsp_fw_heap.h:65`).
- **Clamp bounds (CORRECTED — see note).** [EVIDENCE] Floor is **84 MB**
  (`kgspGetMinWprHeapSizeMB`, applied `kernel_gsp.c:4112`). The **default
  (non-regkey) path ceiling is the pre-scrubbed-region budget**
  `maxScrubbedHeapSizeMB` (passed as the `max` argument, applied `:4115`), NOT a
  fixed 276 MB. The **276 MB** baremetal cap
  (`GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS3_BAREMETAL_MAX_MB`, `gsp_fw_heap.h:74`) is the
  ceiling **only on the registry-override path** (`kernel_gsp.c:4162`).
- [INFERENCE] For a ~10 GB RTX 3080: `20 + 8 + alignUp(96KB·10,1MB)=1 + 96 =
  ~125 MB` WPR heap (≥ 84 MB floor, and well within the pre-scrubbed budget).
  **Exact value is dynamic — do not hard-code; capture from a live boot (TODO).**
- **Pre-scrub constraint.** [EVIDENCE] GA102 has **no scrubber ucode**:
  `bScrubberUcodeSupported = FALSE` (TRUE only for AD10X,
  `g_kernel_gsp_nvoc.c:284-292`), so when no scrubber is supported the heap is
  bounded to the **pre-scrubbed top ~256 MB of FB**
  (`kgspGetFwHeapSize_IMPL`, `kernel_gsp.c:4150-4154`; comment
  `gsp_fw_wpr_meta.h:35-36`). `_kgspPrepareScrubberImageIfNeeded`
  (`kernel_gsp.c:2627-2640`) only allocates a scrubber if
  `fbSize - gspFwRsvdStart > prescrubbedSize` (`:2630-2637`) — [INFERENCE] a
  no-op for first pixel on GA102 if WPR fits in the pre-scrubbed region.

---

### F03.6 GSP-RM image prep + radix3 page table

[EVIDENCE] `_kgspPrepareGspRmBinaryImage` (`kernel_gsp.c:3409`, run from
`kgspInitRm` `:2817`) does, in order:
1. `_kgspFwContainerVerifyVersion(.fwversion)` (`:3418`) — the version gate (F03.2.1).
2. `_kgspFwContainerGetSection(.fwimage)` → `pImageData`/`imageSize` (`:3424`).
3. resolve `.fwsignature_<family>` and copy to 256-aligned sysmem
   (`:3432-3447`).
4. `kgspCreateRadix3(... pImageData, imageSize)` (`:3449-3451`).

#### 6.1 radix3 — `kgspCreateRadix3_IMPL` (`kernel_gsp.c:3457`)
[EVIDENCE] A **3-level radix page table** (3 PDE levels + data) over 4 KB pages,
mapping the GSP-RM ELF placed in FB.
- Page size **4096**, log2 **12**; entries-per-page log2 = `12 - 3 = 9` (512 ×
  8-byte PA entries/page) — `kernel_gsp.c:3467`;
  `LIBOS_MEMORY_REGION_RADIX_PAGE_SIZE/LOG2` (`libos_init_args.h:47-48`).
- 4-entry working array `radix3[4]` (`:3484`): level 3 (data) `nPages =
  ceil(size/4096)` (`:3500-3502`); each higher level
  `nPages = ((lower-1) >> 9) + 1`, computed high→low (`:3503-3504`); the root
  `radix3[0]` must collapse to exactly **1 page** (`NV_ASSERT`, `:3513`).
- Allocates the PT pages (+ data pages, since `pData != NULL` here) as
  **non-contiguous SYSMEM, 4K-aligned, unprotected** (`:3519-3535`); fills PDEs
  with `memdescGetPhysAddrs` of the next level (`:3558-3566`), copies the image
  into the data region (`:3570-3581`), fills the level-2→data PTEs (`:3583-3588`),
  then unmaps the CPU side (GSP-only after) (`:3594`). Result:
  `pKernelGsp->pGspUCodeRadix3Descriptor`, whose PA becomes
  `sysmemAddrOfRadix3Elf` (F03.5.3).

#### 6.2 Boot binary into FB [EVIDENCE]
Booter DMAs the boot binary (SK+BL, F03.2.4) to `bootBinOffset` using
`sysmemAddrOfBootloader` + `bootloader{Code,Data,Manifest}Offset`. After Booter
Load, the driver writes `NV_PFALCON_FALCON_OS = pRiscvDesc->appVersion`
(`kernel_gsp_ga102.c:256`) and checks `kflcnIsRiscvActive` (stage 08, `:259`).

---

### F03.7 libos init args + boot-args memory

[EVIDENCE] Three small sysmem buffers are allocated up-front by
`kgspAllocBootArgs_TU102` (`kernel_gsp_tu102.c:108`), all with
`MEMDESC_FLAGS_ALLOC_IN_UNPROTECTED_MEMORY` (`:119`):

| Buffer | Size / align | Aperture / cache | Field (set site) |
|--------|--------------|------------------|------------------|
| `GspFwWprMeta` | `0x1000` / `0x1000` | SYSMEM, **CACHED** | `pWprMetaDescriptor`→`pWprMeta` (`:122-143`) |
| libos init args | `LIBOS_INIT_ARGUMENTS_SIZE = 0x1000` | SYSMEM, **UNCACHED** | `pLibosInitArgumentsDescriptor` (`:148-171`) |
| GSP args (cached) | `0x1000` / `0x1000` | SYSMEM, **CACHED** | `pGspArgumentsDescriptor`→`pGspArgumentsCached` (`:176-197`) |

`LIBOS_INIT_ARGUMENTS_SIZE = 0x1000` (`g_kernel_gsp_nvoc.h:188`);
`sizeof(GSP_ARGUMENTS_CACHED) <= 0x1000` asserted (`kernel_gsp_tu102.c:174`).

#### 7.1 libos init args — `kgspSetupLibosInitArgs_IMPL` (`kernel_gsp.c:3712`)
[EVIDENCE] Builds an array of `LibosMemoryRegionInitArgument`
(`{id8, pa, size, kind, loc}`, `libos_init_args.h:49-56`; kinds/locs `:35-45`):
- One entry per log task — **LOGINIT first** (required for early logging), then
  LOGINTR, LOGRM (loop `:3727-3734`; "LOGINIT must be first" `:3724`). Each
  `kind = LIBOS_MEMORY_REGION_CONTIGUOUS`, `loc = ..._LOC_SYSMEM`, `id8` from the
  ≤8-char tag (`_kgspGenerateInitArgId`, `kernel_gsp.c:2033`), and `pa =
  rmLibosLogMem[idx].pTaskLogBuffer[1]` — i.e. the **PTE table** for the log
  buffer, stashed right after the put-pointer (`_kgspInitLibosLoggingStructures`,
  `:2406-2412`).
- Final entry `id8 = "RMARGS"` → PA/size of `pGspArgumentsDescriptor`
  (`:3737-3741`). **The RMARGS entry is mandatory** even if logs are unused.
- **Entrypoint handoff.** [EVIDENCE] `kgspProgramLibosBootArgsAddr_TU102` writes
  `PA(pLibosInitArgumentsDescriptor)` lo/hi into
  `NV_PGSP_FALCON_MAILBOX0/MAILBOX1` (`kernel_gsp_tu102.c:329-339`) — this is how
  GSP finds its init args.

#### 7.2 GSP args (cached) — `kgspPopulateGspRmInitArgs_IMPL` (`kernel_gsp.c:3086`)
[EVIDENCE] Fills `GSP_ARGUMENTS_CACHED` (`gsp_init_args.h:51-62`):
`messageQueueInitArguments` (`MESSAGE_QUEUE_INIT_ARGUMENTS`, `:33-40`; shared-mem
PA + queue offsets — **S04 owns the queue mechanics**, `kernel_gsp.c:3099-3112`),
`srInitArguments` (`GSP_SR_INIT_ARGUMENTS`, `:42-46`; cold boot
`bInPMTransition = FALSE`, `oldLevel = 0`, `flags = 0`, `:3116-3118`),
`gpuInstance` (`:3127`), and `profilerArgs` (zeroed unless profiler enabled,
`:3129-3136`). Called at the very start of bootstrap (`kernel_gsp_ga102.c:187`).

#### 7.3 Log buffers (optional for first pixel)
[EVIDENCE] `_kgspInitLibosLoggingStructures` (`kernel_gsp.c:2332`) allocates
LOGINIT (`0x10000` = 64 KB, `:2346`), LOGINTR, LOGRM (release: `0x10000` = 64 KB
each, `:2353-2354`; debug builds use 256 KB) in SYSMEM (`:2387`). Useful for GSP
debugging but **not** needed to reach a pixel.

---

### F03.8 Loading the ucodes — the DMA-load-and-go routine (CORRECTION #8)

[EVIDENCE] All 1768 boot MMIO writes hit only two engines — the GSP Falcon block
`0x11xxxx` (1058) and the SEC2 Falcon block `0x84xxxx` (710) — and they are the
**same** Falcon "DMA-load-and-go" routine driven three times (F03.1 table). The
opening writes of each invocation program the Falcon DMA controller to stream the
ucode into IMEM/DMEM in 256-byte blocks:
`DMATRFBASE (0x110) / DMATRFMOFFS (0x114) / DMATRFCMD (0x118) / DMATRFFBOFFS
(0x11c)`, with `DMATRFCMD = 0x614` for secure IMEM and `0x600` for DMEM
(`CAP/boot-trace.txt`; CONTEXT_BRIEF §4a).

> This section intentionally does **not** re-derive the register program.
> **D01 is the authoritative byte-level reference** for: the 41 distinct offsets,
> the reset preamble, the BROM/PKC quartet (`MOD_SEL/BROM_CURR_UCODE_ID/
> BROM_ENGIDMASK/BROM_PARAADDR`), `BCR_CTRL` core-select, `BOOTVEC`, and
> `CPUCTL`/`CPUCTL_ALIAS` STARTCPU. D01.0/D01.3/D01.4: **1689 of 1768** writes are
> the 256-byte DMA inner loop; only **~79** control writes across **~22** offsets
> are the real implementation surface. The per-ucode `hsSigDmemAddr`, `ucodeId`
> (FWSEC = 9, Booter = 3), and `engineIdMask` come from the signed image
> descriptor (`s_allocateUcodeFromBinArchive`, `kernel_gsp_booter.c:326-338`),
> not invented.

---

### Minimal-path notes (essential vs skippable for first pixel on GA102)

**ESSENTIAL:**
- `gsp_ga10x.bin` at `nvidia/535.183.01/gsp_ga10x.bin`, version-matched to the
  driver; parse `.fwversion`/`.fwimage`/`.fwsignature_ga10x`.
- Extract VBIOS from PROM (1 MB scan, unfiltered reads) and parse FWSEC.
- Booter Load ucode (bindata, SEC2 HS, fuse-versioned PKC sig).
- GSP-RM boot binary (bindata) + parsed `RM_RISCV_UCODE_DESC`.
- radix3 over `.fwimage`; signature memdesc (256-aligned).
- `GspFwWprMeta` (256 B) fully populated (magic/rev/verified=0, all FB offsets and
  sysmem src PAs) in unprotected sysmem; pass its PA to Booter via SEC2
  mailbox0/1.
- libos init args (LOGINIT-first ordering; **RMARGS entry mandatory**) + program
  `NV_PGSP_FALCON_MAILBOX0/1` with the init-args PA.
- GSP-args cached (message-queue args — coordinate with S04).
- FWSEC-FRTS to carve **WPR2** (1 MB FRTS); verify via `NV_PFB_PRI_MMU_WPR2_*`.
- FB layout math: top-down from `fbSize` with the alignments in F03.5.2; honor the
  no-scrubber pre-scrub constraint (keep WPR in the top ~256 MB).
- Implement the DMA-load-and-go routine **once** and drive it 3× (F03.8 / D01).

**SKIPPABLE:**
- Booter **Unload** (teardown/suspend), FWSEC-**SB** (unload).
- `.fwlogging` ELF + libos log decode (debug only).
- Scrubber ucode (GA102 unsupported; no-op unless layout exceeds pre-scrubbed FB).
- CrashCat queue, profiler args, vGPU/partition (`GSP_FMC_BOOT_PARAMS`),
  WPR-end recovery margin (only on ECC/retry boots).

---

### Evidence cited

Source tree (`SRC = open-gpu-kernel-modules`):
- `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c` — `_kgspInitLibosLoggingStructures`
  (2332; LOGINIT 0x10000 2346, release sizes 2353-2354, SYSMEM 2387, PTE table
  2406-2412, id8 2414), `_kgspGenerateInitArgId` (2033),
  `_kgspVbiosVersionToStr` (2615), `_kgspPrepareScrubberImageIfNeeded`
  (2627-2640), `_kgspBootGspRm` (2643; WPR2 guard 2648, FbLayout 2656, scrubber
  2659, bootstrap 2662), `kgspInitRm_IMPL` (2682; RVGBOOT 2704/2751/2833,
  VBIOS/FWSEC 2719-2765, FWSEC parse 2734, FRTS rationale 2712-2715, Booter
  branch 2776-2806/2787/2798, boot-bin 2809, GSP-RM prep 2817),
  `kgspPopulateGspRmInitArgs_IMPL` (3086; MQ 3099-3112, srInit 3116-3118,
  gpuInstance 3127, profiler 3129-3136), `kgspPrepareBootBinaryImage_IMPL` (3146;
  4K align 3170, RM_RISCV_UCODE_DESC 3214), `_kgspCreateSignatureMemdesc` (3257;
  256-align 3270/3273), `_kgspFwContainerVerifyVersion` (3307; compare 3335-3336,
  INVALID_DATA 3351), `_kgspPrepareGspRmBinaryImage` (3409; 3418/3424/3445/3449),
  `kgspCreateRadix3_IMPL` (3457; 3467/3484/3500-3513/3519-3535/3558-3588/3594),
  `_kgspFwContainerGetSection` (3611; ELF magic/class/endian 3630-3632,3649-3653),
  `kgspSetupLibosInitArgs_IMPL` (3712; LOGINIT-first 3724, loop 3727-3734, RMARGS
  3737-3741), `_kgspCalculateFwHeapSize` (4076; formula 4105-4115),
  `kgspGetFwHeapSize_IMPL` (4135; prescrub 4150-4154, override max 4162, calc
  4188), `kgspGetWprEndMargin_IMPL` (4191; flag 4237/4242).
- `.../arch/ampere/kernel_gsp_ga102.c` — `kgspConfigureFalcon_GA102` (48;
  bBootFromHs 61), `_kgspResetIntoRiscv` (91; BCR RISCV 118),
  `kgspBootstrapRiscvOSEarly_GA102` (164; PopulateInitArgs 187, RVGBOOT
  190/204/207/253/270/277/281, FRTS gate 194-201, BooterLoad 244-245,
  FALCON_OS=appVersion 256), `kgspGetGspRmBootUcodeStorage_GA102` (292;
  prod keys 309-310, dbg 304-305).
- `.../arch/turing/kernel_gsp_tu102.c` — `kgspAllocBootArgs_TU102` (108;
  unprotected 119, WprMeta 122-143, libos UNCACHED 148-171, GSP args 174-197),
  `kgspProgramLibosBootArgsAddr_TU102` (329; MAILBOX0/1 338-339),
  `kgspCalculateFbLayout_TU102` (509; ct_assert 256 at 525, offsets 533-639,
  #if0 dump 641-663), `kgspIsWpr2Up_TU102` (978; WPR2_ADDR_HI 985).
- `.../arch/turing/kernel_gsp_frts_tu102.c` — `kgspGetFrtsSize_TU102` (48-56),
  CMD_FRTS 0x15 (101) / CMD_SB 0x19 (102), region desc 115-131 (MEDIA_FB 124,
  SIZE_1MB_IN_4K 125), scratch idx 0x0E (133), `s_executeFwsec_TU102` (272; FRTS
  build 310-325, HsFalcon 437, WPR2 verify 446-482), `kgspExecuteFwsecFrts_TU102`
  (533), `kgspExecuteFwsecSb_TU102` (553).
- `.../kernel_gsp_fwsec.c` — BIT id/sig (43-44), BIOSDATA 0x42 (70), FALCON_DATA
  0x70 (82), AppIDs 0x05/0x45/0x85 (117-119), DESC_V2 134-159, DESC_V3 162-179
  (PKCDataOffset 165, EngineIdMask 172, UcodeId 173, SignatureVersions 175),
  RSA3K 384 (181), BIT find 413-414, BIOSDATA parse 508-526, AppID select
  595-596, desc-ver select 626-635, fill V2/V3 (680/850), sigSize 902,
  `kgspParseFwsecUcodeFromVbiosImg_IMPL` (1081; debug mode 1110).
- `.../arch/turing/kernel_gsp_vbios_tu102.c` — raw PROM reads / 0xbadf bypass
  (59-87), `s_romImgFindPciHeader_TU102` (204), `s_locateExpansionRoms` (278),
  `s_getBaseBiosMaxSize_TU102` (425; 0x100000 at 430),
  `kgspExtractVbiosFromRom_TU102` (444; PCI 498, expansion roms 514).
- `.../kernel_gsp_booter.c` — `s_patchBooterUcodeSignature` (132; sigIndex 165),
  `s_allocateUcodeFromBinArchive` (173; prod/dbg 216-227, bBootFromHs 317, ucode
  fields 326-338), `kgspAllocateBooterLoadUcodeImage_IMPL` (435),
  `...UnloadUcodeImage_IMPL` (453), `...ScrubberUcodeImage_IMPL` (476).
- `.../arch/turing/kernel_gsp_booter_tu102.c` — `s_executeBooterUcode_TU102` (34;
  non-zero mbox0 fail 76-80), `kgspExecuteBooterLoad_TU102` (86; SEC2 96, mailbox
  106-107, reset 113), `kgspExecuteBooterUnloadIfNeeded_TU102` (129).
- `src/nvidia/arch/nvalloc/common/inc/gsp/gsp_fw_wpr_meta.h` — FB map (38-56),
  struct (58-199), FB-layout fields (104-135), flags (191),
  magic/rev/verified (198-203), RECOVERY_MARGIN flag (228).
- `src/nvidia/inc/kernel/gpu/gsp/gsp_init_args.h` — MESSAGE_QUEUE_INIT_ARGUMENTS
  (33-40), GSP_SR_INIT_ARGUMENTS (42-46), GSP_ARGUMENTS_CACHED (51-62).
- `src/common/uproc/os/common/include/libos_init_args.h` — kinds (35-39),
  locs (41-45), radix page size/log2 (47-48), LibosMemoryRegionInitArgument
  (49-56).
- `src/nvidia/inc/kernel/gpu/gsp/gsp_fw_heap.h` — OS_SIZE_LIBOS3 20MB (29),
  BASE_RM_SIZE_TU10X 8MB (36), SIZE_PER_GB_FB 96KB (45), CLIENT_ALLOC_SIZE 96MB
  (65), baremetal MIN 84 (73) / MAX 276 (74).
- `src/nvidia/arch/nvalloc/common/inc/gsp/gspifpub.h` — GSP_ACR_BOOT_GSP_RM_PARAMS
  (59-73), GSP_FMC_BOOT_PARAMS (89-94).
- `src/nvidia/arch/nvalloc/common/inc/nv-firmware.h` — chip-family enum (38-48),
  family→string (50-67; ga10x 57), path selection (80-98; ga10x 86-87), declared
  filenames (128-129).
- `kernel-open/nvidia/nv.c:27` — `NV_FIRMWARE_PATH_FOR_FILENAME`.
- `src/nvidia/generated/g_kernel_gsp_nvoc.h` — LIBOS_INIT_ARGUMENTS_SIZE (188),
  section names (208-211), non-WPR heap bodies ed6b8b=1048576 (776-778) /
  d505ea=2097152 (780-782), min WPR heap 907c84 (964-966).
- `src/nvidia/generated/g_kernel_gsp_nvoc.c` — bScrubberUcodeSupported (284-292;
  AD10X TRUE 287, else FALSE 292), fwHeapParamBaseSize 8MB (298),
  fwHeapParamOsCarveoutSize 20MB (314), kgspCalculateFbLayout→TU102 (526),
  non-WPR heap→ed6b8b (536), min WPR heap→907c84 (742).

Capture artifacts (`CAP = traces/20260530-115116-open-capture`):
- `CAP/boot-trace.txt:2` (VBIOS `94.02.71.40.C4`); stage markers and the 1768
  GSP/SEC2 register writes (full per-offset census in **D01**, which uses this
  same capture; D01.0 notes it is structurally identical to the `112551` capture).
- CONTEXT_BRIEF §3 (target HW), §4a (1768 writes split GSP `0x11xxxx` / SEC2
  `0x84xxxx`; DMATRF offsets / `0x614`/`0x600`).

Cross-referenced specs: **D01** (register reference / DMA-load-and-go), **S02**
(VBIOS devinit / Falcon-SEC2 register semantics), **S04** (RPC control plane).

---

### Open questions / TODO

- **[TODO] WPR `frtsOffset` / heap / FB offsets — runtime values unknown.** The
  capture is write-only, so the *computed* `frtsOffset`, `gspFwHeapSize`,
  `gspFwWprStart/End`, etc. are not in the trace. The in-driver dump that prints
  them is compiled out (`#if 0`, `kernel_gsp_tu102.c:641-663`). Enable that dump
  (or read the populated `GspFwWprMeta` from sysmem before Booter DMAs it) to get
  ground-truth offsets for the ~10 GB RTX 3080. The ~125 MB WPR-heap figure is an
  [INFERENCE] only.
- **[TODO] FWSEC descriptor version (V2 vs V3).** F03.3.2 marks V3/boot-from-HS as
  [INFERENCE]. Confirm by inspecting the parsed `94.02.71.40.C4` VBIOS
  (`kernel_gsp_fwsec.c:626-635` chooses V2@60B vs V3@44B).
- **[TODO] FMC vs Booter on the live target.** GA102 baremetal uses **separate
  Booter ucodes** (`bPartitionedFmc` false branch, `kernel_gsp.c:2776`). Confirm
  `bPartitionedFmc == false` on the live card (expected from the GA102 HAL);
  `GSP_FMC_BOOT_PARAMS` (`gspifpub.h:89-94`) is the GH100/partition path, out of
  scope here.
- **[TODO] Falcon register macro numeric values.** `NV_PFALCON_FALCON_DMATRFBASE/
  MOFFS/CMD/FBOFFS` (0x110/0x114/0x118/0x11c), `NV_PGSP_FALCON_MAILBOX0/1`,
  `NV_PFB_PRI_MMU_WPR2_ADDR_LO/HI` — **already resolved in D01.2** from
  `dev_falcon_v4.h` / `dev_fb.h`; use D01 as the numeric reference rather than
  re-deriving here.
- **[UNCERTAIN] In-tree docs.** No `vbios.rst` / `devinit.rst` exist in this tree;
  the authoritative reference for this section is the source cited above.


## F04 — GSP Boot: Falcon/SEC2 Register-Level Bring-up

> **What this is.** The final, register-accurate reference for GSP boot on the RTX 3080
> (GA102), from a cold card up to `GSP_INIT_DONE`. It merges the three boot drafts —
> **S04** (host orchestration + GSP-Falcon logic), **S05** (SEC2 Booter + FWSEC-FRTS /
> WPR2 carve), and **D01** (the exhaustive 1768-write census + reproducible pseudocode) —
> into one section, deduplicated and reconciled. Everything here is anchored to a primary
> source: a `path:line` in the open driver tree or an `artifact:line` / quoted trace line.
>
> **Path shortcuts** (per CONTEXT_BRIEF §5):
> - `SRC = /home/flare/dev/gpu-repro/open-gpu-kernel-modules`
> - `CAP = /home/flare/dev/gpu-repro/traces/20260530-115116-open-capture` — **primary**; all
>   *address values* quoted below are this capture's.
> - `CAP1 = /home/flare/dev/gpu-repro/traces/20260530-112551-open-capture` — S04/S05's
>   capture; structurally identical (1778 lines, 1768 writes, same 10 markers at the same
>   lines), differs only in printk timestamps + dynamic FB/sysmem addresses. **All line
>   citations resolve in both.** [EVIDENCE: heads of both `boot-trace.txt` compared;
>   `CAP/boot-trace.txt:15`=`0x00cfbd00` vs `CAP1/boot-trace.txt:15`=`0x00cfb000`.]
>
> **Trace-line format** (verified `CAP/boot-trace.txt:5`):
> `[<printk>] NVRM: osDevWriteReg032: RVGREG wr off=0x<abs> val=0x<u32>` = one host 32-bit
> MMIO write (`osDevWriteReg032` → `GPU_REG_WR32`). Register **reads/polls are NOT in this
> trace** — they are noted explicitly wherever an implementer must poll.
>
> **Register headers** are the GA102 swref set under
> `SRC/src/common/inc/swref/published/ampere/ga102/`. The generic Falcon offsets live once
> in `dev_falcon_v4.h` and apply to **both** Falcon blocks (only the base differs).
>
> **Scope boundary / cross-refs.** This section stops at `GSP_INIT_DONE`. The RPC control plane
> (663 / ≈1145 RPCs after INIT_DONE) = S06/D02; FB/WPR2 layout + `gspFwWprEnd`/`frtsOffset` math =
> S03; GSP static-info handles (`hInternalClient` etc.) = D03.

### Verification status (this section was re-verified against trace + headers)
Every offset, field-bit, composed value, marker line, and per-offset write count below was
re-checked. Findings:
- **All register headers resolve** (`dev_falcon_v4.h`, `dev_gsp.h`, `dev_sec_pri.h`,
  `dev_riscv_pri.h`, `dev_falcon_second_pri.h`, `dev_fbif_v4.h`, `dev_gc6_island_addendum.h`,
  the three `*_addendum.h` (gsp/sec/falcon_v4)). The DMATRFCMD words `0x614`/`0x600` and BCR `0x111`/`0x000` are
  re-derived from header bitfields **and** from the source `FLD_SET_DRF`/`DRF_DEF` calls.
- **All 10 stage markers are at the exact claimed lines** (1,2,3,4,727,731,1445,1447,1448,1778).
- **The full per-offset census reproduces exactly** (41 distinct offsets: 21 GSP + 20 SEC2;
  1768 writes = 1058 GSP + 710 SEC2) — see F04.3.
- **One correction applied (was wrong in S04.8):** the DMEM **command** count is **142**, not
  154. A naive count of `val=0x600` lines returns 154; the extra 12 are `DMATRFMOFFS`/
  `DMATRFFBOFFS` writes whose per-block offset coincidentally equals `0x600` (one MOFFS + one
  FBOFFS in each of the 6 DMA segments that reach offset `0x600`). The command-qualified count
  `off=…118 val=0x600` is 142 (44 GSP + 98 SEC2). [EVIDENCE: F04.9.]
- **Two labels downgraded to [INFERENCE]:** (a) the `TARGET=0` aperture name "`LOCAL_FB`" — the
  open `dev_fbif_v4.h` only defines `_TARGET_COHERENT_SYSMEM(=1)`, so "LOCAL_FB" for value 0 is
  the standard NV convention, not a published symbol; (b) the residual `0x110` bits inside
  `TRANSCFG=0x115/0x114` and `FBIF_CTL=0x190` (bits 4 and 8 are unnamed in the open header,
  preserved by the read-modify-write — RMW confirmed at `kgspExecuteHsFalcon_GA102:143`).
- **Path note:** S04 cited `turing/tu102/dev_gsp.h` for `NV_PGSP*`; the file is byte-identical
  to `ampere/ga102/dev_gsp.h` for these symbols. This doc cites the **ampere/ga102** copy
  (chip-correct).

---

### F04.0 — Two engines, two Falcon blocks (the whole boot touches only these)

[EVIDENCE] All **1768** boot register writes hit exactly two MMIO Falcon blocks (counted in
`CAP/boot-trace.txt`: `off=0x11*`=1058, `off=0x84*`=710):
- **GSP Falcon** base `0x110000` — `NV_PGSP = 0x113fff:0x110000` (`ampere/ga102/dev_gsp.h:26`)
  — **1058 writes**.
- **SEC2 Falcon** base `0x840000` — `NV_PSEC = 0x843fff:0x840000` (`ampere/ga102/dev_sec_pri.h:27`)
  — **710 writes**.

[EVIDENCE] Falcon register *offsets are identical across both blocks*; only the base differs.
Generic Falcon offsets are defined once in `ampere/ga102/dev_falcon_v4.h`
(`NV_PFALCON_FALCON_*`), with three sub-apertures per block:
- generic Falcon at base `+0x000`,
- **FBIF** at `fbifBase` (`NV_PGSP_FBIF_BASE=0x110600`, `dev_gsp_addendum.h:26`;
  `NV_PSEC_FBIF_BASE=0x840600`, `dev_sec_addendum.h:26`),
- **Falcon2 / BROM / RISC-V** at `NV_FALCON2_*_BASE` (`NV_FALCON2_GSP_BASE=0x111000`,
  `NV_FALCON2_SEC_BASE=0x841000`, `dev_falcon_second_pri.h:26,28`).

So `0x111668 = 0x111000+0x668` and `0x841668 = 0x841000+0x668`, etc. **A StelluxOS driver
implements ONE Falcon programming routine and points it at `0x110000` (GSP) or `0x840000`
(SEC2).**

[EVIDENCE] Engine objects are configured with these bases in source:
- GSP: `registerBase=DRF_BASE(NV_PGSP)`, `riscvRegisterBase=NV_FALCON2_GSP_BASE`,
  `fbifBase=NV_PGSP_FBIF_BASE`, `bBootFromHs=NV_TRUE`, `pmcEnableMask=0`, `physEngDesc=ENG_GSP`
  (`SRC/.../gsp/arch/ampere/kernel_gsp_ga102.c:58-64`).
- SEC2: `registerBase=DRF_BASE(NV_PSEC)` (`0x840000`), `riscvRegisterBase=NV_FALCON2_SEC_BASE`
  (`0x841000`), `fbifBase=NV_PSEC_FBIF_BASE` (`0x840600`), `bBootFromHs=NV_TRUE`
  (`SRC/.../sec2/arch/ampere/kernel_sec2_ga102.c:46-49`).

---

### F04.1 — The 10-stage orchestration

[EVIDENCE] Entry `kgspInitRm_IMPL` (`SRC/src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c:2682`)
runs stages 1–3, then `kgspBootstrapRiscvOSEarly_GA102`
(`SRC/.../gsp/arch/ampere/kernel_gsp_ga102.c:164`) runs stages 4–10. The reg-write trace is
gated **ON** only inside the bootstrap: `rvg_boot_trace=NV_TRUE` at `kernel_gsp_ga102.c:189`,
`=NV_FALSE` at `:285`. Therefore **every `RVGREG` line lies between marker 04 and marker 10**.

| # | Marker (trace line) | t (s, CAP) | Source site | What it does (cited) |
|---|---|---|---|---|
| 01 | `kgspInitRm START` (`:1`) | 4603.769 | `kernel_gsp.c:2704` | Begin GSP init; acquire locks. No MMIO writes. |
| 02 | `FWSEC ucode parsed from VBIOS (94.02.71.40.C4)` (`:2`) | 4603.933 | `kernel_gsp.c:2727-2751` | `kgspExtractVbiosFromRom` + `kgspParseFwsecUcodeFromVbiosImg`. |
| 03 | `GFW_BOOT ok` (`:3`) | 4603.950 | `kernel_gsp.c:2832` | Poll VBIOS-devinit/FWSEC-from-ROM done (`kgspWaitForGfwBootOk_HAL`). Reads only — F04.7. |
| 04 | `bootstrap START` (`:4`) | 4603.950 | `kernel_gsp_ga102.c:190` | Enter bootstrap: RISC-V-present check (`:181`), populate init args (`:187`), trace ON (`:189`). |
| 05 | `FWSEC-FRTS done (WPR2 carved)` (`:727`) | 4604.162 | `kernel_gsp_ga102.c:196-200` | `kflcnReset` GSP, then DMA-load+run FWSEC to carve FRTS/WPR2 (`kgspExecuteFwsecFrts_HAL`). Phase A writes (`:5-726`). |
| 06 | `reset into RISC-V done` (`:731`) | 4604.162 | `kernel_gsp_ga102.c:206` (`_kgspResetIntoRiscv`) | Secure-reset GSP Falcon, program RISC-V BCR. Phase B (`:728-730`). F04.4/F04.7. |
| 07 | `Booter Load done (GSP-RM authenticated into WPR2)` (`:1445`) | 4604.319 | `kernel_gsp_ga102.c:244-245` (`kgspExecuteBooterLoad_HAL`) | Run Booter Load **on SEC2** to authenticate+boot GSP-RM into WPR2. Phase C (`:732-1444`). F04.6. |
| 08 | `RISC-V active (GSP-RM running)` (`:1447`) | 4604.319 | `kernel_gsp_ga102.c:256-259` | Write `FALCON_OS=appVersion` (`:256`, trace `:1446`), then **read** `kflcnIsRiscvActive` (`:259`). Phase D. F04.7. |
| 09 | `waiting for INIT_DONE...` (`:1448`) | 4604.454 | `kernel_gsp_ga102.c:274-278` | `GspStatusQueueInit` then `kgspWaitForRmInitDone`. No MMIO writes between 08→09. |
| 10 | `INIT_DONE received (GSP-RM ready)` (`:1778`) | 4605.793 | marker print `kernel_gsp_ga102.c:281`; work in `kgspWaitForRmInitDone`→`rpcRecvPoll(GSP_INIT_DONE)` (`kernel_gsp.c:3787-3796`) | During this poll the host **replays GSP-RM's CPU sequencer** → Phase E writes (`:1449-1777`). F04.8. |

[EVIDENCE] Each of markers 01→04 prints once → a **single, successful boot** (no retry of the
`MAX_GSP_BOOT_ATTEMPTS=4` loop at `kernel_gsp.c:2848-2861`).

---

### F04.2 — The ONE "DMA-load-and-go" routine, reused 3× (the core mechanic)

This is the central fact of GSP boot on GA102 (CORRECTIONS #8): **the entire boot is one
`kflcnReset` preamble + one `kgspExecuteHsFalcon_GA102` "DMA-load-and-go" routine, invoked
three times** with different (base, ucode, source-aperture, MAILBOX0). 1689 of the 1768 writes
are the 256-byte DMA inner loop; only **79 are one-shot control writes**, across **22 distinct
register offsets** (F04.3) — that 22-offset surface + the loop is the entire thing to implement.

[EVIDENCE] Source of truth: `s_dmaTransfer_GA102` (per-block engine,
`SRC/.../gsp/arch/ampere/kernel_gsp_falcon_ga102.c:38-92`) and `kgspExecuteHsFalcon_GA102`
(the full load+kick, `:110-236`); reset/ctx/start helpers in
`SRC/.../falcon/arch/turing/kernel_falcon_tu102.c`. Constants `FLCN_BLK_ALIGNMENT=256` and
`FLCN_DMEM_VA_INVALID=0xffffffff` (`SRC/src/nvidia/inc/kernel/gpu/falcon/falcon_common.h:29,54`).

#### F04.2a — The mechanic (what each step writes)
1. **Reset** the Falcon (`kflcnReset` = `kflcnEnable(FALSE)`+`kflcnEnable(TRUE)`,
   `kernel_falcon_tu102.c:155-164`). With `pmcEnableMask==0` each enable does a secure reset:
   toggles `FALCON_ENGINE.RESET` 1→0 and writes `BCR_CTRL=CORE_SELECT_FALCON(0)` via
   `kflcnSwitchToFalcon_GA10X` (`kernel_falcon_ga102.c:84-104`); the TRUE pass then writes
   `FALCON_RM=pGpu->chipId0` (`kernel_falcon_tu102.c:233-234`).
2. **Disable ctx-DMA** so physical addressing is allowed: `FBIF_CTL |= ALLOW_PHYS_NO_CTX` and
   `DMACTL=0` (`kflcnDisableCtxReq_TU102`, `kernel_falcon_tu102.c:260-277`).
3. **Program the DMA aperture** `FBIF_TRANSCFG(0) = TARGET + MEM_TYPE_PHYSICAL` via **RMW**
   (`kgspExecuteHsFalcon_GA102:143-146`): sysmem ucode → `TARGET_COHERENT_SYSMEM` (`val=0x115`);
   FB-sourced GSP-RM replay → `TARGET=0` ("LOCAL_FB" [INFERENCE]) (`val=0x114`).
4. **DMA each segment** (IMEM then DMEM) via `s_dmaTransfer_GA102`: program
   `DMATRFBASE=srcPhys>>8` (lo) + `DMATRFBASE1=(srcPhys>>8)>>32 & 0x1FF` once, then loop per
   256 B: `DMATRFMOFFS=dest`, `DMATRFFBOFFS=memOff`, `DMATRFCMD=cmd`, **poll `DMATRFCMD.IDLE`
   (a READ)**, advance dest/memOff by 256.
5. **Program BROM / PKC** (order = PARAADDR, ENGIDMASK, CURR_UCODE_ID, MOD_SEL;
   `kgspExecuteHsFalcon_GA102:203-211`).
6. **Set BOOTVEC = imemVa** (`:215`); set **MAILBOX0/1** if args (`:218-221`).
7. **GO**: `StartCpu` — write `CPUCTL.STARTCPU=0x2`, or `CPUCTL_ALIAS.STARTCPU=0x2` if
   `CPUCTL.ALIAS_EN` is set (`kflcnStartCpu_TU102`, `kernel_falcon_tu102.c:242-255`).
8. **Wait for halt** (HS ucode self-halts; `kflcnWaitForHalt`, a READ), then **read MAILBOX0**
   (`0`==success for the Booter) (`kgspExecuteHsFalcon_GA102:227-233`).

[EVIDENCE] The command words are literally composed by `FLD_SET_DRF` and verified against
`dev_falcon_v4.h:57-73`:
- **IMEM**: `WRITE_FALSE | SIZE_256B | CTXDMA=0 | IMEM_TRUE | SEC=1`
  (`kgspExecuteHsFalcon_GA102:150-156`) = `SIZE(6<<8=0x600) | IMEM(1<<4=0x10) | SEC(1<<2=0x4)`
  = **`0x614`** (trace `CAP/boot-trace.txt:19`).
- **DMEM**: `IMEM_FALSE | SEC=0` (`:173-174`) = **`0x600`** (trace `:720`). If
  `dmemVa != FLCN_DMEM_VA_INVALID` it also sets `SET_DMTAG` (bit16) → `0x10600` (`:175-178`);
  not triggered on this boot (trace shows `0x600`, so `dmemVa` was INVALID).

#### F04.2b — Reproducible pseudocode (drop-in for StelluxOS)
Implement **once**, parameterized by `FB ∈ {0x110000 GSP, 0x840000 SEC2}`; `fbifBase=FB+0x600`;
`falcon2Base=FB+0x1000`. Verified line-by-line against the cited source + trace.

```c
// composed command words (dev_falcon_v4.h:62-70; verified against trace)
#define DMACMD_IMEM   0x614   // SIZE_256B(0x600) | IMEM(0x10) | SEC=1(0x04)
#define DMACMD_DMEM   0x600   // SIZE_256B only            (+0x10000 SET_DMTAG iff dmemVa valid)

// one 256B-granular DMA of a ucode segment (FB/sysmem -> Falcon IMEM or DMEM)
void flcn_dma_segment(u32 FB, u32 dest, u32 memOff, u64 srcPhys, u32 size, u32 cmd) {
    wr32(FB+0x110, (u32)(srcPhys >> 8));                  // DMATRFBASE              (s_dmaTransfer:55)
    wr32(FB+0x128, (u32)((srcPhys >> 8) >> 32) & 0x1FF);  // DMATRFBASE1             (:56)
    for (u32 done = 0; done < size; done += 256) {        // FLCN_BLK_ALIGNMENT=256
        wr32(FB+0x114, dest);                             // DMATRFMOFFS dest        (:60-61)
        wr32(FB+0x11c, memOff);                           // DMATRFFBOFFS src+base   (:63-64)
        wr32(FB+0x118, cmd);                              // DMATRFCMD  kick block   (:67)
        while ((rd32(FB+0x118) & DMATRFCMD_IDLE) == 0) cpu_relax(); // poll IDLE READ (:70-84)
        dest += 256; memOff += 256;
    }
}

// reset preamble (kflcnReset -> 2x kflcnEnable; secure reset; pmcEnableMask==0).
// The trace shows only the writes; the polls below are REQUIRED reads (not traced), per
// kflcnSecureReset_TU102 = PreResetWait -> ResetHw(toggle) -> WaitForResetToFinish -> SwitchToFalcon.
void flcn_reset(u32 FB) {
    for (int pass = 0; pass < 2; pass++) {                // FALSE then TRUE
        poll_until(  rd32(FB+0x0f4) & (1u<<31));          // HWCFG2.RESET_READY (kflcnPreResetWait; has timeout)
        wr32(FB+0x3c0, 1);                                // FALCON_ENGINE.RESET=TRUE
        wr32(FB+0x3c0, 0);                                // FALCON_ENGINE.RESET=FALSE
        poll_until(!(rd32(FB+0x0f4) & (1u<<12)));         // HWCFG2.MEM_SCRUBBING==DONE  *** before any DMA ***
        wr32(FB+0x1000+0x668, 0x000);                     // BCR_CTRL = CORE_SELECT_FALCON (switchToFalcon)
        poll_until(  rd32(FB+0x1000+0x668) & (1u<<0));    // BCR_CTRL.VALID (switchToFalcon: core switch done)
    }
    wr32(FB+0x084, chipId0);                              // FALCON_RM = pGpu->chipId0  (0xb72000a1 here)
}

// full "load-and-go" of one signed HS ucode (FWSEC / Booter / GSP-RM bootloader)
NV_STATUS flcn_hs_load_and_go(u32 FB, ucode_t *u, u32 *mbox0, u32 *mbox1, bool srcIsFb) {
    // caller already did flcn_reset(FB)
    rmw32(FB+0x624, FBIF_CTL_ALLOW_PHYS_NO_CTX);          // FBIF_CTL |= bit7  -> 0x190 (kflcnDisableCtxReq)
    wr32 (FB+0x10c, 0);                                   // DMACTL = 0
    // aperture (RMW; residual 0x110 preserved): sysmem=0x115, FB=0x114
    rmw32(FB+0x600, (srcIsFb ? MEM_TYPE_PHYSICAL          // 0x004 -> 0x114
                             : (TARGET_COHERENT_SYSMEM|MEM_TYPE_PHYSICAL))); // 0x005 -> 0x115
    flcn_dma_segment(FB, u->imemPa, u->imemVa, u->codePhys, u->imemSize, DMACMD_IMEM); // 0x614
    // DMEM memOff/cmd depend on dmemVa: INVALID on THIS boot -> memOff=0, cmd=0x600 (no SET_DMTAG);
    // if dmemVa valid -> memOff=dmemVa, srcPhys-=dmemVa, cmd|=SET_DMTAG=0x10600 (kgspExecuteHsFalcon_GA102:173-198)
    u32 dmemOff = (u->dmemVa == FLCN_DMEM_VA_INVALID) ? 0 : u->dmemVa;
    u32 dmemCmd = (u->dmemVa == FLCN_DMEM_VA_INVALID) ? DMACMD_DMEM : (DMACMD_DMEM | 0x10000);
    flcn_dma_segment(FB, u->dmemPa, dmemOff, u->dataPhys, u->dmemSize, dmemCmd); // 0x600 on this boot
    // BROM / PKC signature config (order PARAADDR, ENGIDMASK, CURR_UCODE_ID, MOD_SEL)
    wr32(FB+0x1000+0x210, u->hsSigDmemAddr);              // BROM_PARAADDR(0)
    wr32(FB+0x1000+0x19c, u->engineIdMask);               // BROM_ENGIDMASK
    wr32(FB+0x1000+0x198, u->ucodeId);                    // BROM_CURR_UCODE_ID (FWSEC=9, Booter=3, GSP-RM=1)
    wr32(FB+0x1000+0x180, /*MOD_SEL.ALGO=RSA3K*/ 1);      // MOD_SEL
    wr32(FB+0x104, u->imemVa);                            // BOOTVEC
    if (mbox0) wr32(FB+0x040, *mbox0);                    // MAILBOX0 (arg in)
    if (mbox1) wr32(FB+0x044, *mbox1);                    // MAILBOX1
    if (rd32(FB+0x100) & CPUCTL_ALIAS_EN) wr32(FB+0x130, 0x2);  // STARTCPU via alias if ALIAS_EN
    else                                  wr32(FB+0x100, 0x2);  // else CPUCTL.STARTCPU
    wait_for_halt(FB);                                    // READ poll (kflcnWaitForHalt)
    if (mbox0) *mbox0 = rd32(FB+0x040);                   // Booter success == MAILBOX0 0
    return ...;
}
```

#### F04.2c — The three invocations (same routine, three parameter sets) [EVIDENCE]
| Invocation (phase) | Falcon base | Source aperture | ucode (ucodeId) | MAILBOX0 in | IMEM blk | DMEM blk | DMA kicks |
|---|---|---|---|---|---|---|---|
| **FWSEC-FRTS** (A) | GSP `0x110000` | sysmem (`TRANSCFG=0x115`) | FWSEC (**9**, `:723`) | — (FRTS cmd is in DMEM, F04.5) | 226 | 8 | **234** |
| **Booter-Load** (C) | SEC2 `0x840000` | sysmem (`0x115`) | Booter (**3**, `:1439`) | `phys(GspFwWprMeta)` = `0xc7eda000` (`:1442`) | 131 | 98 | **229** |
| **GSP-RM bootloader** (E, via sequencer) | GSP `0x110000` | **FB** (`TRANSCFG=0x114`, `:1459`) | id **1** (`:1766`) | `0xfe` (`:1768`, [UNCERTAIN]) | 64 | 36 | **100** |

[EVIDENCE] The prologues line up byte-for-byte (only the base differs) — direct proof of reuse:

| step | GSP FWSEC (A) | SEC2 Booter (C) | offset = value |
|---|---|---|---|
| ENGINE.RESET 1,0 (enable FALSE) | `:5,6` | `:736,737` | `+0x3c0` = 1,0 |
| BCR_CTRL → FALCON | `:7` | `:738` | `f2+0x668` = 0 |
| ENGINE.RESET 1,0 (enable TRUE) | `:8,9` | `:739,740` | `+0x3c0` = 1,0 |
| BCR_CTRL → FALCON | `:10` | `:741` | `f2+0x668` = 0 |
| FALCON_RM = chipId0 | `:11` | `:742` | `+0x84` = `0xb72000a1` |
| FBIF_CTL | `:12` | `:743` | `+0x624` = `0x190` |
| DMACTL = 0 | `:13` | `:744` | `+0x10c` = 0 |
| FBIF_TRANSCFG(0) | `:14` | `:745` | `fbif+0x0` = `0x115` |
| DMATRFBASE (IMEM) | `:15` | `:746` | `+0x110` (per-ucode addr) |

[EVIDENCE] The **STARTCPU register differs by call site**: the first SEC2 Booter kick uses
`CPUCTL` (`0x840100`, `:1444`); the later sequencer restart of SEC2 uses `CPUCTL_ALIAS`
(`0x840130`, `:1776`) because the authenticated Booter set `CPUCTL.ALIAS_EN`. Your `StartCpu`
must branch on `ALIAS_EN` (handled in the pseudocode). GSP `BCR_CTRL` is set to `0x111` (RISC-V)
at reset-into-RISC-V (`:730`, `:1773`); SEC2's `BCR_CTRL` is only ever `0x000` (it runs the
Booter as a plain Falcon, never RISC-V).

---

### F04.3 — Master register map + write census (the ~22-offset surface) [EVIDENCE]

Offsets are identical across both blocks (`0x110000` GSP / `0x840000` SEC2). Counts are the
**exact** `RVGREG` write counts in `CAP/boot-trace.txt` (independently reproduced — F04.9). The
22 generic offsets below are the **complete implementation surface**; of these, the three DMA
loop registers (`0x114/0x118/0x11c`) carry 1689 writes, the other 19 carry the 79 one-shot
control writes.

| Gen off | GSP abs | SEC2 abs | Register (header `ampere/ga102/…`) | def | GSP× | SEC2× | Role |
|---|---|---|---|---|---|---|---|
| `0x040` | `0x110040` | `0x840040` | `NV_PFALCON_FALCON_MAILBOX0` | `dev_falcon_v4.h:44` | 4 | 1 | arg in / status |
| `0x044` | `0x110044` | `0x840044` | `NV_PFALCON_FALCON_MAILBOX1` | `:45` | 2 | 1 | arg hi |
| `0x080` | `0x110080` | — | `NV_PFALCON_FALCON_OS` | `:104` | 2 | 0 | `=appVersion` (stage 08) |
| `0x084` | `0x110084` | `0x840084` | `NV_PFALCON_FALCON_RM` | `:105` | 2 | 1 | `=chipId0` on enable |
| `0x100` | `0x110100` | `0x840100` | `NV_PFALCON_FALCON_CPUCTL` (`_STARTCPU` 1:1, `_ALIAS_EN` 6:6) | `:107-114` | 2 | 1 | STARTCPU=0x2 |
| `0x104` | `0x110104` | `0x840104` | `NV_PFALCON_FALCON_BOOTVEC` | `:120` | 2 | 1 | boot vector = imemVa |
| `0x10c` | `0x11010c` | `0x84010c` | `NV_PFALCON_FALCON_DMACTL` | `:46` | 2 | 1 | `=0` allow DMA w/o ctx |
| `0x110` | `0x110110` | `0x840110` | `NV_PFALCON_FALCON_DMATRFBASE` | `:53` | 4 | 2 | src phys>>8 (per segment) |
| `0x114` | `0x110114` | `0x840114` | `NV_PFALCON_FALCON_DMATRFMOFFS` (`_OFFS` 23:0) | `:55` | 334 | 229 | **DMA loop**: dest |
| `0x118` | `0x110118` | `0x840118` | `NV_PFALCON_FALCON_DMATRFCMD` (fields :58-73) | `:57` | 334 | 229 | **DMA loop**: kick |
| `0x11c` | `0x11011c` | `0x84011c` | `NV_PFALCON_FALCON_DMATRFFBOFFS` (`_OFFS` 31:0) | `:74` | 334 | 229 | **DMA loop**: src off |
| `0x128` | `0x110128` | `0x840128` | `NV_PFALCON_FALCON_DMATRFBASE1` (`_BASE` 8:0) | `:76` | 4 | 2 | src phys[40:32] (per segment) |
| `0x130` | — | `0x840130` | `NV_PFALCON_FALCON_CPUCTL_ALIAS` (`_STARTCPU` 1:1) | `:116` | 0 | 1 | STARTCPU via alias (post-Booter) |
| `0x3c0` | `0x1103c0` | `0x8403c0` | `NV_P{GSP,SEC}_FALCON_ENGINE` (`_RESET` 0:0) | GSP `dev_gsp.h:31`; SEC2 `dev_sec_pri.h:28` | 12 | 4 | reset toggle |
| fbif`+0x000` | `0x110600` | `0x840600` | `NV_PFALCON_FBIF_TRANSCFG(0)` (`_TARGET` 1:0, `_MEM_TYPE` 2:2) | `dev_fbif_v4.h:27-32` | 2 | 1 | DMA aperture |
| fbif`+0x024` | `0x110624` | `0x840624` | `NV_PFALCON_FBIF_CTL` (`_ALLOW_PHYS_NO_CTX` 7:7) | `dev_fbif_v4.h:33-35` | 2 | 1 | allow phys, no ctx |
| `0xc00` | `0x110c00` | — | `NV_PGSP_QUEUE_HEAD(0)` | `dev_gsp.h:38` | 2 | 0 | RPC msgq head = 0 |
| f2`+0x180` | `0x111180` | `0x841180` | `NV_PFALCON2_FALCON_MOD_SEL` (`_ALGO` 7:0, `_RSA3K`=1) | `dev_falcon_second_pri.h:29-31` | 2 | 1 | sig algo RSA3K |
| f2`+0x198` | `0x111198` | `0x841198` | `NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID` | `:32-33` | 2 | 1 | HS ucode id |
| f2`+0x19c` | `0x11119c` | `0x84119c` | `NV_PFALCON2_FALCON_BROM_ENGIDMASK` | `:34` | 2 | 1 | engine-id mask |
| f2`+0x210` | `0x111210` | `0x841210` | `NV_PFALCON2_FALCON_BROM_PARAADDR(0)` | `:35` | 2 | 1 | sig DMEM addr |
| f2`+0x668` | `0x111668` | `0x841668` | `NV_PRISCV_RISCV_BCR_CTRL` (`_VALID` 0:0, `_CORE_SELECT` 4:4, `_BRFETCH` 8:8) | `dev_riscv_pri.h:33-42` | 6 | 2 | core-select / RISC-V boot cfg |

Column sums: **GSP 1058, SEC2 710 → 1768** ✓ (21 GSP offsets [no `0x130`], 20 SEC2 [no `0x080`,
`0xc00`] → 41 distinct absolute / 22 distinct generic).

**Read-only registers polled this boot (NOT in the write trace — implement as polls):**

| Register | Abs (GSP) | def | Purpose | F04 |
|---|---|---|---|---|
| `NV_PFALCON_FALCON_HWCFG2` (`_RISCV` 10:10) | `0x1100f4` | `dev_falcon_v4.h:99-101` | RISC-V core present (precondition) | F04.7 |
| `NV_PFALCON_FALCON_HWCFG2` (`_RESET_READY` 31:31) | `0x1100f4` | `dev_falcon_v4_addendum.h:27-29` | pre-reset gate (`kflcnPreResetWait`; best-effort, has timeout) | F04.7 |
| `NV_PFALCON_FALCON_HWCFG2` (`_MEM_SCRUBBING` 12:12, `_DONE`=0) | `0x1100f4` | `dev_falcon_v4.h:102-103` | **IMEM/DMEM scrub DONE — poll before any DMA** (`kflcnWaitForResetToFinish`) | F04.2/7 |
| `NV_PRISCV_RISCV_BCR_CTRL` (`_VALID` 0:0) | `0x111668` | `dev_riscv_pri.h:34-36` | core-switch done after `BCR_CTRL→FALCON` (`kflcnSwitchToFalcon`) | F04.7 |
| `NV_PRISCV_RISCV_CPUCTL` (`_ACTIVE_STAT` 7:7) | `0x111388` | `dev_riscv_pri.h:30-32` | stage-08 "RISC-V active" | F04.7 |
| `NV_PFALCON_FALCON_DMATRFCMD` (`_IDLE` 1:1) | per-block | `dev_falcon_v4.h:60-61` | DMA block completion poll | F04.2 |
| `GFW_BOOT` (`_PROGRESS` 7:0, `_COMPLETED`=0xFF) | `NV_PGC6` scratch | `dev_gc6_island_addendum.h:30-32` | VBIOS devinit done (stage 03) | F04.7 |
| `WPR2_ADDR_LO/HI`, VBIOS scratch `0x0E` | `NV_PFB`/`NV_PBUS` | (see F04.5) | WPR2-carve verify (post stage 05) | F04.5 |

---

### F04.4 — The 1768 writes, phase by phase [EVIDENCE]

| Phase | Stage span (markers) | Trace lines | #writes | Block | Runs |
|---|---|---|---|---|---|
| — | 01→04 (`:1`→`:4`) | — | 0 | none | InitRm + VBIOS/FWSEC parse + GFW_BOOT poll (reads) |
| **A** | 04→05 (`:4`→`:727`) | `:5-726` | **722** | GSP | FWSEC-FRTS DMA-load+run on GSP (carve WPR2) |
| **B** | 05→06 (`:727`→`:731`) | `:728-730` | **3** | GSP | reset-into-RISC-V (`BCR_CTRL→0x111`) |
| **C** | 06→07 (`:731`→`:1445`) | `:732-1444` | **713** | 4×GSP prime + 709×SEC2 | Booter-Load DMA-load+run on SEC2 (auth GSP-RM into WPR2) |
| **D** | 07→08 (`:1445`→`:1447`) | `:1446` | **1** | GSP | `FALCON_OS=appVersion` before RISC-V-active read |
| — | 08→09 (`:1447`→`:1448`) | — | 0 | none | status-queue init (no MMIO writes) |
| **E** | 09→10 (`:1448`→`:1778`) | `:1449-1777` | **329** | 328×GSP + 1×SEC2 | host replays GSP-RM CPU sequencer (3rd Falcon load from FB + CORE_RESUME) |

`722 + 3 + 713 + 1 + 329 = 1768` ✓.

**Phase A — FWSEC-FRTS on GSP (`:5-726`)** — reset preamble → aperture → IMEM/DMEM loop → BROM →
BOOTVEC → STARTCPU:
```
:5  off=0x1103c0 val=0x00000001   # FALCON_ENGINE.RESET=TRUE   (kflcnEnable FALSE)
:6  off=0x1103c0 val=0x00000000   # RESET=FALSE
:7  off=0x111668 val=0x00000000   # BCR_CTRL = CORE_SELECT_FALCON
:8  off=0x1103c0 val=0x00000001   # RESET=TRUE                  (kflcnEnable TRUE)
:9  off=0x1103c0 val=0x00000000   # RESET=FALSE
:10 off=0x111668 val=0x00000000   # BCR_CTRL = CORE_SELECT_FALCON
:11 off=0x110084 val=0xb72000a1   # FALCON_RM = pGpu->chipId0
:12 off=0x110624 val=0x00000190   # FBIF_CTL |= ALLOW_PHYS_NO_CTX
:13 off=0x11010c val=0x00000000   # DMACTL = 0
:14 off=0x110600 val=0x00000115   # FBIF_TRANSCFG(0) = COHERENT_SYSMEM | PHYSICAL
:15 off=0x110110 val=0x00cfbd00   # DMATRFBASE = FWSEC code @ 0xcfbd0000 (>>8)
:16 off=0x110128 val=0x00000000   # DMATRFBASE1
:17 off=0x110114 val=0x00000000   ] first IMEM 256B block
:18 off=0x11011c val=0x00000000   ]
:19 off=0x110118 val=0x00000614   # DMATRFCMD IMEM|SEC|256B (then poll IDLE — READ)
...  (226 IMEM blocks; then DMATRFBASE re-program + 8 DMEM blocks @0x600)
:721 off=0x111210 val=0x000005a4  # BROM_PARAADDR(0) = hsSigDmemAddr
:722 off=0x11119c val=0x00000400  # BROM_ENGIDMASK
:723 off=0x111198 val=0x00000009  # BROM_CURR_UCODE_ID = 9 (FWSEC)
:724 off=0x111180 val=0x00000001  # MOD_SEL.ALGO = RSA3K
:725 off=0x110104 val=0x00000000  # BOOTVEC = imemVa (0)
:726 off=0x110100 val=0x00000002  # CPUCTL.STARTCPU -> run FWSEC; then wait-for-halt (READ)
```

**Phase B — reset-into-RISC-V on GSP (`:728-730`)**:
```
:728 off=0x1103c0 val=0x00000001  # FALCON_ENGINE.RESET=TRUE
:729 off=0x1103c0 val=0x00000000  # RESET=FALSE
:730 off=0x111668 val=0x00000111  # BCR_CTRL = VALID|CORE_SELECT_RISCV|BRFETCH (0x1|0x10|0x100)
```

**Phase C — 4 GSP prime writes, then Booter-Load on SEC2 (`:732-1444`)**:
```
:732 off=0x110c00 val=0x00000000  # GSP QUEUE_HEAD(0)=0  (RPC msgq head)
:733 off=0x110c00 val=0x00000000  #   (written twice)
:734 off=0x110040 val=0xc7ed9000  # GSP MAILBOX0 = libos boot-args phys (kgspProgramLibosBootArgsAddr)
:735 off=0x110044 val=0x00000000  # GSP MAILBOX1
:736 off=0x8403c0 val=0x00000001  ] SEC2 reset preamble (mirror of :5-11, base 0x840000)
... :742 off=0x840084=0xb72000a1  (FALCON_RM)  :745 off=0x840600=0x115 (TRANSCFG)
:746 off=0x840110 val=0x00cfbc00  # SEC2 DMATRFBASE = Booter code @ 0xcfbc0000
:748 off=0x840114 val=0x00000000  ] first SEC2 IMEM block — NOTE FBOFFS starts at 0x100:
:749 off=0x84011c val=0x00000100  ]   (131 IMEM blocks, then 98 DMEM blocks @0x600)
:1437 off=0x841210 val=0x00000010 # SEC2 BROM_PARAADDR(0) = hsSig @ DMEM 0x10
:1438 off=0x84119c val=0x00000001 # SEC2 BROM_ENGIDMASK
:1439 off=0x841198 val=0x00000003 # SEC2 BROM_CURR_UCODE_ID = 3 (Booter-Load)
:1440 off=0x841180 val=0x00000001 # SEC2 MOD_SEL.ALGO = RSA3K
:1441 off=0x840104 val=0x00000100 # SEC2 BOOTVEC = imemVa (0x100)
:1442 off=0x840040 val=0xc7eda000 # SEC2 MAILBOX0 = phys(GspFwWprMeta) lo
:1443 off=0x840044 val=0x00000000 # SEC2 MAILBOX1 = hi
:1444 off=0x840100 val=0x00000002 # SEC2 CPUCTL.STARTCPU -> run Booter; wait-for-halt (READ)
```

**Phase D — RISC-V-active preamble (`:1446`)**:
```
:1446 off=0x110080 val=0x00000000 # GSP FALCON_OS = appVersion (0); next is RISCV_CPUCTL.ACTIVE_STAT READ
```

**Phase E — host replays GSP-RM CPU sequencer (`:1449-1777`)** — same routine, 3rd invocation,
on GSP, sourced from **FB** (`TRANSCFG=0x114`), then a `CORE_RESUME` tail that re-resets GSP and
restarts SEC2:
```
:1449 off=0x110040 val=0x00000000 # GSP MAILBOX0 (sequencer scratch)
:1450 off=0x1103c0 val=0x00000001 ] GSP reset preamble (mirror of :5-11)
:1459 off=0x110600 val=0x00000114 # FBIF_TRANSCFG(0) = TARGET=0("LOCAL_FB")|PHYSICAL  <- FB, not sysmem
:1460 off=0x110110 val=0x02758410 # DMATRFBASE = GSP-RM image in FB (phys 0x275841000)
...   (64 IMEM blocks @0x614, then 36 DMEM blocks @0x600)
:1764 off=0x111210 val=0x00001f10 # BROM_PARAADDR(0)
:1765 off=0x11119c val=0x00000400 # BROM_ENGIDMASK
:1766 off=0x111198 val=0x00000001 # BROM_CURR_UCODE_ID = 1
:1767 off=0x111180 val=0x00000001 # MOD_SEL.ALGO = RSA3K
:1768 off=0x110040 val=0x000000fe # MAILBOX0 = 0xfe  [UNCERTAIN sentinel — see TODO]
:1769 off=0x110104 val=0x00000100 # BOOTVEC = 0x100
:1770 off=0x110100 val=0x00000002 # CPUCTL.STARTCPU -> run GSP-RM bootloader
:1771 off=0x1103c0 val=0x00000001 ] CORE_RESUME: _kgspResetIntoRiscv (GSP)
:1772 off=0x1103c0 val=0x00000000 ]
:1773 off=0x111668 val=0x00000111 # GSP BCR_CTRL = RISCV|VALID|BRFETCH
:1774 off=0x110040 val=0xc7ed9000 # GSP MAILBOX0 = libos boot-args phys
:1775 off=0x110044 val=0x00000000 # GSP MAILBOX1
:1776 off=0x840130 val=0x00000002 # SEC2 CPUCTL_ALIAS.STARTCPU -> restart SEC2 (ALIAS_EN now set)
:1777 off=0x110080 val=0x00000000 # GSP FALCON_OS = appVersion
```

---

### F04.5 — FWSEC-FRTS: carving WPR2 (stage 05, runs on the GSP Falcon)

[EVIDENCE] FWSEC ("Firmware Security Licensing") is a VBIOS-resident Falcon ucode. RM parses it
from the VBIOS (stage 02), then re-runs it **on the GSP Falcon** to set up the **FRTS**
("FW Runtime Security") region and program the hardware **WPR2** MMU registers around it. It runs
on GSP because `kgspExecuteFwsecFrts_TU102` → `s_executeFwsec_TU102` passes
`staticCast(pKernelGsp, KernelFalcon)` (base `0x110000`) to the HS executor
(`SRC/.../gsp/arch/turing/kernel_gsp_frts_tu102.c:437-438`) — which is why Phase A's writes are
all `0x11xxxx`.

[EVIDENCE] **The FRTS command is delivered in DMEM, not via MMIO.** RM patches FWSEC's
DMEM "application interface" (`DMEMMAPPER`, entry id `0x4`): `init_cmd = 0x15`
(`FALCON_APPLICATION_INTERFACE_DMEM_MAPPER_V3_CMD_FRTS`, `kernel_gsp_frts_tu102.c:101,239-258`;
the SB variant `0x19` is **not** used). The payload `FWSECLIC_FRTS_CMD` carries
`frtsRegionOffset4K = frtsOffset>>12`, `frtsRegionSize = 0x100` (1 MB), `mediaType = FB(2)`
(`:315-319`). `frtsSize` is hard-coded 1 MB on GA10x (`:48-57`); `frtsOffset = gspFwWprEnd −
frtsSize` (`kernel_gsp_tu102.c:572-573`). **Because the command is a memcpy into a sysmem image,
the runtime `frtsOffset` value never appears in the MMIO trace** (TODO-1).

[EVIDENCE] FWSEC post-conditions RM verifies (all **reads**, so absent from the write trace; the
stage-05 marker firing is the trace-level proof they passed) — `kernel_gsp_frts_tu102.c:454-482`:
(a) VBIOS scratch `0x0E` FRTS error code == NONE; (b) `NV_PFB_PRI_MMU_WPR2_ADDR_HI != 0`
(WPR2 now exists); (c) `NV_PFB_PRI_MMU_WPR2_ADDR_LO == frtsOffset >> alignment` (WPR2 starts
exactly where RM asked).

[INFERENCE] "Carving WPR2" = FWSEC (in HS mode on GSP) programs `NV_PFB_PRI_MMU_WPR2_ADDR_LO/HI`
to hardware-protect `[frtsOffset, frtsOffset+1 MB)`. The Booter (F04.6) then authenticates GSP-RM
*into* that protected region.

---

### F04.6 — SEC2 Booter Load: authenticating GSP-RM into WPR2 (stages 06→07)

[EVIDENCE] After reset-into-RISC-V, RM runs the **Booter Load** HS ucode **on SEC2**
(`kgspExecuteBooterLoad_TU102` → `s_executeBooterUcode_TU102` passes
`staticCast(pKernelSec2, KernelFalcon)`, base `0x840000`,
`SRC/.../gsp/arch/turing/kernel_gsp_booter_tu102.c:115-118`) — which is why all 710 `0x84xxxx`
writes occur in Phase C (plus the one trailing SEC2 write in Phase E). The Booter is a signed
(PKC/RSA3K) heavy-secure ucode that reads `GspFwWprMeta`, verifies the GSP-RM image, and
unlocks/populates WPR2 so the GSP RISC-V core can execute GSP-RM.

[EVIDENCE] **Argument:** `MAILBOX0/1 = memdescGetPhysAddr(pKernelGsp->pWprMetaDescriptor)`
(`kernel_gsp_ga102.c:244-245`; `kernel_gsp_booter_tu102.c:106-107`). Trace: SEC2
`0x840040=0xc7eda000`, `0x840044=0` (`:1442-1443`) ⇒ WPR-meta phys `0x0000_0000_c7eda000`.
**Success = MAILBOX0 reads back 0** after halt (`kernel_gsp_booter_tu102.c:76-80`).

[EVIDENCE] **PKC / Heavy-Secure auth:** before STARTCPU, RM writes the SEC2 **BROM** registers so
SEC2 hardware (not RM) checks the RSA3K signature (`kernel_gsp_falcon_ga102.c:203-211`): trace
`0x841210=0x10` (sig @ DMEM 0x10), `0x84119c=0x1` (engineIdMask), `0x841198=0x3` (ucodeId=Booter),
`0x841180=0x1` (MOD_SEL=RSA3K) (`:1437-1440`). The Booter ucode is a `BOOT_FROM_HS` image kept in
**sysmem** (asserted `kernel_gsp_falcon_ga102.c:137-138`); its DMA source base
`0x840110=0x00cfbc00` ⇒ Booter code at sysmem phys `0xcfbc0000` (`:746`).

[INFERENCE] The ~156.6 ms gap 06→07 is the host `kflcnWaitForHalt` on SEC2 while the Booter runs;
after it halts, GSP RISC-V reports active (F04.7) at stage 08.

---

### F04.7 — Reset preamble, GFW_BOOT, and RISC-V detection (the reads)

**GFW_BOOT (stage 03, a read-poll).** [EVIDENCE] Before touching GSP, RM polls that VBIOS devinit
(FWSEC-from-ROM) finished: `kgspWaitForGfwBootOk_TU102` → `_gpuIsGfwBootCompleted_TU102`
(`SRC/.../gpu/arch/turing/kern_gpu_tu102.c:322-370`):
1. read `NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK`, require
   `_READ_PROTECTION_LEVEL0==_ENABLE` (proves FWSEC lowered its PLM) (`:339,347-348`);
2. read `NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT`, test `_PROGRESS==_COMPLETED`
   (`=0xFF`) (`:362,368-369`; defs `dev_gc6_island_addendum.h:30-32`).

Timeout = `FWSECLIC_PROG_START_TIMEOUT(50ms) + FWSECLIC_PROG_COMPLETE_TIMEOUT(2s)` = **2.05 s**
(`kern_gpu_tu102.c:372-375`). These are `NV_PGC6` scratch **reads**, not in the write trace — but
StelluxOS **must** pass this gate before loading GSP-RM, else FB size / WPR layout are unknown.

**Reset preamble (`kflcnReset`).** [EVIDENCE] `kflcnReset_TU102 = kflcnEnable(FALSE) +
kflcnEnable(TRUE)` (`kernel_falcon_tu102.c:155-164`); with `pmcEnableMask==0` each enable does a
secure reset that toggles `FALCON_ENGINE.RESET` and writes `BCR_CTRL=CORE_SELECT_FALCON(0)` via
`kflcnSwitchToFalcon_GA10X` (`kernel_falcon_ga102.c:84-104`), and the TRUE pass writes
`FALCON_RM=chipId0` (`kernel_falcon_tu102.c:233-234`). Trace `:5-11` (GSP) / `:736-742` (SEC2).

[EVIDENCE] Each secure reset also performs three **read-polls** that are absent from the write
trace but required to reproduce the boot: (a) `HWCFG2._RESET_READY` (bit 31) before the toggle
(`kflcnPreResetWait_GA10X`, `kernel_falcon_ga102.c:138-179`; best-effort, has a timeout —
"sometimes may not get set by HW"); (b) `HWCFG2._MEM_SCRUBBING==_DONE` (bit 12 → 0) after the
toggle — **wait for IMEM/DMEM scrubbing to finish before any DMA** (`kflcnWaitForResetToFinish_GA102`
via `_kflcnWaitForScrubbingToFinish`, `kernel_falcon_ga102.c:190-219`; "avoids pri timeouts"); and
(c) `BCR_CTRL._VALID` (bit 0) after the `BCR_CTRL→FALCON` write (`kflcnSwitchToFalcon_GA10X`,
`:106-123`). The single-toggle `_kgspResetIntoRiscv` reset (Phase B / CORE_RESUME) likewise calls
`kflcnPreResetWait` + `kflcnWaitForResetToFinish` (`kernel_gsp_ga102.c:98,116`) around its one
`FALCON_ENGINE.RESET` toggle before `BCR_CTRL=0x111`. These reads are why the `flcn_reset`
pseudocode (F04.2b) includes `poll_until(...)` lines.

**Reset-into-RISC-V (stage 06).** [EVIDENCE] `kflcnRiscvProgramBcr_GA102` composes
`BCR_CTRL = CORE_SELECT_RISCV(0x10) | VALID(0x1) | BRFETCH(0x100)` = **`0x111`**
(`kernel_falcon_ga102.c:64-78`; fields `dev_riscv_pri.h:33-42`). Trace `0x111668=0x111` (`:730`).
Then the host primes RPC: `QUEUE_HEAD(0)=0` + GSP `MAILBOX0/1`=libos boot-args phys (`:732-735`).

**"RISC-V active" (stage 08, a read).** [EVIDENCE] After Booter, `kernel_gsp_ga102.c:256` writes
`FALCON_OS=appVersion` (trace `0x110080=0`, `:1446`), then **reads** `kflcnIsRiscvActive_GA10X`,
which tests `NV_PRISCV_RISCV_CPUCTL._ACTIVE_STAT==_ACTIVE` (bit7) at `0x111388`
(`kernel_falcon_ga102.c:44-54`; `dev_riscv_pri.h:30-32`). So stage 08 emits exactly the one
`FALCON_OS` write before a read — confirming detection is poll-by-read.

**RISC-V present (precondition, stage 04).** [EVIDENCE] `kflcnIsRiscvCpuEnabled` reads
`HWCFG2._RISCV==_ENABLE` (bit10, `0x1100f4`) (`kernel_falcon_tu102.c:124-133`; called
`kernel_gsp_ga102.c:181`). Read-only.

---

### F04.8 — The GSP CPU sequencer during the INIT_DONE wait (stage 09→10)

[EVIDENCE] `kgspWaitForRmInitDone` only calls `rpcRecvPoll(pGpu, pRpc, GSP_INIT_DONE)`
(`kernel_gsp.c:3787-3796`). During that poll, one inbound RPC is `run_cpu_sequencer`, handled by
`kgspExecuteSequencerBuffer_IMPL` (`kernel_gsp.c:3819`): the opcode
`GSP_SEQ_BUF_OPCODE_REG_WRITE` does `GPU_REG_WR32(addr,val)` (`:3854-3860`) — which is why these
appear as `RVGREG` writes — plus `REG_MODIFY/REG_POLL/DELAY_US/CORE_RESET/CORE_START/
CORE_WAIT_FOR_HALT` (`:3864-3949`), and the arch-specific `CORE_RESUME`
(`kgspExecuteSequencerCommand_GA102`, `kernel_gsp_ga102.c:340-389`), which does
`_kgspResetIntoRiscv` + program libos args + `kflcnStartCpu(SEC2)`.

[EVIDENCE] The `CORE_RESUME` tail (`:1771-1777`) is an exact register match: GSP `FALCON_ENGINE`
reset (`:1771-1772`) + `BCR_CTRL=0x111` (`:1773`) = `_kgspResetIntoRiscv`; GSP `MAILBOX0/1`=
`0xc7ed9000`/0 (`:1774-1775`) = `kgspProgramLibosBootArgsAddr`; **SEC2** `CPUCTL_ALIAS=0x2`
(`:1776`) = `kflcnStartCpu(pKernelSec2Falcon)` (via alias because the authenticated Booter set
`CPUCTL.ALIAS_EN`); GSP `FALCON_OS=0` (`:1777`). Only `CORE_RESUME` is implemented in the kernel
client; other opcodes return `NV_ERR_INVALID_ARGUMENT` (`kernel_gsp_ga102.c:391-394`).

[INFERENCE — sequencer opcode list] Writes `:1449-1777` = the host executing GSP-RM's sequencer
buffer: a third full Falcon DMA-load-and-go on GSP (reset → `DMATRFBASE=0x02758410` from FB →
256B loop → BROM → `BOOTVEC=0x100` → `CPUCTL=0x2`), then the `CORE_RESUME` tail. **The exact
opcode stream is INFERENCE** (reasoned from the source path + the register pattern + timestamp
placement) — the raw sequencer-buffer bytes are expanded inline by the host and are **not** in
any captured artifact. **This is the section's one TODO requiring a new trace point** (TODO-1).

[EVIDENCE — timing] stage 09 `:1448`=4604.454397; first Phase-E write `:1449`=4605.574752; last
Phase-E write `:1777`=4605.583535; stage 10 `:1778`=4605.792726. ⇒ ~1120 ms GSP-RM-internal idle,
then ~8.8 ms of writes, then ~209 ms to INIT_DONE.

---

### F04.9 — DMA block accounting + timing (independently reproduced) [EVIDENCE]

Per-phase per-engine inner-loop counts (each = `MOFFS`+`FBOFFS`+`CMD` triplet) and IMEM/DMEM split
by `DMATRFCMD` value. **All numbers below were re-counted from `CAP/boot-trace.txt`** (and match
`CAP1`):

| Phase | engine | IMEM blk (`0x614`) | DMEM blk (`0x600`) | kicks | ×3 loop wr | control wr | phase total |
|---|---|---|---|---|---|---|---|
| A FWSEC | GSP | 226 | 8 | 234 | 702 | 20 | 722 |
| C Booter | SEC2 | 131 | 98 | 229 | 687 | 22 (+4 GSP prime) | 713 |
| E sequencer | GSP (+1 SEC2) | 64 | 36 | 100 | 300 | 29 | 329 |
| B reset / D OS | GSP | — | — | — | — | 3 + 1 | 4 |
| **totals** | | **421** | **142** | **563** | **1689** | **79** | **1768** |

Command-qualified verification (the source of the S04.8 correction):
- `off=0x110118 val=0x614` = 290, `off=0x110118 val=0x600` = 44 → **GSP DMATRFCMD 334** ✓
- `off=0x840118 val=0x614` = 131, `off=0x840118 val=0x600` = 98 → **SEC2 DMATRFCMD 229** ✓
- IMEM kicks = 290+131 = **421**; DMEM **command** kicks = 44+98 = **142**.
- ⚠ A naive `val=0x00000600` count = **154** (both captures). The extra **12** are not commands:
  they are `DMATRFMOFFS`/`DMATRFFBOFFS` writes whose per-block offset equals `0x600` — one MOFFS +
  one FBOFFS in each of the 6 DMA segments that reach offset `0x600` (6×2=12). **142 + 12 = 154.**
  The DMA *command* count is **142** (this corrects S04.8's "154").

[INFERENCE] Segment byte sizes from `DMATRFBASE` deltas (`base<<8`), confirming block counts:
FWSEC IMEM `0xcfbde2−0xcfbd00=0xe2 → 0xe200` = 57.9 KB (226×256, FBOFFS from 0x0); Booter IMEM
`0xcfbc84−0xcfbc00=0x84 → 0x8400`, header 0x100 → 131×256 (FBOFFS from **0x100**); sequencer IMEM
`0x02758451−0x02758410=0x41 → 0x4100`, header 0x100 → 64×256. The FWSEC-vs-Booter FBOFFS start
difference (0x0 vs 0x100) is why block counts are not derivable from base-deltas alone — the
measured counts are authoritative.

**Stage timing.** [EVIDENCE] Two captures, same code path, different wall clocks:

| Δ | CAP (115116) ms | CAP1 (112551) ms | note |
|---|---|---|---|
| 01→02 | 163.7 | 179.8 | VBIOS extract + FWSEC parse |
| 02→03 | 16.9 | 16.6 | GFW_BOOT poll |
| 04→05 | 212.2 | 212.5 | FWSEC-FRTS DMA-load+run (A) + wait-for-halt |
| 06→07 | 156.6 | 156.5 | Booter-Load on SEC2 (C) + wait-for-halt |
| 08→09 | 135.7 | 134.9 | status-queue init |
| 09→10 | 1338.3 | 1360.6 | GSP-RM init (RPC + sequencer replay); dominant |
| **01→10** | **~2024 (~2.02 s)** | **~2061 (~2.06 s)** | cf. CONTEXT_BRIEF §4a "~2.07 s" |

[INFERENCE] The ~1768 MMIO writes themselves take <30 ms of wall time; the critical path is on-GPU
compute (FWSEC ~212 ms, Booter ~156 ms, GSP-RM internal init ~1.1–1.36 s).

---

### F04.10 — Chip contrast (why this is the GA102 path)

- **GA102 (our card)** uses the `DMATRF*` Falcon DMA engine for HS load (everything above), with
  BROM/RSA3K and `bBootFromHs=NV_TRUE`. [EVIDENCE: `kernel_sec2_ga102.c:46-49`,
  `kgspExecuteHsFalcon_GA102`.]
- **Turing (TU10x)** has no Falcon DMA engine for HS load: `kgspExecuteHsFalcon_TU102`
  (`kernel_gsp_falcon_tu102.c:308`, asserts `!bBootFromHs` at `:324`) copies via `IMEMC/DMEMC`
  windows and writes **no** `0x841xxx` BROM regs. **Skip on GA102.** [EVIDENCE/INFERENCE]
- **Hopper (GH100)** uses **no SEC2 booter and no FWSEC-FRTS**; it boots a GSP-FMC via the **FSP**
  (`kernel_gsp_gh100.c:237,385,442`). **Wrong chip — not relevant.** [EVIDENCE]

---

### Minimal-path notes (essential vs skippable for first pixel on GA102)

**Essential — implement `flcn_reset(base)` + `flcn_hs_load_and_go(base,…)` ONCE, then drive in
order:**
1. Poll `GFW_BOOT.PROGRESS==COMPLETED(0xFF)` (F04.7) before touching GSP — gates FB/WPR2 layout.
2. **FWSEC-FRTS on GSP** (`TRANSCFG=0x115` sysmem, ucodeId 9): carve WPR2; verify `WPR2_ADDR_HI!=0`
   and `WPR2_ADDR_LO==frtsOffset>>align` (reads). FRTS cmd (`init_cmd=0x15`, region 1 MB, media FB)
   goes in DMEM, not MMIO (F04.5).
3. **Reset-into-RISC-V** on GSP: `BCR_CTRL=0x111` (F04.7).
4. **Booter Load on SEC2** (`MAILBOX0/1=phys(GspFwWprMeta)`, ucodeId 3): DMA the signed Booter from
   sysmem, BROM quartet, STARTCPU; require `MAILBOX0==0` (F04.6). Then confirm
   `RISCV_CPUCTL.ACTIVE_STAT` (read).
5. **Service the CPU sequencer** during the INIT_DONE wait (F04.8): implement at least
   `REG_WRITE` + `CORE_RESUME` (3rd DMA-load-and-go from FB on GSP + `kflcnStartCpu(SEC2)` via
   `CPUCTL_ALIAS`). GSP-RM will not reach INIT_DONE without it.

**The 22 offsets in F04.3 are the complete register surface.** Get `DMATRFCMD=0x614/0x600`, the
IDLE poll, the BROM quartet, `BOOTVEC`, `BCR_CTRL`, and `CPUCTL`-vs-`CPUCTL_ALIAS` exactly right;
the other 1689 writes are mechanical repetition of three loop registers.

**Reuse, don't re-author:** FWSEC (VBIOS-resident) and Booter (bindata, shipped with
535.183.01 `gsp_ga10x.bin`) are signed NVIDIA blobs; the BROM does the crypto. Carry the images
as-is — never alter signatures.

**Skippable / not on this path:** the TU102 `IMEMC/DMEMC` window copy; the Scrubber ucode (gated
`if pScrubberUcode != NULL`, not fired here — `kernel_gsp_ga102.c:234-239`); CrashCat queue setup;
Confidential-Compute branches; Booter Unload (teardown/GC6/SR only); the FWSEC SB cmd `0x19`;
debug-signed (`*_dbg`) ucode; the Hopper FSP/GSP-FMC path.

---

### Evidence cited

**Trace — `CAP/boot-trace.txt` (`CAP=traces/20260530-115116-open-capture`; same line numbers in
`CAP1=…112551`):**
- Markers L1,2,3,4,727,731,1445,1447,1448,1778 (verified at exact lines).
- Phase A L5–16 (reset/aperture), L17–19 (first IMEM block), L719–726 (DMEM tail + BROM + BOOTVEC
  + STARTCPU); Phase B L728–730; Phase C L732–735 (GSP prime), L736–749 (SEC2 preamble, FBOFFS
  starts 0x100), L1437–1444 (BROM/MAILBOX/STARTCPU); Phase D L1446; Phase E L1449–1461, L1654,
  L1759–1777 (CORE_RESUME tail).
- Counts (re-measured): `off=0x11*`=1058, `off=0x84*`=710; `off=0x110118`=334, `off=0x840118`=229;
  `off=0x110118 val=0x614`=290, `val=0x600`=44; `off=0x840118 val=0x614`=131, `val=0x600`=98;
  `val=0x614`(all)=421, `val=0x600`(all lines)=154 → DMEM **commands**=142; 41 distinct offsets
  (21 GSP + 20 SEC2); 1768 total writes.
- Phase-E timestamps: stage09 4604.454397, first seq write 4605.574752, last 4605.583535,
  stage10 4605.792726.

**Source — `SRC=open-gpu-kernel-modules`:**
- `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c:2682,2704,2727-2751,2832,2848-2861,3787-3796,3819,3854-3949`
- `src/nvidia/src/kernel/gpu/gsp/arch/ampere/kernel_gsp_ga102.c:58-64,91-121 (_kgspResetIntoRiscv: PreResetWait :98, WaitForResetToFinish :116),164,181,187,189-200,206,234-239,244-245,256,259,274-281,285,340-389,391-394`
- `src/nvidia/src/kernel/gpu/gsp/arch/ampere/kernel_gsp_falcon_ga102.c:38-92 (s_dmaTransfer: 55-56,60-67,70-84), 110-236 (DisableCtxReq 140, TRANSCFG RMW 143-146, IMEM cmd 150-156, DMEM cmd 173-178, BROM 203-211, BOOTVEC 215, MAILBOX 218-221, StartCpu 224, halt+read 227-233), 137-138 (sysmem assert)`
- `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_frts_tu102.c:48-57,101,239-258,315-319,437-438,454-482`
- `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c:572-573`
- `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_booter_tu102.c:76-80,106-107,115-118`
- `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_falcon_tu102.c:308,324`
- `src/nvidia/src/kernel/gpu/gsp/arch/hopper/kernel_gsp_gh100.c:237,385,442`
- `src/nvidia/src/kernel/gpu/falcon/arch/ampere/kernel_falcon_ga102.c:44-54,64-78,84-129 (SwitchToFalcon+VALID poll),138-179 (PreResetWait/RESET_READY),190-219 (WaitForResetToFinish/MEM_SCRUBBING)`
- `src/nvidia/src/kernel/gpu/falcon/arch/turing/kernel_falcon_tu102.c:124-133,155-164,233-234,242-255,260-277`
- `src/nvidia/src/kernel/gpu/arch/turing/kern_gpu_tu102.c:322-370,372-375`
- `src/nvidia/src/kernel/gpu/sec2/arch/ampere/kernel_sec2_ga102.c:46-49`
- `src/nvidia/inc/kernel/gpu/falcon/falcon_common.h:29,54`

**Register headers — `SRC/src/common/inc/swref/published/ampere/ga102/`:**
- `dev_falcon_v4.h`: MAILBOX0/1 :44-45; DMACTL :46; DMATRFBASE :53; DMATRFMOFFS :55; DMATRFCMD :57
  + fields IDLE :60-61, SEC :62, IMEM :63-65, WRITE :66-68, SIZE :69, SIZE_256B :70, SET_DMTAG
  :72-73; DMATRFFBOFFS :74; DMATRFBASE1 :76; HWCFG2 :99, RISCV :100-101, MEM_SCRUBBING :102-103;
  OS :104; RM :105; CPUCTL :107,
  STARTCPU :108-109, ALIAS_EN :113-114; CPUCTL_ALIAS :116-118; BOOTVEC :120.
- `dev_gsp.h`: NV_PGSP :26; FALCON_MAILBOX0/1 :27,29; FALCON_ENGINE :31, RESET :32-34;
  QUEUE_HEAD(i) :38.
- `dev_sec_pri.h`: NV_PSEC :27; FALCON_ENGINE :28, RESET :29-31.
- `dev_riscv_pri.h`: NV_FALCON2_GSP_BASE :27; RISCV_CPUCTL :30, ACTIVE_STAT :31-32; BCR_CTRL :33,
  VALID :34-36, CORE_SELECT :37, _FALCON :38, _RISCV :39, BRFETCH :40-42.
- `dev_falcon_second_pri.h`: NV_FALCON2_GSP_BASE :26, NV_FALCON2_SEC_BASE :28; MOD_SEL :29, ALGO
  :30, RSA3K :31; BROM_CURR_UCODE_ID :32-33; BROM_ENGIDMASK :34; BROM_PARAADDR(i) :35.
- `dev_fbif_v4.h`: TRANSCFG(i) :27, TARGET :29, _COHERENT_SYSMEM :30, MEM_TYPE :31, _PHYSICAL :32;
  FBIF_CTL :33, ALLOW_PHYS_NO_CTX :34-35.
- `dev_falcon_v4_addendum.h`: HWCFG2_RESET_READY :27-29 (bit 31:31).
- `dev_gc6_island_addendum.h`: GFW_BOOT :30, PROGRESS :31, _COMPLETED(0xFF) :32.
- `dev_gsp_addendum.h:26` (NV_PGSP_FBIF_BASE=0x110600); `dev_sec_addendum.h:26`
  (NV_PSEC_FBIF_BASE=0x840600).

(Note: S04 cited `turing/tu102/dev_gsp.h` for `NV_PGSP*` — byte-identical to the ampere/ga102 copy
cited here.)

---

### Open questions / TODO

- **TODO-1 (sequencer opcode stream — the one INFERENCE in the boot path):** the exact opcode list
  for `:1449-1777` (F04.8) is reasoned from source + register pattern + timing, **not** captured.
  Add an `RVGSEQ` trace point in `kgspExecuteSequencerBuffer_IMPL` (`kernel_gsp.c:3819`) dumping
  opcode+args, and re-capture, to upgrade F04.8 from [INFERENCE] to [EVIDENCE]. Same trace would
  resolve the `MAILBOX0=0xfe` sentinel (`:1768`, [UNCERTAIN] — likely a sequencer-provided value).
- **TODO-2 (`frtsOffset` exact value):** not in the write trace (delivered via FWSEC DMEM cmd
  buffer, F04.5). Capture via the `#if 0` `GspFwWprMeta` dump (`kernel_gsp_tu102.c`) or by reading
  `NV_PFB_PRI_MMU_WPR2_ADDR_LO` after stage 05. Expected `frtsOffset = gspFwWprEnd − 0x100000`.
- **TODO-3 (TRANSCFG/FBIF_CTL residual `0x110`, and "LOCAL_FB" name):** bits 4 and 8 in
  `0x115`/`0x114`/`0x190` are unnamed in the open `dev_fbif_v4.h`; confirm they are HW-init defaults
  preserved by the RMW (read `FB+0x600`/`FB+0x624` before stage 04). Likewise `TARGET=0`="LOCAL_FB"
  is the NV convention, not a published symbol — both currently [INFERENCE]. Does not affect
  reproduction (RMW preserves them; TARGET=0 is the default).
- **TODO-4 (`chipId0`):** `FALCON_RM=0xb72000a1` (`:11`,`:742`) is asserted to be `pGpu->chipId0`;
  independently confirm by reading `NV_PMC_BOOT_0` on the card.
- **TODO-5 (`FALCON_OS` appVersion):** trace shows GSP `0x110080=0` at both stage-07 follow-up
  (`kernel_gsp_ga102.c:256`) and post-resume (`:377`); confirm `pRiscvDesc->appVersion==0` for this
  `gsp_ga10x.bin` rather than a missing value.
- **TODO-6 (WPR-meta address space):** `MAILBOX0=0xc7eda000` is documented "in SYSTEM"
  (`kernel_gsp_booter_tu102.c:103`) and matches the sysmem Booter image base; confirm
  `pWprMetaDescriptor` is `ADDR_SYSMEM` on GA102 to state sysmem-vs-FB as EVIDENCE.
- **TODO-7 (BROM_PARAADDR per ucode):** FWSEC `0x5a4`, Booter `0x10`, GSP-RM `0x1f10` are per-ucode
  `hsSigDmemAddr` from the signed image descriptor — carry them from the image, do not invent.


## F05 — GSP RPC Transport

Final, judge-verified merge of **S06** (RPC infrastructure: semantics/API) and **D02**
(byte-replicable queue/header layout), reconciled against the live source tree and the
gap-fill capture. This is the complete CPU↔GSP RPC transport StelluxOS must build *after*
the GSP RISC-V is alive (GSP boot = F0x/S05 scope) and *before* the object/control/alloc
payload layer (F06/S07). Where a value lets an implementer "byte-match" GSP-RM, it is given
to the byte. Every non-trivial claim is labelled **[EVIDENCE]** (cited file:line),
**[INFERENCE]** (reasoned — from what), or **[TODO/UNCERTAIN]**.

Path shorthands (CONTEXT_BRIEF §5, CONTEXT_ADDENDUM):
`SRC = /home/flare/dev/gpu-repro/open-gpu-kernel-modules` (open-gpu-kernel-modules @ 535.183.01),
`CAP2 = /home/flare/dev/gpu-repro/traces/20260530-115116-open-capture` (full-modeset gap-fill
capture: `rpc-resp-trace.txt` reply dwords + `boot-trace.txt` doorbell writes).
ABI: LP64, natural alignment, no packing pragmas — `NvU8=1`, `NvU32=NvHandle=4`,
`NvU64=RmPhysAddr=8`.

> **Verification status (this revision).** Every layout/value below was re-checked against
> the cited source on disk and the live trace. Two substantive defects in the S06 draft were
> **corrected** (header_version value; rpc.c line citations) and the RPC counts were
> reconciled per CORRECTIONS #2. See **§17 Corrections applied** for the exact diffs.

---

### 1. Where the transport sits / object model

[EVIDENCE] The GSP client wires two HAL function pointers that the generic RPC layer calls;
the base `_IMPL` versions are deliberate stubs returning `NV_ERR_NOT_SUPPORTED`, so an
implementer **must** supply the GSP send/recv bodies — there is no logic to inherit:

```138:148:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia/kernel/vgpu/nv/rpc.c
NV_STATUS rpcSendMessage_IMPL(OBJGPU *pGpu, OBJRPC *pRpc)
{
    NV_PRINTF(LEVEL_ERROR, "virtual function not implemented.\n");
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS rpcRecvPoll_IMPL(OBJGPU *pGpu, OBJRPC *pRpc, NvU32 expectedFunc)
{
    NV_PRINTF(LEVEL_ERROR, "virtual function not implemented.\n");
    return NV_ERR_NOT_SUPPORTED;
}
```

[EVIDENCE] For GSP the fnptrs are bound to `_kgspRpcSendMessage` / `_kgspRpcRecvPoll` and the
OBJRPC is told its staging buffer + max size in `_kgspConstructRpcObject`
(`SRC/.../gsp/kernel_gsp.c:1995-1998`): `pRpc->maxRpcSize = GSP_MSG_QUEUE_RPC_SIZE_MAX`
(`:1995`), `rpcSendMessage_FNPTR(pRpc) = _kgspRpcSendMessage` (`:1997`).

[EVIDENCE] `pRpc->message_buffer` is **exactly the `rpc` sub-field of the working queue
element** (not the ring): `pMQI->pRpcMsgBuf = &pMQI->pCmdQueueElement->rpc`
(`message_queue_cpu.c:184`). Accessor macros used throughout:
- `RPC_HDR == (rpc_message_header_v*)(pRpc->message_buffer)` (`kernel_gsp.c:82`).
- `vgpu_rpc_message_header_v == (rpc_message_header_v*)(pRpc->message_buffer)`,
  `rpc_message == vgpu_rpc_message_header_v->rpc_message_data` (`SRC/.../inc/objrpc.h:98-99`).

[EVIDENCE] CPU API surface (all that StelluxOS must re-implement),
`SRC/src/nvidia/inc/kernel/gpu/gsp/message_queue.h:39-43`:
`GspMsgQueuesInit`, `GspMsgQueuesCleanup`, `GspStatusQueueInit`, `GspMsgQueueSendCommand`,
`GspMsgQueueReceiveStatus`.

---

### 2. Compile-time constants (everything derives from these)

[EVIDENCE] All from `message_queue_priv.h:91-104`, `message_queue_cpu.c:73-74`,
`msgq.h:31,39`, `msgq_priv.h:38`, `rm_page_size.h:38,41,42`.

| Constant | Value | Source |
|---|---|---|
| `RM_PAGE_SIZE` | `4096` (0x1000) | rm_page_size.h:38 |
| `RM_PAGE_MASK` | `0x0FFF` | rm_page_size.h:41 |
| `RM_PAGE_SHIFT` | `12` | rm_page_size.h:42 |
| `GSP_MSG_QUEUE_ELEMENT_SIZE_MIN` | `4096` (= RM_PAGE_SIZE) | priv.h:91 |
| `GSP_MSG_QUEUE_ELEMENT_SIZE_MAX` | `65536` (0x10000 = 16×MIN) | priv.h:92 |
| `GSP_MSG_QUEUE_ELEMENT_HDR_SIZE` | `48` (= `offsetof(GSP_MSG_QUEUE_ELEMENT, rpc)`) | priv.h:93 (computed §5) |
| `GSP_MSG_QUEUE_RPC_SIZE_MAX` (= `maxRpcSize`) | `65488` (0x10000 − 48) | priv.h:95-96 |
| `GSP_MSG_QUEUE_HEADER_SIZE` | `4096` (= RM_PAGE_SIZE) | priv.h:103 |
| `GSP_MSG_QUEUE_HEADER_ALIGN` | `4` ⇒ align `1<<4 = 16` | priv.h:104 |
| `GSP_MSG_QUEUE_ELEMENT_ALIGN` | `12` ⇒ align `1<<12 = 4096` | priv.h:102 |
| `GSP_MSG_QUEUE_ALIGN` | `12` ⇒ align `4096` | priv.h:101 |
| `defaultCommandQueueSize` | `0x40000` (256 KB) | message_queue_cpu.c:73 |
| `defaultStatusQueueSize` | `0x40000` (256 KB) | message_queue_cpu.c:74 |
| `MSGQ_VERSION` | `0` | msgq_priv.h:38 |
| `MSGQ_MSG_SIZE_MIN` | `16` | msgq.h:31 |
| `MSGQ_FLAGS_SWAP_RX` | `1` | msgq.h:39 |
| `RPC_TASK_RM_QUEUE_IDX` / `RPC_TASK_ISR_QUEUE_IDX` / `RPC_QUEUE_COUNT` | `0` / `1` / `2` | message_queue.h:31-33 |

[EVIDENCE] On silicon the command queue is the 256 KB default; the `×6` branch is
pre-silicon only (`message_queue_cpu.c:78-89`). The status queue is 256 KB unless overridden
by `NV_REG_STR_RM_GSP_STATUS_QUEUE_SIZE` (`:92-101`). **First-pixel: 0x40000 / 0x40000.**

---

### 3. Shared-memory block: software page table + two SPSC rings

[EVIDENCE] Two logical queues per direction live in **one** non-contiguous, cached,
**unprotected** sysmem allocation: a page table, then the RM command queue (CPU→GSP), then
the RM status queue (GSP→CPU). The block is kernel-mapped writeable and fully zeroed before
use (`message_queue_cpu.c:240-280`). Sub-region order is fixed and page-aligned:

```284:307:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c
    // Shared memory layout.
    //
    // Each of the following are page aligned:
    //   Shared memory layout header (includes page table)
    //   RM Command queue header
    //   RM Command queue entries
    //   RM Status queue header
    //   RM Status queue entries
    ...
    pRmQueueInfo->pCommandQueue = NvP64_VALUE(
        NvP64_PLUS_OFFSET(pVaKernel, pMQCollection->pageTableSize));

    pRmQueueInfo->pStatusQueue  = NvP64_VALUE(
        NvP64_PLUS_OFFSET(NV_PTR_TO_NvP64(pRmQueueInfo->pCommandQueue), pRmQueueInfo->commandQueueSize));
```

#### 3a. Page-table sizing (RM-only, TaskIsr disabled) — deterministic arithmetic

[EVIDENCE] Computed in `_getMsgQueueParams` (`message_queue_cpu.c:116-133`): `queueSize` =
sum of the four queue sizes; `numPtes = queueSize >> RM_PAGE_SHIFT`, then bumped by the pages
needed to hold the PTEs themselves; `pageTableSize = RM_PAGE_ALIGN_UP(numPtes * 8)`.

[INFERENCE — arithmetic from that code with §2 constants; values are deterministic]:

| Quantity | Expression | Value |
|---|---|---|
| `queueSize` | 0x40000 + 0x40000 (+0+0) | `0x80000` (524288) |
| `numPtes` (queues) | 0x80000 >> 12 | `128` |
| `numPtes` (+self) | 128 + ⌈128×8 / 4096⌉ = 128 + 1 | `129` |
| `pageTableSize` | `RM_PAGE_ALIGN_UP(129×8 = 1032)` | `0x1000` (one page) |
| `pageTableEntryCount` | — | `129` |
| `sharedBufSize` | 0x1000 + 0x40000 + 0x40000 | `0x81000` (528384 = 129 pages) |

[EVIDENCE] The page table is an array of `RmPhysAddr` (NvU64) filled by
`memdescGetPhysAddrs(..., offset 0, stride RM_PAGE_SIZE, count 129, pPageTbl)`
(`message_queue_cpu.c:296-301`): `pPageTbl[i]` = physical address of page `i` of the
(non-contiguous) block. **The single PA handed to GSP is the first PTE:**

```338:338:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c
    pMQCollection->sharedMemPA  = pPageTbl[0];
```

[INFERENCE, from `:282`+`:296-301`+`:338`] `pPageTbl = pVaKernel` (page 0), so `pPageTbl[0]`
is the physical address **of the page-table page itself**. GSP receives only `sharedMemPA`,
reads the 129-entry PTE array from it, and reconstructs every queue page. Each PTE is a raw RM
physical address (AT_GPU translation) — **not** an arch MMU PTE with permission bits; bytes
1032..4095 of the page-table page stay zero.
[TODO] *Which* boot-arg field carries `sharedMemPA` into GSP is F0x/S05 scope (see §18).

#### 3b. Full byte map of the shared block (RM-only, Confidential-Compute off)

[INFERENCE, composed from §2 constants + `:303-307` + §4 header offsets — deterministic]:

| Block offset | Region | Size | Written by |
|---|---|---|---|
| `0x00000` | Page table (129 × 8-byte PTEs; PTE[0]=`sharedMemPA`) | `0x1000` | CPU |
| `0x01000` | **RM command queue** (CPU→GSP), total | `0x40000` | — |
| `0x01000` | · cmd `msgqTxHeader` (`writePtr` @ `0x01010`) | 32 B | CPU |
| `0x01020` | · cmd `msgqRxHeader.readPtr` (SWAP: CPU's *status* read cursor) | 4 B | CPU |
| `0x02000` | · cmd ring entries: **63** slots × 4096 B | `0x3F000` | CPU |
| `0x41000` | **RM status queue** (GSP→CPU), total | `0x40000` | — |
| `0x41000` | · status `msgqTxHeader` (`writePtr` @ `0x41010`) | 32 B | GSP |
| `0x41020` | · status `msgqRxHeader.readPtr` (SWAP: GSP's *cmd* read cursor) | 4 B | GSP |
| `0x42000` | · status ring entries: **63** slots × 4096 B | `0x3F000` | GSP |
| `0x81000` | end | — | — |

The `0x40000` queue size **includes** its header page, so each ring holds **63 (not 64)**
entries (§4).

---

### 4. The `msgq` ring headers (TX/RX) — struct + exact CPU-written values

[EVIDENCE] Each queue backing store begins with a TX header (written by the producer) then a
single-dword RX header (written by the consumer), then the entry ring
(`msgq_priv.h:40-65`):

```48:65:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/common/shared/msgq/inc/msgq/msgq_priv.h
// buffer metadata, written by source, at start of block
typedef struct
{
    NvU32 version;   // queue version
    NvU32 size;      // bytes, page aligned
    NvU32 msgSize;   // entry size, bytes, must be power-of-2, 16 is minimum
    NvU32 msgCount;  // number of entries in queue
    NvU32 writePtr;  // message id of next slot
    NvU32 flags;     // if set it means "i want to swap RX"
    NvU32 rxHdrOff;  // Offset of msgqRxHeader from start of backing store.
    NvU32 entryOff;  // Offset of entries from start of backing store.
} msgqTxHeader;

// buffer metadata, written by sink
typedef struct
{
    NvU32 readPtr; // message id of last message read
} msgqRxHeader;
```

[EVIDENCE] `msgqTxCreate` computes the header fields (`msgq.c:226-241`) from the args the CPU
passes for the command queue — `size=0x40000, msgSize=4096, hdrAlign=4, entryAlign=12,
flags=SWAP_RX` (`message_queue_cpu.c:170-176`). Resulting **exact dword values**:

| Field | Off | Command-queue value | Derivation (msgq.c) |
|---|---|---|---|
| `version` | 0 | `0` | `=MSGQ_VERSION` (:236) |
| `size` | 4 | `0x40000` | `=size` (:237) |
| `msgSize` | 8 | `0x1000` (4096) | `=msgSize` (:238) |
| `msgCount` | 12 | `63` | `(0x40000 − 0x1000)/0x1000` (:241) |
| `writePtr` | 16 (`0x10`) | `0` → advances | init :239; advance :528-535 |
| `flags` | 20 | `1` | `=MSGQ_FLAGS_SWAP_RX` (:240) |
| `rxHdrOff` | 24 | `0x20` (32) | `ALIGN_UP(32,16)` (:226) |
| `entryOff` | 28 | `0x1000` (4096) | `ALIGN_UP(32+4,4096)` (:227-228) |

```226:241:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/common/shared/msgq/msgq.c
    pQueue->tx.rxHdrOff = NV_ALIGN_UP(sizeof(msgqTxHeader), 1 << hdrAlign);
    pQueue->tx.entryOff = NV_ALIGN_UP(pQueue->tx.rxHdrOff + sizeof(msgqRxHeader),
                              1 << entryAlign);
    ...
    pQueue->tx.version  = MSGQ_VERSION;
    pQueue->tx.size     = size;
    pQueue->tx.msgSize  = msgSize;
    pQueue->tx.writePtr = 0;
    pQueue->tx.flags    = flags;
    pQueue->tx.msgCount = (NvU32)((size - pQueue->tx.entryOff) / msgSize);
```

[INFERENCE] The 256 KB **status** queue (created by GSP) must carry identical geometry
(`size=0x40000, msgSize=0x1000, msgCount=63, rxHdrOff=0x20, entryOff=0x1000`): the CPU's
`msgqRxLink` rejects any mismatch (`msgq.c:366-385`), so GSP must replicate these values for
link to succeed.

#### 4a. Pointer ownership under `SWAP_RX` (who writes which dword)

[EVIDENCE] With `MSGQ_FLAGS_SWAP_RX` set on both peers, `rxSwapped` is true and the read
cursors are placed so **each peer only ever writes inside its own producer backing store**
(`msgq.c:254-272`; `pWriteOutgoing=&pOurTxHdr->writePtr` :257; swapped read cursors :262-263):

| Shared dword | Address | Written by | Read by | Meaning |
|---|---|---|---|---|
| command `writePtr` | `cmdQueue + 0x10` | CPU | GSP | CPU producer cursor (commands) |
| status `readPtr`  | `cmdQueue + 0x20` | CPU | GSP | CPU consumer cursor (status) |
| status `writePtr` | `statusQueue + 0x10` | GSP | CPU | GSP producer cursor (status) |
| command `readPtr` | `statusQueue + 0x20` | GSP | CPU | GSP consumer cursor (commands) |

Net effect [INFERENCE]: **the CPU writes only inside the command-queue region; GSP writes only
inside the status-queue region** ⇒ StelluxOS may map the status queue read-only and still
satisfy the protocol.

[EVIDENCE] SPSC discipline: ring is "full one slot before wrap": free =
`readPtr + msgCount − writePtr − 1` (`msgq.c:461`), initial `txFree = msgCount−1 = 62`
(`msgq.c:251`); producer/consumer advances wrap mod `msgCount` (`msgq.c:528-535`, `675-683`).
[EVIDENCE] The CPU installs **no** msgq hook callbacks, so the library uses **direct volatile
pointer** access (`msgq.c:108-136`) — all CPU memory ordering comes from the explicit `port*`
fences in §10/§11, not from msgq.

---

### 5. Per-slot wrapper `GSP_MSG_QUEUE_ELEMENT` (48-byte prefix)

[EVIDENCE] `SRC/src/nvidia/inc/kernel/gpu/gsp/message_queue_priv.h:43-51`:

```43:51:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia/inc/kernel/gpu/gsp/message_queue_priv.h
typedef struct GSP_MSG_QUEUE_ELEMENT
{
    NvU8  authTagBuffer[16];         // Authentication tag buffer.
    NvU8  aadBuffer[16];             // AAD buffer.
    NvU32 checkSum;                  // Set to value needed to make checksum always zero.
    NvU32 seqNum;                    // Sequence number maintained by the message queue.
    NvU32 elemCount;                 // Number of message queue elements this message has.
    NV_DECLARE_ALIGNED(rpc_message_header_v rpc, 8);
} GSP_MSG_QUEUE_ELEMENT;
```

[EVIDENCE struct + INFERENCE offsets — `rpc` is `aligned(8)` so the 44→48 pad is forced ⇒
`GSP_MSG_QUEUE_ELEMENT_HDR_SIZE = 48`, and `sizeof(GSP_MSG_QUEUE_ELEMENT) = 48 + 32 = 80`]:

| Field | Off | Size | Notes |
|---|---|---|---|
| `authTagBuffer[16]` | 0 | 16 | Confidential-Compute only — **zero on GA102** |
| `aadBuffer[16]` | 16 | 16 | CC only — **zero on GA102** |
| `checkSum` | 32 | 4 | 32-bit fold; whole element XORs to 0 |
| `seqNum` | 36 | 4 | `txSeqNum`/`rxSeqNum`, monotonic, msgq-validated |
| `elemCount` | 40 | 4 | # of 4096-byte pages this message spans |
| *(pad)* | 44 | 4 | alignment to 8 |
| `rpc` | **48** | var | the wire message (§6) |

[EVIDENCE] At runtime the CPU fills a **staging copy** in non-paged memory (`pCmdQueueElement`
inside `pWorkArea`; `message_queue_cpu.c:147-160`), and send/recv copy this element to/from the
ring **one 4096-byte page at a time** (`:591-593`, `:677-679`).

[EVIDENCE] Non-CC checksum (the only one on GA102): XOR all 64-bit words over
`[element, element+uElementSize)` (input zero-padded to the next 8 bytes first,
`:513-514`), then fold hi32 ⊕ lo32; `checkSum` is included and pre-zeroed, so the receiver's
recompute must equal 0 (`message_queue_priv.h:112-124`, `message_queue_cpu.c:518,547,710-715`).

---

### 6. The wire header `rpc_message_header_v` (32 bytes) — **header_version corrected**

[EVIDENCE] `SRC/src/nvidia/generated/g_rpc-message-header.h:41-54` (the union `u` is two
`NvU32` alternatives ⇒ 4 bytes; `sizeof(rpc_message_header_v) = 8×4 = 32`):

```41:54:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia/generated/g_rpc-message-header.h
typedef struct rpc_message_header_v03_00
{
    NvU32      header_version;
    NvU32      signature;
    NvU32      length;
    NvU32      function;
    NvU32      rpc_result;
    NvU32      rpc_result_private;
    NvU32      sequence;
    rpc_message_rpc_union_field_v u;
    rpc_generic_union rpc_message_data[];
} rpc_message_header_v03_00;
```

**Byte layout** (offsets within the header and within the enclosing element, i.e. `+48`):

| Field | Off (hdr) | Off (elem) | Size | Value built by CPU |
|---|---|---|---|---|
| `header_version` | 0 | 48 | 4 | **`0x03000000`** (see box below) |
| `signature` | 4 | 52 | 4 | `0x43505256` ("VRPC", LE bytes `56 52 50 43`) |
| `length` | 8 | 56 | 4 | `32 + paramLength` |
| `function` | 12 | 60 | 4 | RPC id (§13) |
| `rpc_result` | 16 | 64 | 4 | `0xFFFFFFFF` (PENDING) → `0` on success |
| `rpc_result_private` | 20 | 68 | 4 | `0xFFFFFFFF` → `0` |
| `sequence` | 24 | 72 | 4 | header-level seq (≠ element `seqNum`) |
| `u` (`spare`/`cpuRmGfid`) | 28 | 76 | 4 | `0` (`NV_VGPU_MSG_UNION_INIT`) |
| `rpc_message_data[]` | 32 | 80 | var | params (§7) |

> **CORRECTION (applied) — `header_version = 0x03000000`, not `0x00030000`.**
> S06 §6 inferred `0x00030000` by OR-ing the two `_TOT` magnitudes without applying the DRF
> field shift. **[EVIDENCE]** The field is built as
> `DRF_DEF(_VGPU,_MSG_HEADER_VERSION,_MAJOR,_TOT) | DRF_DEF(...,_MINOR,_TOT)`
> (`rpc_common.c:114-115`). The MAJOR field is bits **31:24**, MINOR bits **23:16**, with
> `MAJOR_TOT=0x3`, `MINOR_TOT=0x0` (`rpc_headers.h:56-59`); `DRF_DEF` shifts the value to the
> field's low bit, so MAJOR `= 0x3 << 24 = 0x03000000`, MINOR `= 0`. **Confirmed on the wire:
> all 1143 replies in `CAP2/rpc-resp-trace.txt` carry `w[0]=03000000` (§15)** — matches
> CORRECTIONS #3.

[EVIDENCE] Magic constants (`SRC/.../inc/vgpu/rpc_headers.h`):
`NV_VGPU_MSG_SIGNATURE_VALID = 0x43505256` (`:61`),
`NV_VGPU_MSG_RESULT_RPC_PENDING = 0xFFFFFFFF` (`:140`),
`NV_VGPU_MSG_UNION_INIT = 0x0` (`:142`),
`NV_VGPU_MSG_RESULT_SUCCESS = NV_OK = 0` (`:68`).

---

### 7. Payload sub-headers for the two control-plane RPCs

These are F06/S07's domain, but their fixed sub-headers sit at **element offset 80** and the
trace validates the whole frame, so they belong to the byte map. [EVIDENCE]
`SRC/src/nvidia/generated/g_rpc-structures.h:228-254`:

`rpc_gsp_rm_control_v` (`function=76`) — **24-byte** fixed header then params:

| Field | Off (data) | Off (elem) | Size |
|---|---|---|---|
| `hClient` | 0 | 80 | 4 |
| `hObject` | 4 | 84 | 4 |
| `cmd` | 8 | 88 | 4 |
| `status` | 12 | 92 | 4 |
| `paramsSize` | 16 | 96 | 4 |
| `flags` | 20 | 100 | 4 |
| `params[]` | 24 | 104 | `paramsSize` |

`rpc_gsp_rm_alloc_v` (`function=103`) — **32-byte** fixed header then params:
`hClient`(0) `hParent`(4) `hObject`(8) `hClass`(12) `status`(16) `paramsSize`(20) `flags`(24)
`reserved[4]`(28) `params[]`(32).

[EVIDENCE] Length identity holds on the wire (`CAP2/rpc-resp-trace.txt:3`, a fn=76 reply):
`32 (msg hdr) + 24 (ctrl hdr) + 92 (paramsSize 0x5c) = 148 = len`. ✓ For fn=103 alloc replies,
`len=64 = 32 + 32 + 0`. ✓

---

### 8. Building a request: `rpcWriteCommonHeader` + magic values

[EVIDENCE] Every RPC builder first calls `rpcWriteCommonHeader(pGpu, pRpc, func, paramLength)`,
which zeroes the whole buffer and stamps the header (`rpc_common.c:100-142`):

```112:139:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia/src/kernel/rmapi/rpc_common.c
    portMemSet(pRpc->message_buffer, 0, pRpc->maxRpcSize);

    vgpu_rpc_message_header_v->header_version     = DRF_DEF(_VGPU, _MSG_HEADER_VERSION, _MAJOR, _TOT) |
                                                    DRF_DEF(_VGPU, _MSG_HEADER_VERSION, _MINOR, _TOT);
    vgpu_rpc_message_header_v->signature          = NV_VGPU_MSG_SIGNATURE_VALID;
    vgpu_rpc_message_header_v->rpc_result         = NV_VGPU_MSG_RESULT_RPC_PENDING;
    vgpu_rpc_message_header_v->rpc_result_private = NV_VGPU_MSG_RESULT_RPC_PENDING;
    if (gpuIsSriovEnabled(pGpu) && IS_GSP_CLIENT(pGpu))
    {
        ...
        vgpu_rpc_message_header_v->u.cpuRmGfid = 0;
        ...
    }
    else
    {
        vgpu_rpc_message_header_v->u.spare        = NV_VGPU_MSG_UNION_INIT;
    }
    vgpu_rpc_message_header_v->function           = func;
    vgpu_rpc_message_header_v->length             = sizeof(rpc_message_header_v) + paramLength;
```

[INFERENCE] GA102 is **not** SR-IOV in this bring-up, so the `else` branch runs ⇒ `u = 0`.
After the common header the builder writes its params into `rpc_message` and calls an issue
helper (§12). The two builders an implementer needs first set
`func = NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL` (`rpc.c:1677`) and `..._GSP_RM_ALLOC`
(`rpc.c:1887`).

---

### 9. Construction order (CPU init) — exact sequence

[EVIDENCE] `GspMsgQueuesInit` (`message_queue_cpu.c:200-346`) then `GspStatusQueueInit`
(`:348-424`):

1. `_getMsgQueueParams` → sizes; `pageTableEntryCount=129`, `pageTableSize=0x1000` (§3a) (`:235`).
2. `memdescCreate(sharedBufSize=0x81000, RM_PAGE_SIZE, NONCONTIGUOUS, ADDR_SYSMEM, CACHED, ALLOC_IN_UNPROTECTED_MEMORY)` (`:246-255`).
3. `memdescAlloc` (`:259-261`); `memdescMap(... NV_PROTECT_WRITEABLE)` → `pVaKernel` (`:264-268`).
4. **Zero the whole block**: `portMemSet(pVaKernel, 0, sharedBufSize)` (`:280`).
5. `pPageTbl = pVaKernel` (`:282`); `memdescGetPhysAddrs(stride=RM_PAGE_SIZE, count=129, pPageTbl)` fills PTEs (`:296-301`).
6. `pCommandQueue = pVaKernel + pageTableSize`; `pStatusQueue = pCommandQueue + 0x40000` (`:303-307`).
7. `_gspMsgQueueInit(RM)`: alloc CPU-private work area `= 4096 + 0x10000 + msgqGetMetaSize()` (`:147-149`); `pCmdQueueElement = ALIGN_UP(pWorkArea, 4096)` (`:158-159`); `pMetaData = pCmdQueueElement + 0x10000` (`:160`); `msgqInit(&hQueue, pMetaData)` (`:162`); `msgqTxCreate(hQueue, pCommandQueue, 0x40000, 4096, 4, 12, SWAP_RX)` **writes the command TX header into shared mem** (`:170-176`); `pRpcMsgBuf = &pCmdQueueElement->rpc` (`:184`).
8. `queueIdx = RPC_TASK_RM_QUEUE_IDX = 0` (`:329`); `sharedMemPA = pPageTbl[0]` (`:338`) → deliver to GSP (F0x/S05).
9. *(GSP boots and runs its own `msgqTxCreate` on the status queue.)*
10. `GspStatusQueueInit`: loop { `portAtomicMemoryFenceFull()` (`:381`); `msgqRxLink(hQueue, pStatusQueue, 0x40000, 4096)` (`:383-384`) } until it returns 0, **4 s** timeout (`timeoutUs=4000000`, `:354`). This rendezvous proves GSP is live on the queue.

The staging element + `msgqMetadata` live in CPU-private non-paged memory — **only the page
table + two rings are shared**.

---

### 10. Send path + store fence + doorbell — exact order

[EVIDENCE] `GspMsgQueueSendCommand` (`message_queue_cpu.c:490-622`), assuming the caller
already wrote `pCmdQueueElement->rpc` (header via §8, then params):

1. `uElementSize = GSP_MSG_QUEUE_ELEMENT_HDR_SIZE + rpc.length = 48 + (32 + paramLen)` (`:500-501`); reject if `< 80` or `> 0x10000` (`:503-510`).
2. Zero-pad staging tail to an 8-byte multiple (`:513-514`).
3. `seqNum = txSeqNum`; `elemCount = ⌈uElementSize / 4096⌉`; `checkSum = 0` (`:516-518`).
4. (CC off) `checkSum = _checkSum32(pSrc, uElementSize)` (`:547`).
5. For `i = 0 .. elemCount−1`: spin on `msgqTxGetWriteBuffer(hQueue, i)` ("one at a time, since they could wrap"), **1 s** timeout (`gpuSetTimeout(... 1000000 ...)`, `:558`; retry does `portAtomicMemoryFenceFull()` then `osSpinLoop()`, `:572-574`); `portMemCopy` exactly **4096 B** per page (`:591-593`). Out of space ⇒ `NV_ERR_BUSY_RETRY` (`:577-585`).
6. **STORE FENCE** before publishing the write pointer, then submit:

```604:606:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c
    portAtomicMemoryFenceStore();

    nRet = msgqTxSubmitBuffers(pMQI->hQueue, pCQE->elemCount);
```
   `msgqTxSubmitBuffers` advances `tx.writePtr += elemCount` (wrap mod 63) and writes it to
   `cmdQueue+0x10` (`msgq.c:528-535`). Then `txSeqNum++` (`:616`).

7. **DOORBELL** — the data copy + `writePtr` update do **not** by themselves wake GSP. The
   caller `_kgspRpcSendMessage` rings it immediately after a successful send (`kernel_gsp.c:346`):

```332:346:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c
    nvStatus = GspMsgQueueSendCommand(pRpc->pMessageQueueInfo, pGpu);
    if (nvStatus != NV_OK)
    {
        ...
        return nvStatus;
    }

    kgspSetCmdQueueHead_HAL(pGpu, pKernelGsp, pRpc->pMessageQueueInfo->queueIdx, 0);
```

   The HAL (GA102→TU102, `g_kernel_gsp_nvoc.h:546`) is a single MMIO write
   (`kernel_gsp_tu102.c:316-322`):

```316:322:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c
    NV_ASSERT_OR_RETURN(queueIdx < NV_PGSP_QUEUE_HEAD__SIZE_1, NV_ERR_INVALID_ARGUMENT);

    // Write the value to the correct queue head.
    GPU_REG_WR32(pGpu, NV_PGSP_QUEUE_HEAD(queueIdx), value);

    return NV_OK;
```

[EVIDENCE] Register (`SRC/.../ampere/ga102/dev_gsp.h:38-40`):
`NV_PGSP_QUEUE_HEAD(i) = 0x110c00 + i*8`, `__SIZE_1 = 8`, field `_ADDRESS 31:0`. For the RM
queue (idx 0) the bring-up doorbell is **`GPU_REG_WR32(BAR0 + 0x110c00, 0x00000000)`** (the
4th arg passed at `kernel_gsp.c:346` is the literal `0`). This offset is in the GSP Falcon
`0x11xxxx` block, consistent with CONTEXT_BRIEF §4a.

[EVIDENCE] Empirically confirmed: `CAP2/boot-trace.txt:732-733` shows exactly the two
doorbell writes for the two async boot RPCs (§15):
```
RVGREG wr off=0x110c00 val=0x00000000
RVGREG wr off=0x110c00 val=0x00000000
```
[INFERENCE] value is always `0`; GSP learns the real producer cursor from the shared
`writePtr` (the register is a pure "kick"/interrupt) — see §18 TODO.
**Producer ordering (canonical):** per-4096-page copies → **store fence** → publish `writePtr`
→ doorbell MMIO.

---

### 11. Receive path + full fence

[EVIDENCE] Each receive is preceded by a **full memory fence** so the CPU observes GSP's queue
writes (`kernel_gsp.c:1465-1470`; without it "the CPU may get stuck in an infinite loop
waiting for a message that has already arrived"). Then `GspMsgQueueReceiveStatus`
(`message_queue_cpu.c:636-814`):

1. `msgqRxGetReadBuffer(hQueue, 0)`; if NULL on the **first** element ⇒ `NV_WARN_NOTHING_TO_DO` (queue empty) (`:660-665`).
2. `portMemCopy` 4096 B into staging; from element 0 read `elemCount` to learn the true span; copy remaining pages (`:676-688`).
3. (CC off) validate `_checkSum32(staging, 48 + rpc.length) == 0` (`:710-720`).
4. validate `seqNum == rxSeqNum`; stale-packet recovery via `msgqRxMarkConsumed` (`:722-749`).
5. validate `length` in `[80, 0x10000]` (`:789-799`).
6. on success `rxSeqNum++`; `msgqRxMarkConsumed(hQueue, nElements)` advances the consumer `readPtr` (SWAP: `cmdQueue+0x20`) (`:801-811`, `msgq.c:675-683`).

---

### 12. RPC dispatch: `_issueRpcAndWait` vs `_issueRpcAsync`, recv-poll drain

[EVIDENCE] **Synchronous** `_issueRpcAndWait` (`rpc.c:150-257`): cache `expectedFunc` (`:187`),
send, poll for the reply, then check `rpc_result`. Our trace hooks bracket it (`:189` send,
`:226` recv, `:229-238` the 32-dword `RVGRESP` reply dump):

```186:227:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia/kernel/vgpu/nv/rpc.c
    // For HCC, cache expectedFunc value before encrypting.
    NvU32 expectedFunc = vgpu_rpc_message_header_v->function;

    NV_PRINTF(LEVEL_ERROR, "RVGTRACE rpc send sync fn=%u len=%u\n",
              vgpu_rpc_message_header_v->function, vgpu_rpc_message_header_v->length);

    status = rpcSendMessage(pGpu, pRpc);
    ...
    // Use cached expectedFunc here because vgpu_rpc_message_header_v is encrypted for HCC.
    status = rpcRecvPoll(pGpu, pRpc, expectedFunc);
    ...
    NV_PRINTF(LEVEL_ERROR, "RVGTRACE rpc recv sync fn=%u result=0x%08x\n",
              expectedFunc, vgpu_rpc_message_header_v->rpc_result);
```
Success check: `rpc_result != NV_VGPU_MSG_RESULT_SUCCESS` → return the GSP code or
`NV_ERR_GENERIC` (`rpc.c:244-254`).

[EVIDENCE] **Asynchronous** `_issueRpcAsync` (`rpc.c:259-285`): send only, no poll —
fire-and-forget, used for the pre-`INIT_DONE` boot RPCs. Trace hook `RVGTRACE rpc send async`
at `:266`; `rpcSendMessage` at `:269`. (`_issueRpcLarge` at `:287+` adds multi-element chunking
via `CONTINUATION_RECORD` for payloads > `maxRpcSize` = 65488 — only needed once a single
display control struct exceeds that, not for first send.)

[EVIDENCE] The GSP recv-poll HAL `_kgspRpcRecvPoll` (`kernel_gsp.c:1774-1882`) loops with a
timeout, each iteration calling `_kgspRpcDrainEvents` (`:1854`); the drain core decides what to
do with each record:

```1472:1480:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c
    if (nvStatus == NV_OK)
    {
        rpc_message_header_v *pMsgHdr = RPC_HDR;

        if (pMsgHdr->function == expectedFunc)
            return NV_WARN_MORE_PROCESSING_REQUIRED;

        _kgspProcessRpcEvent(pGpu, pRpc);
    }
```
i.e. **any record whose `function` is not what we are waiting for is dispatched as an async
event** (`_kgspProcessRpcEvent`), then polling continues. An implementer's recv loop **MUST**
do this drain-and-dispatch or it will deadlock on interleaved events.

[EVIDENCE] Our instrumentation→trace mapping (current source line numbers):
- `rpc.c:189` `RVGTRACE rpc send sync fn=%u len=%u`
- `rpc.c:226` `RVGTRACE rpc recv sync fn=%u result=0x%08x`
- `rpc.c:229-238` `RVGRESP fn=%u len=%u w=… (32 dwords)` (added for the CAP2 gap-fill; see §17)
- `rpc.c:266` `RVGTRACE rpc send async fn=%u len=%u`
- `rpc.c:1686` `RVGTRACE ctrl cmd=… hClient=… hObject=… paramsSize=…` (in the GSP_RM_CONTROL builder)
- `rpc.c:1897` `RVGTRACE alloc hClass=… hParent=… hObject=…` (in the GSP_RM_ALLOC builder)

---

### 13. Function ids (`rpc_global_enums.h`)

[EVIDENCE] Function ids are the **0-based position** in the `X(...)` list
(`rpc_global_enums.h:9-...`, macro `X` → `NV_VGPU_MSG_FUNCTION_##RPC`). Ids the bring-up path
uses:
- `NOP = 0` (`:9`), `SET_GUEST_SYSTEM_INFO = 1` (`:10`), `FREE = 10` (`:19`)
- `GET_GSP_STATIC_INFO = 65` (`:74`), `UPDATE_BAR_PDE = 70` (`:79`), `CONTINUATION_RECORD = 71` (`:80`)
- `GSP_SET_SYSTEM_INFO = 72` (`:81`), `SET_REGISTRY = 73` (`:82`)
- `GSP_RM_CONTROL = 76` (`:85`), `GSP_RM_ALLOC = 103` (`:112`)

---

### 14. `INIT_DONE` and the event channel (`NV_VGPU_MSG_EVENT_*`)

[EVIDENCE] Events are a separate enum based at `0x1000` (macro `E` → `NV_VGPU_MSG_EVENT_##RPC`):

```225:256:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia/kernel/inc/vgpu/rpc_global_enums.h
    E(FIRST_EVENT = 0x1000)                      // 0x1000
    E(GSP_INIT_DONE)                             // 0x1001
    E(GSP_RUN_CPU_SEQUENCER)                     // 0x1002
    E(POST_EVENT)                                // 0x1003
    E(RC_TRIGGERED)                              // 0x1004
    E(MMU_FAULT_QUEUED)                          // 0x1005
    E(OS_ERROR_LOG)                              // 0x1006
    E(RG_LINE_INTR)                              // 0x1007
    ...
    E(UCODE_LIBOS_PRINT)                         // 0x100c
    ...
    E(GSP_SEND_USER_SHARED_DATA)                 // 0x101b
    E(NVLINK_FAULT_UP)                           // 0x101c
    E(GSP_LOCKDOWN_NOTICE)                       // 0x101d
    E(MIG_CI_CONFIG_UPDATE)                      // 0x101e
    E(NUM_EVENTS)                                // END
```

[EVIDENCE] **`INIT_DONE (0x1001)` is the GSP-boot completion signal.** After boot, CPU-RM waits
for it through the *same* recv-poll, passing the event id as `expectedFunc`
(`kernel_gsp.c:3790`), then asserts the header result (`:3796`):
```
    NV_CHECK_OK_OR_RETURN(LEVEL_ERROR,
        rpcRecvPoll(pGpu, pRpc, NV_VGPU_MSG_EVENT_GSP_INIT_DONE));
    ...
    NV_ASSERT_OK_OR_RETURN(RPC_HDR->rpc_result);
```
Because `INIT_DONE` is consumed as the awaited func, the dispatcher's `INIT_DONE` case is a
no-op (`kernel_gsp.c:1417`, "Handled by `_kgspRpcRecvPoll`"). [INFERENCE] this is also why
`INIT_DONE` emits **no** `RVGTRACE rpc recv sync` line (that hook lives only in
`_issueRpcAndWait`, not the raw recv-poll) — consistent with its absence from the trace.

[EVIDENCE] Async events during normal operation are dispatched by `_kgspProcessRpcEvent`'s
switch (`kernel_gsp.c:1288-1424`), e.g. `RC_TRIGGERED`, `OS_ERROR_LOG`, `UCODE_LIBOS_PRINT`,
`GSP_LOCKDOWN_NOTICE`, `POST_EVENT`, `GSP_RUN_CPU_SEQUENCER`; unknown/`INIT_DONE`/default are
logged and ignored (`:1417-1424`). The interrupt bottom-half that drains events outside a sync
wait is `kgspRpcRecvEvents_IMPL` → `_kgspRpcDrainEvents(..., NUM_FUNCTIONS)`
(`kernel_gsp.c:3755-3768`).

---

### 15. Trace evidence (measured invariants, CAP2 = 115116 full modeset)

[EVIDENCE] Counts re-measured in `CAP2` this revision (`rpc-trace.txt`):
**2** `rpc send async`, **1143** `rpc send sync`, **1143** `rpc recv sync`, and **all 1143**
recvs carry `result=0x00000000` (0 non-zero). Total **≈1145 RPCs**.

[EVIDENCE] `CAP2/rpc-resp-trace.txt` = **1143** `RVGRESP` lines; the dump shows `w[0..31]`
(32 dwords ⇒ ≤24 payload dwords/96 bytes per reply). Whole-file invariants:
- **1143/1143** begin `w=03000000 43505256` ⇒ `header_version=0x03000000`,
  `signature=0x43505256` (0 exceptions).
- `w[2]==len`, `w[3]==fn` on every line (spot-checked `:1-3`: fn=1/len=824→0x338,
  fn=65/len=2200→0x898, fn=76/len=148→0x94).
- `w[4]=w[5]=00000000` (rpc_result / rpc_result_private = success) across the file.

[EVIDENCE] Opening order (`CAP2/rpc-trace.txt`): async `fn=72` (GSP_SET_SYSTEM_INFO) → async
`fn=73` (SET_REGISTRY) → [GSP init / `INIT_DONE` wait] → sync `fn=1` (SET_GUEST_SYSTEM_INFO) →
sync `fn=65` (GET_GSP_STATIC_INFO) → steady-state `fn=76`/`fn=103`. [INFERENCE] the two async
sends are the only RPCs without a matching recv, and they are the two `0x110c00` doorbell
writes in `boot-trace.txt:732-733`.

> **CORRECTION (applied) — RPC headline count.** Per CORRECTIONS #2: **663 RPCs** in the
> *minimal bring-up* capture; **≈1145 RPCs (1143 sync reply pairs + 2 async)** in the
> *full-modeset* captures. Always state which capture. The 1145 figure is verified here against
> `CAP2`; the 663 figure is the earlier minimal capture (not in `CAP2`, see §18).

---

### 16. Minimal-path notes (first pixel on GA102)

**Essential (byte-match these):**
- **One RM queue pair** (idx 0), 256 KB each, in cached **unprotected** sysmem, prefixed by a
  129-entry software page table; shared block = **`0x81000`** (page table `0x0`, command queue
  `0x1000`, status queue `0x41000`); `sharedMemPA = pPageTbl[0]`. Zero it all first. [§3]
- Both TX headers: `version=0, size=0x40000, msgSize=0x1000, msgCount=63, flags=1(SWAP_RX),
  rxHdrOff=0x20, entryOff=0x1000`; entries start at queue+`0x1000` (63 slots × 4096 B).
  CPU writes the command TX header; expect GSP to match the status TX header (`msgqRxLink`
  validates). [§4]
- Per-slot prefix (48 B): `checkSum@32, seqNum@36, elemCount@40`; `rpc@48`;
  `authTagBuffer`/`aadBuffer` = zero (CC off). [§5]
- Wire header (32 B @ elem+48): **`header_version=0x03000000`**, `signature=0x43505256`,
  `length=32+paramLen`, `function`, `rpc_result/_private=0xFFFFFFFF` pre-send, `sequence`,
  `u=0`. [§6]
- Send: per-4096-page copy → `portAtomicMemoryFenceStore()` → advance `writePtr` (cmdQueue+0x10)
  → doorbell **`GPU_REG_WR32(BAR0+0x110c00, 0)`**. [§10]
- Recv: `portAtomicMemoryFenceFull()` first → checksum(==0) + `seqNum` checks → advance
  `readPtr` (cmdQueue+0x20); recv-poll loop **must drain-and-dispatch** non-matching records as
  events. [§11–§12]
- Boot ordering: async `GSP_SET_SYSTEM_INFO(72)` + `SET_REGISTRY(73)` → wait `INIT_DONE(0x1001)`
  → synchronous `GSP_RM_CONTROL(76)` / `GSP_RM_ALLOC(103)` plane. [§14–§15]

**Skippable / stub for first pixel:**
- **TaskIsr queue (idx 1)** — not allocated unless `bIsTaskIsrQueueRequired`
  (`message_queue_cpu.c:104-114`); keeping it off is what makes `pageTableEntryCount=129`,
  `sharedBufSize=0x81000`. If ever enabled, recompute §3a. [EVIDENCE]
- **Confidential Compute** (`authTagBuffer`/`aadBuffer`, `ccslEncrypt`/`Decrypt`, whole-element
  checksum) — GA102 consumer, CC disabled; take the plain branches
  (`message_queue_cpu.c:545-548`, `:708-713`). [INFERENCE from `PDB_PROP_CONFCOMPUTE_*` gates]
- **RPC profiler** (`bProfileRPC`) / history rings — diagnostics only (`rpc.c:159-184`).
- **Most async event handlers** — may log+ignore, BUT keep `UCODE_LIBOS_PRINT(0x100c)` and
  `OS_ERROR_LOG(0x1006)` for debugging, and never skip the drain loop. [INFERENCE §12]
- **`_issueRpcLarge`/CONTINUATION_RECORD** — only once a single payload exceeds `maxRpcSize`
  (65488 B); deferrable until large display control structs appear. [EVIDENCE `rpc.c:287+`,
  `kernel_gsp.c:1995`]

---

### 17. Corrections applied (vs S06 / D02 drafts) — judge reconciliation

1. **`header_version` value [CORRECTED, CORRECTIONS #3].** S06 §6 stated `0x00030000`
   ([INFERENCE]). Verified wrong: DRF field MAJOR=31:24 with MAJOR_TOT=0x3 ⇒ **`0x03000000`**
   (`rpc_common.c:114-115` + `rpc_headers.h:56-59`), and **1143/1143** live replies carry
   `w[0]=03000000` (`CAP2/rpc-resp-trace.txt`). F05 uses `0x03000000` and labels it [EVIDENCE].
2. **rpc.c line citations [CORRECTED].** S06's rpc.c citations after line ~229 are ~11 lines
   too low because the 10-line `RVGRESP` dump block (`rpc.c:229-238`) was added for the CAP2
   gap-fill *after* S06 was written. Re-pinned to current source: async trace `255→266`,
   `GSP_RM_CONTROL` builder `1666→1677`, ctrl trace `1675→1686`, `GSP_RM_ALLOC` builder
   `1876→1887`, alloc trace `1886→1897`, success-check `233-243→244-254`. (The sync trace
   points `189`/`226` and the quoted block predate the insertion and were already correct.)
3. **RPC counts [RECONCILED, CORRECTIONS #2].** Replaced S06's open "663 vs 1145" TODO with the
   resolved statement: 663 (minimal bring-up) / ≈1145 (full modeset, = 1143 sync + 2 async),
   the latter re-verified against `CAP2`.
4. **Send-loop timeout label [CLARIFIED].** The per-element wait is **1 s** (`gpuSetTimeout(...,
   1000000, ...)`, `message_queue_cpu.c:558`); the inline source comment "Retry for up to 10 ms"
   (`:560`) is stale — trust the code value (matches S06/D02's "~1 s").
5. **No downgrades required for the byte map.** D02's shared-block byte map, TX-header dword
   values (`msgCount=63`, `rxHdrOff=0x20`, `entryOff=0x1000`), 48-byte element prefix, 32-byte
   wire header, SWAP_RX ownership, doorbell register/value, and the 24/32-byte control/alloc
   sub-headers were each re-derived from source and **all hold**.

Items kept as **[INFERENCE]** (not promoted): GSP-side status-queue `msgqTxCreate` literal
values (inferred from the CPU's `msgqRxLink` sanity checks, GSP firmware closed); doorbell
"pure kick" semantics (value always 0); the 44→48 element pad (struct + `aligned(8)`);
`"VRPC"` ASCII reading of `0x43505256`.

---

### Evidence cited

Source (`SRC = /home/flare/dev/gpu-repro/open-gpu-kernel-modules`):
- `src/nvidia/inc/kernel/gpu/mem_mgr/rm_page_size.h:38,41,42` — `RM_PAGE_SIZE=4096`, `RM_PAGE_MASK=0x0FFF`, `RM_PAGE_SHIFT=12`.
- `src/nvidia/inc/kernel/gpu/gsp/message_queue.h:31-33,39-43` — queue indices (RM=0, COUNT=2) + CPU API.
- `src/nvidia/inc/kernel/gpu/gsp/message_queue_priv.h:43-51` `GSP_MSG_QUEUE_ELEMENT`; `:53-86` `MESSAGE_QUEUE_INFO`/`MESSAGE_QUEUE_COLLECTION` (`sharedMemPA`); `:91-104` sizes/aligns (`HDR_SIZE=48`, `SIZE_MIN=4096`, `SIZE_MAX=0x10000`, `RPC_SIZE_MAX=65488`, `HEADER_ALIGN=4`, `ELEMENT_ALIGN=12`); `:112-124` `_checkSum32`.
- `src/common/shared/msgq/inc/msgq/msgq.h:31,39` — `MSGQ_MSG_SIZE_MIN=16`, `MSGQ_FLAGS_SWAP_RX=1`; `:37-39` swap-RX rationale.
- `src/common/shared/msgq/inc/msgq/msgq_priv.h:38` `MSGQ_VERSION=0`; `:48-65` `msgqTxHeader`(8×u32)+`msgqRxHeader`; `:67-109` `msgqMetadata` (CPU-private).
- `src/common/shared/msgq/msgq.c:108-136` no-hook direct access; `:226-241` TX-header field computation (`rxHdrOff=0x20`,`entryOff=0x1000`,`msgCount=63`); `:251` `txFree=62`; `:254-272` SWAP_RX pointer wiring (`pWriteOutgoing` :257, swapped read cursors :262-263); `:366-385` rxLink sanity checks; `:461` txFree formula; `:528-535` writePtr advance; `:675-683` readPtr advance.
- `src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c:57` header-fits assert; `:73-74,78-101` sizes; `:104-114,312-322` TaskIsr gating; `:116-133` `_getMsgQueueParams` (numPtes=129, pageTableSize=0x1000); `:147-184` work area + `msgqInit`/`msgqTxCreate`/`pRpcMsgBuf`; `:200-346` `GspMsgQueuesInit`; `:240-280` shared alloc/map/zero; `:296-301` PTE fill; `:303-307` queue VAs; `:338` `sharedMemPA=pPageTbl[0]`; `:348-424` `GspStatusQueueInit`/`msgqRxLink` (+full fence `:381`, 4 s timeout `:354`); `:490-622` `GspMsgQueueSendCommand` (`:500-510` size check, `:513-518` pad+seqNum+elemCount+checksum, `:547` checksum, `:558` 1 s timeout, `:572-574` spin fence, `:591-593` per-page copy, `:604-606` store fence + submit, `:616` seq++); `:636-814` `GspMsgQueueReceiveStatus` (`:660-665,676-688,710-720,722-749,789-799,801-811`).
- `src/nvidia/generated/g_rpc-message-header.h:33-54` — `rpc_message_header_v` (32 B) + 4-byte union `u`.
- `src/nvidia/generated/g_rpc-structures.h:228-254` — `rpc_gsp_rm_alloc_v` (32-B hdr) / `rpc_gsp_rm_control_v` (24-B hdr).
- `src/nvidia/kernel/inc/vgpu/rpc_headers.h:56-59` version field defs (MAJOR=31:24, MINOR=23:16, MAJOR_TOT=3) → `header_version=0x03000000`; `:61` `SIGNATURE_VALID=0x43505256`; `:68` `RESULT_SUCCESS=0`; `:140` `RPC_PENDING=0xFFFFFFFF`; `:142` `UNION_INIT=0`.
- `src/nvidia/src/kernel/rmapi/rpc_common.c:100-142` `rpcWriteCommonHeader` (`:112` zero buffer, `:114-115` `header_version` DRF_DEF, `:116` signature, `:117-118` PENDING, `:136` union, `:138-139` function/length).
- `src/nvidia/kernel/inc/objrpc.h:98-99` — `vgpu_rpc_message_header_v`/`rpc_message` accessors.
- `src/nvidia/kernel/vgpu/nv/rpc.c:138-148` `_IMPL` stubs; `:150-257` `_issueRpcAndWait` (RVGTRACE `:189,226`; RVGRESP `:229-238`; success-check `:244-254`); `:259-285` `_issueRpcAsync` (RVGTRACE `:266`); `:287+` `_issueRpcLarge`/CONTINUATION; `:1407` SET_REGISTRY builder (async); `:1677,1686` GSP_RM_CONTROL builder + RVGTRACE ctrl; `:1887,1897` GSP_RM_ALLOC builder + RVGTRACE alloc.
- `src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c:82` `RPC_HDR`; `:319-348` `_kgspRpcSendMessage` + doorbell call `:346`; `:1288-1424` `_kgspProcessRpcEvent` (INIT_DONE no-op `:1417`); `:1462-1480` `_kgspRpcDrainOneEvent` (full fence `:1468`, drain-and-dispatch `:1472-1480`); `:1504-1529` `_kgspRpcDrainEvents`; `:1774-1882` `_kgspRpcRecvPoll`; `:1995-1998` OBJRPC wiring (maxRpcSize `:1995`, send fnptr `:1997`); `:3755-3768` `kgspRpcRecvEvents_IMPL`; `:3790,3796` `kgspWaitForRmInitDone` (INIT_DONE poll + result assert).
- `src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c:308-323` `kgspSetCmdQueueHead_TU102` (doorbell MMIO).
- `src/nvidia/generated/g_kernel_gsp_nvoc.h:537,546,549` — HAL resolves GA102→TU102.
- `src/common/inc/swref/published/ampere/ga102/dev_gsp.h:38-40` — `NV_PGSP_QUEUE_HEAD(i)=0x110c00+i*8`, `__SIZE_1=8`, `_ADDRESS 31:0`.
- `src/nvidia/kernel/inc/vgpu/rpc_global_enums.h:9,10,19,74,79,80,81,82,85,112` function ids; `:225-256` events (`GSP_INIT_DONE=0x1001`).

Trace (`CAP2 = /home/flare/dev/gpu-repro/traces/20260530-115116-open-capture`):
- `rpc-resp-trace.txt` — 1143 `RVGRESP` lines, **all** `w[0]=03000000`, `w[1]=43505256`, `w[4]=w[5]=0`; `:1-3` fn=1/65/76 length+sub-header decode.
- `rpc-trace.txt` — counts: 2 async send, 1143 sync send, 1143 sync recv, 0 non-zero results.
- `boot-trace.txt:732-733` — the two doorbell writes `off=0x110c00 val=0x00000000` (async boot RPCs).

Cross-ref drafts: S06 (semantics), D02 (byte layout), D03 (reply framing); CORRECTIONS #2, #3.

---

### Open questions / TODO

- [TODO] **`sharedMemPA` delivery to GSP.** Which boot-arg field carries `pPageTbl[0]` (and any
  per-queue offsets) into GSP is F0x/S05 scope, not byte-specified here. Inspect
  `kgspSetupLibosInitArgs` / `GSP_ARGUMENTS_CACHED` near `kernel_gsp.c:3700-3744`.
- [TODO] **GSP-side status-queue literal values.** `msgCount=63`, `rxHdrOff=0x20`,
  `entryOff=0x1000` for the status queue are [INFERENCE] from the CPU's `msgqRxLink` sanity
  checks (`msgq.c:366-385`), not read from GSP firmware (closed). Confirm empirically if a
  status-queue dump becomes available.
- [UNCERTAIN] **Doorbell value semantics.** `NV_PGSP_QUEUE_HEAD(0)` is written `0` every send
  although the field is `_ADDRESS 31:0`. Confirm GSP treats it as a pure kick (reads `writePtr`
  from the shared header) vs. consuming the register value. GSP-side consumer is not in the open
  tree; check for any `QUEUE_HEAD` read-back.
- [TODO] **`sequence` (header) vs `seqNum` (element).** Both are 0 in the captured replies
  (`w[6]=0`); confirm whether GSP ever increments the header-level `sequence` under load (the
  element `seqNum` is the one msgq validates).
- [TODO] **Endianness / struct packing.** Offsets assume natural LP64 alignment, no packing
  pragmas (none seen). Re-verify the 44→48 element pad and the 4-byte union for any non-x86-64
  StelluxOS ABI target.
- [EVIDENCE-GAP] **`RVGRESP` dumps only 32 dwords** (≤96 payload bytes) → deep reply fields
  (e.g. fn=65 `GspStaticConfigInfo` tail) are not captured here; corroborate via F06/D03.
- [TODO] **663 figure not in `CAP2`.** The minimal-bring-up RPC count (663) comes from the
  earlier `110235/110540` capture referenced in CONTEXT_BRIEF §5, not present in `CAP2`; the
  full-modeset 1145 is the one verified in this section. Re-confirm 663 against that capture if a
  single headline number is needed.


## F06 — RM Object Model & Bring-up RPC Sequence

Final, merged, source-verified section. Consolidates draft **S07** (RPC entry
points + object/handle model), **D03** (measured early RPC replies), and **D04**
(GSP_RM_ALLOC catalog) for the GA102 RTX 3080 minimal display bring-up.

Scope: the CPU-RM ↔ GSP-RM control plane that runs **after** `INIT_DONE` (the
10th GSP-boot stage; register-level boot is F0x/§4a, out of scope here). Covers
(1) the four early handshake RPCs and what `fn=65` returns, (2) the GSP-assigned
internal handles, (3) the RM client/device/subdevice handle model, (4)
`GSP_RM_CONTROL` (fn=76) and `GSP_RM_ALLOC` (fn=103) mechanics, and (5) the
complete minimal display-object allocation catalog (parent/child + which structs
need real values). Detection/EDID/IMP controls and EVO modeset/scanout are F07+.

Path shorthand (as in the briefs):
`SRC = /home/flare/dev/gpu-repro/open-gpu-kernel-modules`,
`CAP2 = traces/20260530-115116-open-capture` (reply/data capture),
`CAP = traces/20260530-112551-open-capture` (payload capture),
`AN = analysis`. RPC entry points are in `SRC/src/nvidia/kernel/vgpu/nv/rpc.c`
(`rpc.c` below).

---

### F06.0 — Verification status (what this judge re-checked)

Every claim below was re-verified against the live source tree and traces.
Findings that changed the source drafts:

- **[CORRECTED — line numbers] S07's `rpc.c` citations are systematically ~11
  lines low** (the draft was written against an earlier copy). The verified
  current offsets are used throughout F06. Spot table:

  | symbol | S07 said | **verified** |
  |---|---|---|
  | `rpcRmApiControl_GSP` | 1559 | **1570** |
  | `rpcRmApiAlloc_GSP` | 1819 | **1830** |
  | `RmRpcSetGuestSystemInfo` (fn=1) | 676 | **687** |
  | `rpcGetGspStaticInfo_v14_00` (fn=65) | 1059 | **1070** |
  | `rpcGspSetSystemInfo_v17_00` (fn=72) | 1269 | **1280** |
  | `rpcSetRegistry_v17_00` (fn=73) | 1380 | **1391** |
  | `RVGPAY` ctrl dump | 1606-1617 | **1621-1627** |
  | `RVGTRACE alloc` hook | 1886-1887 | **1897-1898** |

  D03 and D04 citations were found accurate (they match the live tree).
- **[CORRECTED — reconciliation] The GSP-shared internal client is NOT
  `0xc1e00001`.** S07.2 implied `pGpu->hInternalClient = RS_CLIENT_INTERNAL_HANDLE_BASE+1
  = 0xc1e00001` per `gpu.c:779`; that line is the **Tegra/T234-only** path. On
  this GA102 (dGPU) the handle comes from `GspStaticConfigInfo` and is
  **`0xc2000005`** (F06.3). The `0xc1e0xxxx` clients in the trace are separate
  CPU-RM probe clients (F06.5).
- **[RESOLVED — was S07 TODO] `NV04_DISPLAY_COMMON` (0x73) NULL alloc params.**
  Confirmed two ways: the class header defines no `*_ALLOC_PARAMETERS` struct
  (`cl0073.h`), and the alloc path keys NULL-acceptance on the per-class
  `bNullAllowed` flag from `rmapiGetClassAllocParamSize` (`rpc.c:1868,1879-1884`).
- **CORRECTIONS applied:** **#4** (DISPLAY_COMMON parented to the DEVICE) and
  **#7** (GSP-assigned internal handles). See F06.3 and F06.5/F06.8.
- **[CORRECTED — dword index] CHIP_INFO PCI-id offsets in F06.4 were off by one.**
  The draft put device/vendor/subsys/rev at `w[17]/w[18]/w[19]`; in the cited reply
  (`CAP2/rpc-resp-trace.txt:3`) they are at `w[18]/w[19]/w[20]` (`w[14..17]` are
  leading params). Values and §3 match either way; indices corrected.
- **[CORRECTED — name] grCapsBits corroborating control in F06.2** is
  `NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CAPS` (`cmd=0x20800a1f`,
  `…decoded-full.txt:165`), not "GR_GET_INFO". The fn=65 decode boundary is proven
  by the GID/UUID match, not this control.
- **Honest labels kept:** alloc-param *values* are **[INFERENCE]** (the
  `RVGTRACE alloc` hook prints only `hClass/hParent/hObject`, not params);
  struct *layouts* are **[EVIDENCE]** from the headers.

---

### F06.1 — Transport, the message header, and the trace hooks

**[EVIDENCE]** All bring-up RPC traffic flows through two RM-API entry points and
two transport helpers in `rpc.c`:

- `rpcRmApiControl_GSP` (`rpc.c:1570`) — fn=76, our `RVGPAY ctrl` payload dump
  (`rpc.c:1621-1627`) + `RVGTRACE ctrl` line (`rpc.c:1686-1687`).
- `rpcRmApiAlloc_GSP` (`rpc.c:1830`) — fn=103, our `RVGTRACE alloc` line
  (`rpc.c:1897-1898`).
- `_issueRpcAndWait` (`rpc.c:150`) — sync send/recv; `RVGTRACE rpc send sync`
  (`rpc.c:189-190`), `recv sync` (`rpc.c:226-227`), and the `RVGRESP` reply dump
  (`rpc.c:231-237`).
- `_issueRpcAsync` (`rpc.c:259`) — fire-and-forget; `RVGTRACE rpc send async`
  (`rpc.c:266-267`). Large variants `_issueRpcAndWaitLarge`/`_issueRpcAsyncLarge`
  (`rpc.c:466`/`:480`).

**[EVIDENCE]** Every reply is the raw GSP message buffer. The first **8 dwords are
the fixed header** `rpc_message_header_v03_00`
(`SRC/src/nvidia/generated/g_rpc-message-header.h:41-52`); the **payload begins at
dword index 8** (`rpc_message_data[]`, `:51`):

| dword | header field | observed (all early replies) |
|---|---|---|
| `w[0]` | `header_version` | `0x03000000` → v3 (MAJOR in 31:24) |
| `w[1]` | `signature` | `0x43505256` = ASCII "VRPC" |
| `w[2]` | `length` | full msg incl. 32-byte header |
| `w[3]` | `function` | `=fn` (`0x01`/`0x41`/`0x4c`/`0x67`…) |
| `w[4]` | `rpc_result` | `0x0` = transport success |
| `w[5]` | `rpc_result_private` | fn=1 handshake status |
| `w[6]` | `sequence` | `0x0` |
| `w[7]` | `u` (spare/cpuRmGfid) | `0x0` |
| `w[8…]` | `rpc_message_data[]` | the reply payload |

**[EVIDENCE]** The `RVGRESP` hook dumps exactly `w[0]..w[31]` (`rpc.c:231-237`) ⇒
**≤24 payload dwords (96 bytes) per reply** regardless of true `len`. For large
replies (e.g. fn=65 `len=2200`) only the first 96 payload bytes are visible;
deeper fields are **[EVIDENCE-GAP]** in this trace (see TODO).

**[EVIDENCE]** Transport success (`w[4]=0`) ≠ per-handler success: a control can
return `rpc_params->status != 0` while `w[4]=0` (F06.4 NVLink example).

**[EVIDENCE]** Function ids (`SRC/src/nvidia/kernel/inc/vgpu/rpc_global_enums.h`):
`SET_GUEST_SYSTEM_INFO=1` (`:10`), `GET_GSP_STATIC_INFO=65` (`:74`),
`GSP_SET_SYSTEM_INFO=72` (`:81`), `SET_REGISTRY=73` (`:82`),
`GSP_RM_CONTROL=76` (`:85`), `GSP_RM_ALLOC=103` (`:112`). Confirmed on-wire:
fn=65 reply `w[3]=0x41`, fn=76 `w[3]=0x4c`, fn=103 `w[3]=0x67`.

---

### F06.2 — The four early RPCs (post-INIT_DONE handshake), in order

**[EVIDENCE]** Exact captured order & transport
(`AN/20260530-110235-open-capture-rpc-decoded.txt:1-6`):

```
send async fn=72  GSP_SET_SYSTEM_INFO    len=704
send async fn=73  SET_REGISTRY           len=1292
send sync  fn=1   SET_GUEST_SYSTEM_INFO  len=824   -> recv result=0x0
send sync  fn=65  GET_GSP_STATIC_INFO    len=2200  -> recv result=0x0
```

72/73 are **async** (no recv line; end in `_issueRpcAsync`); 1/65 are **sync**
(paired send/recv; end in `_issueRpcAndWait`).

#### fn=72 `GSP_SET_SYSTEM_INFO` — CPU→GSP, push (async)
`rpcGspSetSystemInfo_v17_00` (`rpc.c:1280`). Fills `GspSystemInfo`
(`rpc.c:1292`; struct `gsp_static_config.h:151-181`) and sends async
(`rpc.c:1379`). **[EVIDENCE]** Key fields the implementer must supply
(`rpc.c:1311-1377`):
- `gpuPhysAddr`/`gpuPhysFbAddr`/`gpuPhysInstAddr` ← `pGpu->busInfo.*`
  (`rpc.c:1311-1313`) — physical bases GSP needs for MMIO/FB. **[INFERENCE]**
  `gpuPhysAddr` is the BAR0 register-aperture base; cross-ref F03 BAR map (BAR0
  `0xfb000000`).
- `nvDomainBusDeviceFunc` (BDF, `:1314`), `oorArch` (`:1315`);
  `pciConfigMirrorBase/Size` + `bMnocAvailable` (`:1320-1325`).
- `pcieAtomicsOpMask`/`consoleMemSize`/`maxUserVa` (`:1337-1340`),
  `acpiMethodData` (`:1349`), ASPM/bridge flags (`:1352-1357`),
  `hypervisorType`/`bIsPassthru` (`:1359-1360`), `gspVFInfo` SR-IOV
  (`:1363-1370`), `sysTimerOffsetNs` (`:1373`), `bTdrEventSupported` (`:1377`).

#### fn=73 `SET_REGISTRY` — CPU→GSP, push (async)
`rpcSetRegistry_v17_00` (`rpc.c:1391`). Packs the RM regkey table into a
`PACKED_REGISTRY_TABLE` via `osPackageRegistry` (`rpc.c:1414` size pass,
`:1445` fill) and sends async; **[EVIDENCE]** header is written with size 0 then
back-patched to `totalSize` (`rpc.c:1407`,`:1455`); a large-RPC bounce buffer is
used if the table exceeds one message (`rpc.c:1429-1451`).

#### fn=1 `SET_GUEST_SYSTEM_INFO` — CPU→GSP, sync (RPC version handshake)
`RmRpcSetGuestSystemInfo` (`rpc.c:687`). **[EVIDENCE]** Sends the driver version
string (`rpc.c:756-759`, buffer sized by
`NV0000_CTRL_CMD_SYSTEM_GET_VGX_SYSTEM_INFO_BUFFER_SIZE`, `:753-755`) + build
branch (`:770-772`), plus `vgxVersionMajor/MinorNum = VGX_*_VERSION_NUMBER`
(`rpc.c:792-794`), sync (`rpc.c:796`).

**[EVIDENCE — measured reply]** (`CAP2/rpc-resp-trace.txt:1`), payload struct
`rpc_set_guest_system_info_v03_00` (`g_rpc-structures.h:36-49`):

| dword | value | field | meaning |
|---|---|---|---|
| `w[8]` | `0x23` = 35 | `vgxVersionMajorNum` | negotiated VGX major |
| `w[9]` | `0x05` = 5 | `vgxVersionMinorNum` | negotiated VGX minor |
| `w[10..12]` | `0x100` each | `guest{Driver,,Title}VersionBufferLength` | 256 |
| `w[13]` | `0x0` | `guestClNum` | open build changelist 0 |
| `w[14..16]` | `2e353335 2e333831 00003130` | `guestDriverVersion[]` | LE ASCII **"535.183.01"** |

**[EVIDENCE]** Version-string decode (LE per dword) → `"535." "183." "01\0"` =
**`535.183.01`** = the GSP firmware/driver version (matches the on-disk
`gsp_ga10x.bin` 535.183.01).

**Host's required handling [EVIDENCE] (`rpc.c:798-857`):** after the sync wait,
check **`rpc_result_private` (`w[5]`)**, not just `rpc_result` (`w[4]`). If
`w[5] != NV_OK` and the echoed version differs (`rpc.c:798-802`): re-issue with
GSP's requested version if the host is newer (`:804-817`), else fail (`:819-833`).
On success, latch into `rpcVgxVersion` (`:848-849`) and call `rpcSetIpVersion(...
RPC_VERSION_FROM_VGX_VERSION(major,minor))` (`:855-857`) — **this selects the
serialization version for all later RPCs**. For StelluxOS: send `35.5`
(`0x23.0x05`) so GSP 535.183.01 accepts with no retry, as measured.

#### fn=65 `GET_GSP_STATIC_INFO` — GSP→CPU, sync (the critical pull)
`rpcGetGspStaticInfo_v14_00` (`rpc.c:1070`). **[EVIDENCE]** Sizes the buffer to
`sizeof(GspStaticConfigInfo)` (`rpc.c:1082-1092`), sends sync (`:1100`), then
`portMemCopy`s the **byte-exact `GspStaticConfigInfo`** straight into
`GPU_GET_GSP_STATIC_INFO(pGpu)` with no FINN/serialization (`rpc.c:1104`; wire
struct is just `{ NvU32 data; }`, `g_rpc-structures.h:156-159`). So payload
`w[8]` = struct byte 0 of `GspStaticConfigInfo`
(`SRC/src/nvidia/inc/kernel/gpu/gsp/gsp_static_config.h:72-148`).

**[EVIDENCE — measured reply]** (`CAP2/rpc-resp-trace.txt:2`), visible fields
(struct bytes 0–95):
- **`grCapsBits[23]`** (`gsp_static_config.h:74`; `NV0080_CTRL_GR_CAPS_TBL_SIZE=23`,
  `SRC/src/common/sdk/nvidia/inc/ctrl/ctrl0080/ctrl0080gr.h:78`) occupies
  `w[8]..w[13]` (byte 23 = pad). These bytes are the GR caps table, also fetched by
  the `NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CAPS` control (`cmd=0x20800a1f`,
  `…decoded-full.txt:165`) — corroborating the boundary (decisively confirmed by the
  `gidInfo` GID/UUID match below).
- **`gidInfo`** = `NV2080_CTRL_GPU_GET_GID_INFO_PARAMS` (`gsp_static_config.h:75`)
  starts at struct byte 24 = `w[14]`: `w[14]=index=0`, `w[15]=flags=0x2`
  (`FORMAT_BINARY`/`TYPE_SHA1`), `w[16]=length=0x10`=16,
  `w[17..20]=8d15db20 ce92bb55 254aad7b 115618b3`.
  **[EVIDENCE — decisive]** read LE the GID is
  **`GPU-20db158d-55bb-92ce-7bad-4a25b3185611`**, exactly the `nvidia-smi -q` GPU
  UUID (`CAP2/nvidia-smi-q.txt:26`) — proving the dword→field decode is correct.

**What fn=65 returns that the implementer needs** (`gsp_static_config.h:72-148`;
fields past the 96-byte dump are **[EVIDENCE-GAP]** in this trace — copy the whole
struct anyway):
- FB geometry: `fb_length` (`:92`), `fbio_mask`/`fb_bus_width`/`fb_ram_type`/
  `fbp_mask`/`l2_cache_size` (`:93-97`).
- `gpuNameString` (`:102`); `bar1PdeBase`/`bar2PdeBase` (`:118-119`);
  `bVbiosValid`/`vbiosSubVendor`/`vbiosSubDevice` (`:121-123`).
- Display caps: `displaylessMaxHeads`/`Resolution`/`Pixels` (`:131-133`).
- **The three internal RMAPI handles (essential — F06.3):** `hInternalClient`
  (`:136`), `hInternalDevice` (`:139`), `hInternalSubdevice` (`:142`).

**[EVIDENCE]** Host handling: the receive buffer must be ≥
`sizeof(GspStaticConfigInfo)` (handler asserts, `rpc.c:1082-1092`); copy the
**entire** payload (`rpc.c:1104`), then extract the internal handles (F06.3).

---

### F06.3 — GSP-assigned internal handles (CORRECTIONS #7)

**[EVIDENCE]** After fn=65, `_gpuAllocateInternalObjects` (`SRC/src/nvidia/src/kernel/gpu/gpu.c:764`)
chooses the internal client/device/subdevice. There are **two paths**:
- **Tegra/T234 only** (`gpu.c:773-782`): literal constants
  `RS_CLIENT_INTERNAL_HANDLE_BASE+1` (`:779`),
  `NV_GPU_INTERNAL_DEVICE_HANDLE=0xABCD0080`,
  `NV_GPU_INTERNAL_SUBDEVICE_HANDLE=0xABCD2080`
  (`SRC/src/nvidia/generated/g_gpu_nvoc.h:836-837`).
- **dGPU (this GA102)** (`gpu.c:783-791`): **all three are taken from
  `GspStaticConfigInfo`** — `pGpu->hInternalClient = pGSCI->hInternalClient`,
  `…Device = pGSCI->hInternalDevice`, `…Subdevice = pGSCI->hInternalSubdevice`.

⇒ **[CORRECTION to S07.2]** `gpu.c:779` (the `0xc1e00001` form) does **not** apply
here; it is Tegra-only. On GA102 the handles are GSP-chosen and delivered by
fn=65.

**[EVIDENCE — measured]** Every early `NV2080_CTRL_CMD_INTERNAL_*` reply carries
`w[8]=hClient`, `w[9]=hObject` (`CAP2/rpc-resp-trace.txt:3-11`), and the host
fills these from `pGpu->hInternalClient/hInternalSubdevice`
(`SRC/src/nvidia/src/kernel/gpu/gpu_gspclient.c:272-277`). Therefore the measured
values *are* the fn=65 struct fields:

- **`GspStaticConfigInfo.hInternalClient = 0xc2000005`** (`:136`) — GSP-assigned;
  **does not** match either client base (`0xC1D0…`/`0xC1E0…`). Treat as opaque.
- **`GspStaticConfigInfo.hInternalSubdevice = 0xabcd2080`** (`:142`) — happens to
  equal `NV_GPU_INTERNAL_SUBDEVICE_HANDLE`, but read it from fn=65, don't assume.
- **`hInternalDevice = 0xabcd0080`** (`:139`) **[INFERENCE]** — not exercised by
  the early (subdevice-targeted) controls, so not directly in the trace; read it
  from fn=65.

**[EVIDENCE]** `_gpuAllocateInternalObjects` also registers
`(hInternalClient,hInternalSubdevice)` and `(hInternalClient,hInternalDevice)` in
the control cache (`gpu.c:793-799`). This is why the `0xabcd2080` controls can
only begin **after** fn=65 returns.

**Implementer rule:** read `hInternalClient/Device/Subdevice` back from the fn=65
struct; use `(hInternalClient, hInternalSubdevice)` as `(hClient, hObject)` for
every `NV2080_CTRL_CMD_INTERNAL_*`. Hardcoding `0xabcd2080` happens to work;
hardcoding `0xc2000005` is unsafe (GSP-build-specific).

---

### F06.4 — The post-static-info control storm (first fn=76 replies)

**[EVIDENCE]** Immediately after fn=65, the host issues a run of internal
controls on `(0xc2000005, 0xabcd2080)`. The first ten replies
(`CAP2/rpc-resp-trace.txt:3-11`; named in
`AN/20260530-110540-open-capture-decoded-full.txt:7-31`) — fn=76 payload is
`rpc_gsp_rm_control_v03_00` (`g_rpc-structures.h:243-252`): `w[8]=hClient`,
`w[9]=hObject`, `w[10]=cmd`, `w[11]=status`, `w[12]=paramsSize`, `w[13]=flags`,
`w[14…]=params[]`:

| line | `cmd` | name | `status` | notes |
|---|---|---|---|---|
| `:3` | `0x20800a36` | `INTERNAL_GPU_GET_CHIP_INFO` | `0` | dev/sub/rev ids (below) |
| `:6` | `0x2080302c` | `NVLINK_GET_NVLINK_DEVICE_INFO` | **`0x56`** | benign NOT_SUPPORTED |
| `:9` | `0x20800170` | `GPU_GET_ENGINES_V2` | `0` | 11 engines |
| `:10` | `0x20801803` | `BUS_GET_PCI_BAR_INFO` | `0` | BAR map |
| `:11` | `0x20800a4b` | `INTERNAL_DISPLAY_GET_IP_VERSION` | `0` | `ipVersion` |

- **CHIP_INFO** (`ctrl2080internal.h:564`): `w[18]=0x221610de` → device **`0x2216`**
  / vendor **`0x10de`**; `w[19]=0x403f1458` → subsys **`0x403f`** / subvendor
  **`0x1458`** (Gigabyte); `w[20]=0x000000a1` → revision **`0xa1`** (`CAP2/rpc-resp-trace.txt:3`;
  `w[14..17]` are leading params, PCI ids begin at `w[18]`). Matches §3.
- **NVLINK** returns per-control `status=0x56` = `NV_ERR_NOT_SUPPORTED`
  (`SRC/src/common/sdk/nvidia/inc/nvstatuscodes.h:115`) while transport `w[4]=0`
  — GA102 has no NVLink. **Host rule [EVIDENCE]:** treat `rpc_result` (`w[4]`) and
  `rpc_params->status` (`w[11]`) independently; `NOT_SUPPORTED` on optional units
  must not abort bring-up.
- **GET_ENGINES_V2**: `w[14]=0x0b` = 11 engines (GR0/COPY0-4/NVDEC0/NVENC0/SW/
  SEC2/OFA) — graphics/CE/codec; **no display engine** (display is reached via the
  object tree, F06.5/F06.8), so not on the first-pixel path.
- **BUS_GET_PCI_BAR_INFO**: `w[14]=4` BARs; recognizable values BAR0 `0xfb000000`
  size `0x01000000` (16 MB), BAR1 `0xd0000000` size `0x10000000` (256 MB), and a
  `0x02000000` (32 MB) — match §3.
- **DISPLAY_GET_IP_VERSION** (`ctrl2080internal.h:912`): params `{ NvU32 ipVersion; }`
  ⇒ **`ipVersion = 0x04010000`** ⇒ **[INFERENCE]** NVDISPLAY 4.x ⇒ the
  `NVC670/NVC67D` class family used in F06.8/F07.

> **[INFERENCE]** Exact field *boundaries* inside FINN-serialized `params[]`
> (CHIP_INFO `bar1Size`, per-BAR record layout) are inferred; the *values* above
> are unambiguous and match §3.

**Essential to consume for first pixel:** `BUS_GET_PCI_BAR_INFO` and
`DISPLAY_GET_IP_VERSION`. `CHIP_INFO` is a useful sanity check. The rest
(`*_DEVICE_INFO_TABLE`, `CONSTRUCTED_FALCON_INFO`, `USER_REGISTER_ACCESS_MAP`,
`NVLINK_*`) are graphics/CE/FIFO bring-up — issue them (GSP/driver flow expects
the sequence) but don't gate a pixel on their output.

---

### F06.5 — RM object/handle model

#### Class hierarchy (parent → child) — CORRECTIONS #4 applied
**[EVIDENCE]** Class ids (`SRC/src/nvidia/generated/g_allclasses.h`):

| Class | Id | g_allclasses.h |
|---|---|---|
| `NV01_ROOT` (client) | `0x00000000` | `:192` |
| `NV01_ROOT_CLIENT` | `0x00000041` | `:212` |
| `NV01_DEVICE_0` | `0x00000080` | `:224` |
| `NV20_SUBDEVICE_0` | `0x00002080` | `:228` |
| `NV04_DISPLAY_COMMON` | `0x00000073` | `:498` |
| `NV40_I2C` | `0x0000402c` | `:822` |

Tree: **client(`NV01_ROOT`) → device(`NV01_DEVICE_0`) → subdevice(`NV20_SUBDEVICE_0`)**.
**[EVIDENCE — CORRECTION #4]** `NV04_DISPLAY_COMMON` is parented to the **DEVICE,
not the subdevice** — its `hParent` is the device handle in every observed client:
`hParent=0xcaf00000` (device) at `…decoded-full.txt:43` and `:334`, and
`hParent=0x00010001` (device) at `:406`. So DISPLAY_COMMON is a **sibling of the
subdevice**. (`NV40_I2C`, by contrast, *is* a child of the subdevice —
`hParent=0xcaf00001` at `…decoded-full.txt:331`.)

#### Handle scheme — three client namespaces + one object generator
**[EVIDENCE]** Bases:
- `RS_CLIENT_HANDLE_BASE = 0xC1D00000` — external/user clients
  (`SRC/src/nvidia/inc/libraries/resserv/resserv.h:137`).
- `RS_CLIENT_INTERNAL_HANDLE_BASE = 0xC1E00000` — CPU-RM internal clients
  (`resserv.h:140`).
- `RS_UNIQUE_HANDLE_BASE = 0xcaf00000` — RM auto-generated object handles within a
  client (`SRC/src/nvidia/inc/libraries/resserv/rs_client.h:36`).

**[EVIDENCE]** Observed, decoded:
- **External clients** `0xc1d00000`, `0xc1d00007…0xc1d00009` — userspace
  detection / nvkms (`…decoded-full.txt:290,325,379,388`).
- **CPU-RM internal probe clients** `0xc1e00001…0xc1e00006` — kernel subsystem
  helpers; each allocates its own ROOT→DEVICE→SUBDEVICE tree
  (`…decoded-full.txt:37,76,126,159,210,245`). These are **not**
  `pGpu->hInternalClient`.
- **GSP-shared internal client** `0xc2000005` with subdevice `0xabcd2080` — from
  fn=65 (F06.3); used only for `NV2080_CTRL_CMD_INTERNAL_*`.
- **Object handles**: RM resets the `0xcaf0xxxx` generator inside each client
  (`0xcaf00000` device, `0xcaf00001` subdevice, …); nvkms instead uses its own
  base `0x00010000` (`…decoded-full.txt:388-826`).

**[INFERENCE]** StelluxOS scheme: pick any unique external client handle (RM uses
`0xC1D0xxxx`), use a per-client object counter for child handles (any scheme; RM
uses `0xcaf0xxxx` or nvkms's `0x0001xxxx`), and **read the GSP internal handles
from fn=65** rather than hardcoding.

---

### F06.6 — `GSP_RM_CONTROL` (fn=76) mechanics — `rpcRmApiControl_GSP`

**[EVIDENCE]** Signature (`rpc.c:1570-1578`):
`rpcRmApiControl_GSP(pRmApi, hClient, hObject, cmd, pParamStructPtr, paramsSize)`.
Mirrors userspace `NVOS54_PARAMETERS`
(`SRC/src/common/sdk/nvidia/inc/nvos.h:2171-2180`):
`{ NvHandle hClient; NvHandle hObject; NvV32 cmd; NvU32 flags; NvP64 params; NvU32 paramsSize; NvV32 status; }`.

Flow:
1. `rmapiutilGetControlInfo(cmd,…)` + cacheability (`rpc.c:1614-1615`); then our
   `RVGPAY ctrl cmd=.. sz=.. w=<≤16 u32>` dump of the param struct **before**
   serialization (`rpc.c:1621-1627`).
2. **NVOS54 flags** drive serialization (read from call context, `rpc.c:1640-1643`).
   If `NVOS54_FLAGS_FINN_SERIALIZED` is set the buffer is already serialized
   (`rpc.c:1645-1648`); otherwise `serverSerializeCtrlDown` serializes it here
   (`rpc.c:1651`). **[EVIDENCE]** Flags (`nvos.h:2165-2168`): `NONE=0x0`,
   `IRQL_RAISED=0x1`, `LOCK_BYPASS=0x2`, `FINN_SERIALIZED=0x4`.
3. Cacheable controls short-circuit via `rmapiControlCacheGet` (`rpc.c:1663-1668`).
4. Header `NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL` (`rpc.c:1677`); then
   `rpc_params->hClient/hObject/cmd/paramsSize/flags` (`rpc.c:1680-1684`); then our
   `RVGTRACE ctrl` line (`rpc.c:1686-1687`).
5. Params `portMemCopy`'d into `rpc_params->params` (`rpc.c:1714`); **big payloads**
   (> one message) use `large_message_copy` + `_issueRpcAndWaitLarge`
   (`rpc.c:1696-1703`,`:1743`) — seen as the `len=8260/…` controls in the trace.
   Otherwise sync `_issueRpcAndWait` (`rpc.c:1747`).
6. On return: check `rpc_params->status` (`rpc.c:1758-1760`); results are
   deserialized (`serverDeserializeCtrlUp`, `rpc.c:1776`) or flat-copied back
   (`rpc.c:1785`).

**[INFERENCE] `cmd` encoding** (observed cmd vs target class): the high 16 bits of
`cmd` select the target object's class, so `cmd` and `hObject` must agree:
`0x2080xxxx`→subdevice (`hObject`=`0xabcd2080` or `0xcaf00001`),
`0x0073xxxx`→DISPLAY_COMMON, `0x0080xxxx`→device, `0x402cxxxx`→`NV40_I2C`,
`0x5070xxxx`/`0xc370xxxx`→display objects. Consistent with the trace (e.g.
`cmd=0x00730120` on DISPLAY_COMMON, F06.8).

---

### F06.7 — `GSP_RM_ALLOC` (fn=103) mechanics — `rpcRmApiAlloc_GSP`

**[EVIDENCE]** Signature (`rpc.c:1830-1839`):
`rpcRmApiAlloc_GSP(pRmApi, hClient, hParent, hObject, hClass, pAllocParams, allocParamsSize)`.
Wire layout `rpc_gsp_rm_alloc_v03_00` (`rpc.c:1846`). Fill order:
1. Resolve param size + NULL-acceptance:
   `rmapiGetClassAllocParamSize(&paramsSize, pAllocParams, &bNullAllowed, hClass)`
   (`rpc.c:1868`; decl `SRC/src/nvidia/inc/kernel/rmapi/alloc_size.h:35`).
   **[EVIDENCE]** Special case: for `NV01_ROOT`/`NV01_ROOT_CLIENT`, paramsSize is
   forced to `sizeof(NV0000_ALLOC_PARAMETERS)` (`rpc.c:1874-1877`).
   **[EVIDENCE]** NULL params are rejected unless the class's `bNullAllowed` is set
   (`rpc.c:1879-1884`) — this is the runtime gate behind "class 0x73 takes NULL".
2. Header `NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC` (`rpc.c:1887`); then
   `rpc_params->hClient/hParent/hObject/hClass/flags` (`rpc.c:1891-1895`).
   → our hook `RVGTRACE alloc hClass=.. hParent=.. hObject=..` (`rpc.c:1897-1898`)
   — note it prints `hClass/hParent/hObject` but **not** `hClient` (so alloc-param
   *values* are not in the trace).
3. If `paramsSize>0`: `serverSerializeAllocDown` (`rpc.c:1905`),
   `RMAPI_ALLOC_FLAGS_SERIALIZED` propagated (`:1906-1909`), `portMemCopy` into
   `rpc_params->params` (`:1911`).
4. Sync `_issueRpcAndWait` (`rpc.c:1925`); on success `serverDeserializeAllocUp`
   (`:1930`) / flat copy-back (`:1933`).

**[EVIDENCE]** All fn=103 sends are `len=64` on the wire (fixed alloc header;
params ride separately).

**[EVIDENCE+INFERENCE] Client handle for `NV01_ROOT`:** the new client travels in
`NV0000_ALLOC_PARAMETERS.hClient` (the param struct), **not** in the RPC
`hObject` — the trace shows `NV01_ROOT` with `hParent=0/hObject=0`
(`…decoded-full.txt:34,322,385`) while the **next** device alloc names the client
as its `hParent` (`…:37,325,388`). At the RPC level `rpc_params->hClient = hClient`
is also set (`rpc.c:1891`); for a client alloc that arg equals the new client
**[INFERENCE]** (not in the trace; the hook omits `hClient`).

---

### F06.8 — Minimal display-object allocation catalog (parent/child + real values)

**[EVIDENCE]** The display tree is allocated by the nvkms client (object base
`0x00010000`, `…decoded-full.txt:388-826`). Verified parent/child:

```
NV01_ROOT            (client, e.g. external 0xc1d00000)             full:385
└─ NV01_DEVICE_0          hObject=0x00010001  hParent=<client>      full:388
   ├─ NV20_SUBDEVICE_0    hObject=0x00010002  hParent=0x00010001    full:391
   │  └─ NV01_EVENT_KERNEL_CALLBACK_EX 0x00010003 hParent=0x00010002 full:397 (skippable)
   ├─ NV01_MEMORY_VIRTUAL hObject=0x00010004  hParent=0x00010001    full:403
   ├─ NV04_DISPLAY_COMMON hObject=0x0001000d  hParent=0x00010001    full:406  (DEVICE child!)
   ├─ NVC372_DISPLAY_SW   hObject=0x00010010  hParent=0x00010001    full:634  (DEVICE child)
   └─ NVC670_DISPLAY      hObject=0x00010011  hParent=0x00010001    full:637  (DEVICE child)
      ├─ NVC67D_CORE_CHANNEL_DMA      0x00010016 hParent=0x00010011 full:646
      ├─ NVC67E_WINDOW_CHANNEL_DMA ×8 0x1001b…0x10061 hParent=0x00010011 full:712-796
      ├─ NVC67B_WINDOW_IMM_CHANNEL_DMA ×8            hParent=0x00010011 full:718-802
      └─ NVC67A_CURSOR_IMM_CHANNEL_PIO ×4 0x10067…0x1006a hParent=0x00010011 full:808-826
```

**[EVIDENCE — parent rules]** `NV04_DISPLAY_COMMON`, `NVC372_DISPLAY_SW`,
`NVC670_DISPLAY` are **children of the DEVICE** (CORRECTION #4); **all display
channels** (`c67d/c67e/c67b/c67a`) are **children of `NVC670_DISPLAY`**
(`hParent=0x00010011`), not the device.

#### Per-class catalog (id, parent, alloc-params struct, the values you must set)

| # | Class (id) | Parent | Alloc-params struct | **Real values to set** | First-pixel |
|---|---|---|---|---|---|
| 1 | `NV01_ROOT` (`0x0`) | none (`hParent=0`) | `NV0000_ALLOC_PARAMETERS` (`cl0000.h:47-51`) | **`hClient`** = new client (must be first member, `:48`); paramsSize forced (`rpc.c:1874-1877`) | **ESSENTIAL** |
| 2 | `NV01_DEVICE_0` (`0x80`) | client | `NV0080_ALLOC_PARAMETERS` (`cl0080.h:54-64`) | `deviceId=0` [INF]; rest 0 (`vaMode=0`=OPTIONAL_MULTIPLE, `nvos.h:2404`) | **ESSENTIAL** |
| 3 | `NV20_SUBDEVICE_0` (`0x2080`) | device | `NV2080_ALLOC_PARAMETERS` (`cl2080.h:43-45`) | `subDeviceId=0` [INF] | **ESSENTIAL** |
| 4 | `NV01_MEMORY_VIRTUAL` (`0x70`) | device | `NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS` (`cl0070.h:66-70`) | all-zero (default VA space, max limit) [INF] | supporting (keep) |
| 5 | `NV04_DISPLAY_COMMON` (`0x73`) | **device** | **none** (`cl0073.h:32`, no struct) | **NULL** (verified, F06.0) | **ESSENTIAL** |
| 6 | `NVC372_DISPLAY_SW` (`0xc372`) | device | none (`clc372sw.h:30`) | **NULL** | skippable (IMP only) |
| 7 | `NVC670_DISPLAY` (`0xc670`) | device | `NVC670_ALLOCATION_PARAMETERS{numHeads,numSors,numDsis}` (`clc670.h:36-40`) | **NULL passed** (RM/GSP defaults; read counts back via `NV0073_..._GET_NUM_HEADS`) | **ESSENTIAL** |
| 8 | `NVC67D_CORE_CHANNEL_DMA` (`0xc67d`) | `NVC670_DISPLAY` | `NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS` (`nvos.h:2417-2432`) | **`channelInstance=0`**, **`hObjectBuffer`**=core PB ctxdma, `offset=0`, `flags=0` (=CONNECT_PB_AT_GRAB_YES, `nvos.h:2428-2430`); `pControl` OUT | **ESSENTIAL ×1** |
| 9 | `NVC67E_WINDOW_CHANNEL_DMA` (`0xc67e`) | `NVC670_DISPLAY` | same `NV50VAIO_CHANNELDMA_…` (`clc67e.h` has no params) | **`channelInstance=N`** (window 0..7), **`hObjectBuffer`**=that window's PB ctxdma | **ESSENTIAL ×1 (window 0)** |
| 10 | `NVC67B_WINDOW_IMM_CHANNEL_DMA` (`0xc67b`) | `NVC670_DISPLAY` | same `NV50VAIO_CHANNELDMA_…` | per-window | skippable |
| 11 | `NVC67A_CURSOR_IMM_CHANNEL_PIO` (`0xc67a`) | `NVC670_DISPLAY` | `NV50VAIO_CHANNELPIO_ALLOCATION_PARAMETERS` (`nvos.h:2434-2440`) | `channelInstance=head`; observed `hObjectNotify=0` | skippable |

Display class ids also in `g_allclasses.h`: c372 `:482`, c670 `:530`, c67a `:538`,
c67b `:542`, c67d `:546`, c67e `:550`. **[INFERENCE]** alloc-param *values* (rows
2,3,8,9) are not on the wire (the alloc hook omits params); struct *layouts* are
**[EVIDENCE]** from the headers.

**Which structs actually carry values you must populate:** only **4** —
`NV0000_ALLOC_PARAMETERS.hClient`, `NV0080_ALLOC_PARAMETERS.deviceId(=0)`,
`NV2080_ALLOC_PARAMETERS.subDeviceId(=0)`, and per-DMA-channel
`NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS{channelInstance, hObjectBuffer}`.
Everything else is NULL or all-zero defaults.

#### Channel pushbuffer registration (the memory binding) — D04.3
**[EVIDENCE]** Immediately **before every channel `GSP_RM_ALLOC`**, the driver
issues `NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER` (`cmd=0x20800a58`) on
the **internal** subdevice `hObject=0xabcd2080` (`…decoded-full.txt:643→646` core,
repeating through `:823→826`). Params
`NV2080_CTRL_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER_PARAMS`
(`ctrl2080internal.h:1361-1373`, 40 bytes):
`{ NvU32 addressSpace; NvU64 physicalAddr; NvU64 limit; NvU32 cacheSnoop; NvU32 hclass; NvU32 channelInstance; NvBool valid; }`.

**[EVIDENCE]** Decoded core registration (`CAP/payload-trace.txt:154`,
`cmd=0x20800a58 sz=40`): `addressSpace=0x1` (`ADDR_SYSMEM`; enum 0/1/2 =
UNKNOWN/SYSMEM/FBMEM, `ctrl0080fb.h:167-169`), `physicalAddr=0xc7e8e000`,
`limit=0xfff` (4 KB), `hclass=0xc67d`, `channelInstance=0`, `valid=1`. Window #0
is identical with `hclass=0xc67e`; PIO cursor registers with `valid=0` / no
pushbuffer.

**[INFERENCE → implementer]** EVO pushbuffers here are **sysmem, 4 KB** each. For
each DMA channel: allocate a sysmem pushbuffer, emit this `0x20800a58` control
(`addressSpace=1`, `physicalAddr`=PB phys, `limit`=size−1, `hclass`=channel class,
`channelInstance`, `valid=1`) **before** the channel `_ALLOC`, else the pusher
won't fetch methods. The channel's `hObjectBuffer`/`hObjectNotify` are CPU-RM-side
ctxdma objects (the handle gaps in the tree) and do **not** appear as fn=103
allocs.

#### Exact minimal allocation order (first pixel, head 0 + window 0)
**[EVIDENCE order]** (`…decoded-full.txt:385-646`), interleaving the mandatory
pre-alloc pushbuffer control:

```
1.  GSP_RM_ALLOC  NV01_ROOT            params NV0000_ALLOC_PARAMETERS{ hClient=<client> }   # hParent=0 hObject=0
2.  GSP_RM_ALLOC  NV01_DEVICE_0        params NV0080_ALLOC_PARAMETERS{ deviceId=0 }          # hParent=<client>
3.  GSP_RM_ALLOC  NV20_SUBDEVICE_0     params NV2080_ALLOC_PARAMETERS{ subDeviceId=0 }       # hParent=<device>
4.  GSP_RM_ALLOC  NV01_MEMORY_VIRTUAL  params {0,0,0}                                         # hParent=<device> (surface VA)
5.  GSP_RM_ALLOC  NV04_DISPLAY_COMMON  params NULL                                            # hParent=<device>  (NOT subdevice)
6.  GSP_RM_ALLOC  NVC670_DISPLAY       params NULL                                            # hParent=<device>  -> <disp>
    (CPU-side: allocate core pushbuffer in sysmem)
7.  GSP_RM_CONTROL 0x20800a58 on <0xabcd2080>   { SYSMEM, physAddr, limit, hclass=0xc67d, inst=0, valid=1 }
8.  GSP_RM_ALLOC  NVC67D_CORE_CHANNEL_DMA   params {channelInstance=0, hObjectBuffer=<core PB>, offset=0, flags=0}  # hParent=<disp>
    (CPU-side: allocate window0 pushbuffer in sysmem)
9.  GSP_RM_CONTROL 0x20800a58 on <0xabcd2080>   { SYSMEM, physAddr, limit, hclass=0xc67e, inst=0, valid=1 }
10. GSP_RM_ALLOC  NVC67E_WINDOW_CHANNEL_DMA params {channelInstance=0, hObjectBuffer=<win0 PB>, offset=0, flags=0}  # hParent=<disp>
```

Then map each channel's 4 KB control page (PUT@0x0/GET@0x4), push the modeset
(CORE) + surface bind (WINDOW), and `UPDATE` each (F07/F08). The first display
control to confirm the path is live is
`NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED cmd=0x00730120` on the DISPLAY_COMMON object
(`…decoded-full.txt:340`).

---

### Minimal-path notes (first pixel on GA102)

- **Essential RPCs:** the four early RPCs `72→73→1→65` in order. fn=65 is
  mandatory (internal handles + display caps come from it). fn=73 may carry an
  (almost) empty `PACKED_REGISTRY_TABLE` but the RPC itself must be sent.
- **Essential allocs (6 objects + 2 pushbuffer controls):** `NV01_ROOT`,
  `NV01_DEVICE_0`, `NV20_SUBDEVICE_0`, `NV04_DISPLAY_COMMON` (NULL),
  `NVC670_DISPLAY` (NULL), `NVC67D_CORE_CHANNEL_DMA` (inst 0),
  `NVC67E_WINDOW_CHANNEL_DMA` (inst 0) — each DMA channel preceded by its
  `0x20800a58` pushbuffer registration. Keep `NV01_MEMORY_VIRTUAL` (cheap).
- **Skippable for first pixel:** `NVC372_DISPLAY_SW` (IMP — hardcode a known-good
  2560×1440 timing instead); windows 1–7; all `NVC67B`; all `NVC67A` cursors;
  `NV01_EVENT_KERNEL_CALLBACK_EX`; `NV40_I2C` (DP EDID comes via AUX). The
  `0xc1e00002…6` probe clients and the `NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_*`
  storm are graphics/CE bring-up, not display.
- **Plumbing:** implement the large-payload path (`_issueRpcAndWaitLarge`) — some
  display static-info controls exceed one message. Controls without
  `FINN_SERIALIZED` are serialized in-path by `serverSerializeCtrlDown`; a
  from-scratch client may pass flat structs and let RM serialize.
- **Robustness:** always test header `rpc_result` (`w[4]`) and per-control
  `status` (`w[11]`) separately; `0x56` (NOT_SUPPORTED) on NVLink/optional units
  is benign.

---

### Evidence cited

Source — `SRC/src/nvidia/kernel/vgpu/nv/rpc.c` (verified current line numbers):
- `:150,189-190,226-227,231-237,259,266-267,466,480` — transport helpers + RVGTRACE/RVGRESP hooks.
- `:687,753-759,770-772,792-794,796,798-857` — `RmRpcSetGuestSystemInfo` (fn=1) + version handshake.
- `:1070,1082-1092,1100,1104` — `rpcGetGspStaticInfo_v14_00` (fn=65) + portMemCopy.
- `:1280,1292,1311-1377,1379` — `rpcGspSetSystemInfo_v17_00` (fn=72).
- `:1391,1407,1414,1429-1451,1455` — `rpcSetRegistry_v17_00` (fn=73).
- `:1570-1578,1586,1614-1615,1621-1627,1640-1654,1663-1668,1677,1680-1687,1696-1703,1714,1743,1747,1758-1760,1776,1785` — `rpcRmApiControl_GSP` (fn=76).
- `:1830-1839,1846,1868,1874-1877,1879-1884,1887,1891-1898,1905-1912,1925,1930-1933` — `rpcRmApiAlloc_GSP` (fn=103).

Source — structs / ids / constants:
- `SRC/src/nvidia/generated/g_rpc-message-header.h:41-52` — `rpc_message_header_v03_00` (w[0..7]; payload at w[8]).
- `SRC/src/nvidia/generated/g_rpc-structures.h:36-49,156-159,243-252` — fn=1 / fn=65 (`{NvU32 data}`) / fn=76 payload structs.
- `SRC/src/nvidia/inc/kernel/gpu/gsp/gsp_static_config.h:72-148` (`GspStaticConfigInfo`; `:74` grCapsBits, `:75` gidInfo, `:92-97` FB, `:131-133` displayless, `:136/:139/:142` hInternalClient/Device/Subdevice), `:151-181` (`GspSystemInfo`).
- `SRC/src/nvidia/src/kernel/gpu/gpu.c:764,773-782,783-791,793-799` — `_gpuAllocateInternalObjects` (Tegra vs dGPU paths).
- `SRC/src/nvidia/src/kernel/gpu/gpu_gspclient.c:272-277` — internal controls use `hInternalClient`+`hInternalSubdevice`.
- `SRC/src/nvidia/generated/g_gpu_nvoc.h:836-837` — `NV_GPU_INTERNAL_DEVICE_HANDLE=0xABCD0080`, `…SUBDEVICE_HANDLE=0xABCD2080`.
- `SRC/src/nvidia/inc/libraries/resserv/resserv.h:137,140` — client handle bases `0xC1D00000`/`0xC1E00000`; `rs_client.h:36` — `RS_UNIQUE_HANDLE_BASE 0xcaf00000`.
- `SRC/src/nvidia/generated/g_allclasses.h:192,212,224,228,482,498,530,538,542,546,550,822` — class ids.
- `SRC/src/common/sdk/nvidia/inc/class/cl0000.h:47-51` (NV0000), `cl0080.h:54-64` (NV0080), `cl2080.h:43-45` (NV2080), `cl0073.h:32` (no params), `cl0070.h:66-70` (MEMORY_VIRTUAL), `clc372sw.h:30`, `clc670.h:32,36-40` (NVC670 params), `clc67d.h:34`, `clc67e.h:34`.
- `SRC/src/common/sdk/nvidia/inc/nvos.h:2165-2168` (NVOS54 flags), `:2171-2180` (NVOS54_PARAMETERS), `:2404` (vaMode), `:2417-2432` (CHANNELDMA params + CONNECT_PB_AT_GRAB `:2428-2430`), `:2434-2440` (CHANNELPIO params).
- `SRC/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080internal.h:564,912,1361-1373` — CHIP_INFO / DISPLAY_GET_IP_VERSION / DISPLAY_CHANNEL_PUSHBUFFER (cmd + params).
- `SRC/src/common/sdk/nvidia/inc/ctrl/ctrl0080/ctrl0080gr.h:78` — `NV0080_CTRL_GR_CAPS_TBL_SIZE=23`; `ctrl0080fb.h:167-169` — ADDR_UNKNOWN/SYSMEM/FBMEM=0/1/2.
- `SRC/src/common/sdk/nvidia/inc/nvstatuscodes.h:115` — `NV_ERR_NOT_SUPPORTED=0x56`.
- `SRC/src/nvidia/inc/kernel/rmapi/alloc_size.h:35` — `rmapiGetClassAllocParamSize(... pBAllowNull, hClass)`.
- `SRC/src/nvidia/kernel/inc/vgpu/rpc_global_enums.h:10,74,81,82,85,112` — fn ids 1/65/72/73/76/103.

Traces / decoded:
- `AN/20260530-110235-open-capture-rpc-decoded.txt:1-6` — early RPC order (72→73→1→65; 1/65 sync).
- `CAP2/rpc-resp-trace.txt:1,2,3,6,9,10,11` — fn=1 reply ("535.183.01", vgx 35.5); fn=65 reply (grCapsBits + GID UUID); fn=76 CHIP_INFO (`c2000005`/`abcd2080`, dev `0x2216`/`0x10de`, sub `0x403f`/`0x1458`, rev `0xa1`), NVLINK status `0x56`, ENGINES, BAR map, IP_VERSION `0x04010000`.
- `CAP2/nvidia-smi-q.txt:26` — GPU UUID `GPU-20db158d-55bb-92ce-7bad-4a25b3185611` (fn=65 GID cross-check).
- `AN/20260530-110540-open-capture-decoded-full.txt:7-31,34-44,322-343,385-409,634-646,712-826` — internal-control names, handle values, parent/child for the whole tree (DISPLAY_COMMON `hParent=0xcaf00000`/`0x00010001`=device `:43,:334,:406`; I2C `hParent=subdevice` `:331`; channels `hParent=NVC670` `:646`; first display ctrl `cmd=0x00730120` `:340`).
- `CAP/payload-trace.txt:154` — core `0x20800a58` decode (SYSMEM, phys `0xc7e8e000`, limit `0xfff`, hclass `0xc67d`, inst 0, valid 1).

---

### Open questions / TODO

- **[EVIDENCE-GAP]** fn=65 fields past the 96-byte `RVGRESP` window (`fb_length`,
  `fbio_mask`, `fb_ram_type`, `bar1PdeBase`, `displaylessMax*`, **`hInternalDevice`**)
  are not directly captured. **TODO:** widen the dump (`rpc.c:231-237`) or add a
  targeted `GspStaticConfigInfo` dump at offsets `:92`,`:131-133`,`:139` to capture
  FB size, display caps, and `hInternalDevice` on-wire (FB total is currently only
  [INFERENCE] from `nvidia-smi-q.txt:95`, 10240 MiB).
- **[TODO — values]** `NV01_DEVICE_0`/`NV20_SUBDEVICE_0`/channel alloc-param
  *values* are [INFERENCE] (the `RVGTRACE alloc` hook prints only
  `hClass/hParent/hObject`). To upgrade to [EVIDENCE], extend the hook to dump
  `pAllocParams` or read the nvkms fill sites (`nvkms-rm.c`/`nvkms-evo.c`, F07/F08).
- **[TODO]** Confirm `NV0000_ALLOC_PARAMETERS` travels the client handle in
  `params.hClient` by dumping the ROOT alloc params on the wire (currently
  [INFERENCE] from the device `hParent`; RPC-level `rpc_params->hClient` is set at
  `rpc.c:1891` but not traced).
- **[TODO]** `NVC670_ALLOCATION_PARAMETERS` defaults when NULL is passed — confirm
  GA102 `numHeads/numSors/numDsis` from the RM-side ctor; for bring-up read counts
  via `NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS` (F07/F08).
- **[TODO]** `hObjectNotify` for DMA channels: verify a minimal client may pass
  `hObjectNotify=0` (cursor PIO uses 0; core/window notifier ctxdma was not exposed
  separately in the capture).
- **[INFERENCE]** `cmd` high-16-bits→class encoding (F06.6) and FINN field
  boundaries inside `params[]` (CHIP_INFO/BAR_INFO) are inferred; confirm against
  the FINN descriptor (`g_finn_rm_api.h`) and request-side `RVGPAY` dumps.
- **[UNCERTAIN]** Why `hInternalClient=0xc2000005` specifically — GSP-RM-assigned;
  the host must treat it as opaque and read it from fn=65 (do not hardcode).
- **[TODO — fn=72 verify]** `GspSystemInfo.gpuPhysAddr` vs BAR0 `0xfb000000`: fn=72
  is async and not in `payload-trace.txt`; capture it to confirm equality.


## F07 — Display Engine Objects & Channels

Merged + judge-verified from draft `S08` (object model / channel / pushbuffer / UPDATE
mechanics) and the **channel parts** of draft `D04` (GSP_RM_ALLOC catalog: channel handles,
alloc-param fields, pushbuffer registration, minimal order). Scope: the GA102/NVDisplay
(class family `C6`) display object tree, the minimal object set to drive **one head + one
window**, the EVO DMA-pushbuffer model (method-header encoding, PUT/kickoff, UPDATE/commit),
the channel↔head/window mapping, and the `NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER`
registration control.

Paths are relative to `SRC=/home/flare/dev/gpu-repro/open-gpu-kernel-modules`,
`AN=/home/flare/dev/gpu-repro/analysis`, `CAP=/home/flare/dev/gpu-repro/traces/20260530-112551-open-capture`.
Cross-refs (not duplicated here): the RPC/`GSP_RM_ALLOC` wire mechanics + handle scheme =
**F-RPC (S07)**; the non-channel root objects (`NV01_ROOT`/`NV01_DEVICE_0`/`NV20_SUBDEVICE_0`/
`NV01_MEMORY_VIRTUAL`) alloc params = **D04**; the per-method modeset programming
(`HEAD_SET_*`, window `SET_*`) = **F-modeset (S09/S10)**. This section owns the
object/channel/pushbuffer **plumbing**.

> **Judge note (verification status).** Every register offset, opcode, struct field, method
> offset, class id, and control id below was re-verified against the cited headers/source.
> All *values/mechanisms* in S08+D04 hold. The only substantive corrections are **line-number
> drift in the two files that carry OUR instrumentation** (`nvkms-dma.h`, `nvkms-dma.c`): S08's
> line numbers there were systematically low (because S08 did not account for the added
> `rvg_evo_*` variables and the second `RVGEVOD` data-word hook). All line numbers below are the
> corrected, re-read values. CORRECTIONS #5 (decoder caveat) and #9 (no `MEMORY_LAYOUT`) are
> applied.

---

### F07.0 — TL;DR object/channel map (GA102, Ampere `C6`)

```
NV01_ROOT (client)                                            [D04, S07]
└─ NV01_DEVICE_0            (pDevEvo->deviceHandle)
   ├─ NV20_SUBDEVICE_0      (pDevEvo->pSubDevices[sd]->handle) — maps channel control regs
   ├─ NV01_MEMORY_VIRTUAL   (device VA; supporting)
   ├─ NV04_DISPLAY_COMMON   (0x0073)  — detect/EDID/IMP RPC target (S07/S09)   ── child of DEVICE
   ├─ NVC372_DISPLAY_SW     (0xc372)  — IS_MODE_POSSIBLE/IMP object (skippable) ── child of DEVICE
   └─ NVC670_DISPLAY        (0xc670)  — "Evo object: parent of all display stuff" ── child of DEVICE
      ├─ NVC67D_CORE_CHANNEL_DMA       (0xc67d) ×1   instance 0          channelMask bit0
      ├─ NVC67E_WINDOW_CHANNEL_DMA     (0xc67e) ×8   instance=win        channelMask bit(1+win)
      ├─ NVC67B_WINDOW_IMM_CHANNEL_DMA (0xc67b) ×8   instance=win (skippable)
      └─ NVC67A_CURSOR_IMM_CHANNEL_PIO (0xc67a) ×4   instance=head (skippable) channelMask bit(33+head)
```

**[EVIDENCE]** Class ids: `clc670.h:32` (`NVC670_DISPLAY=0xc670`), `clc67d.h:34`
(`NVC67D_CORE_CHANNEL_DMA=0xc67d`), `clc67e.h:34` (`NVC67E_WINDOW_CHANNEL_DMA=0xc67e`),
`clc67b.h:34` (`NVC67B_WINDOW_IMM_CHANNEL_DMA=0xc67b`), `clc67a.h:32`
(`NVC67A_CURSOR_IMM_CHANNEL_PIO=0xc67a`), `clc372sw.h:30` (`NVC372_DISPLAY_SW=0xc372`).
**[EVIDENCE]** Observed instances on this GA102: core ×1, window ×8, window-imm ×8, cursor ×4
(⇒ 4 heads / 8 windows) — `AN/...decoded-full.txt:646,712-796,718-802,808-826`. Matches
CONTEXT_BRIEF §4c.

---

### F07.1 — Minimal display object set + parent rules

**Parent rules [EVIDENCE — confirms CORRECTIONS #4 / D04.1].** The three display objects are
children of the **DEVICE** (siblings of the subdevice); the channels are children of
`NVC670_DISPLAY`.

- `NV04_DISPLAY_COMMON`, `NVC372_DISPLAY_SW`, `NVC670_DISPLAY` → `hParent = device (0x00010001)`
  — `AN/...decoded-full.txt:406,634,637`.
- All channels (`c67d/c67e/c67b/c67a`) → `hParent = NVC670_DISPLAY (0x00010011)` —
  `AN/...decoded-full.txt:646-826`.

**`NV04_DISPLAY_COMMON` (0x0073) — ESSENTIAL, NULL params.**
[EVIDENCE] Allocated under `pDevEvo->deviceHandle` with class `NV04_DISPLAY_COMMON` and
`NULL` params at `nvkms-rm.c:1603-1611` (include `class/cl0073.h` at `nvkms-rm.c:50`). If the
class is unavailable the device fails with `NVKMS_ALLOC_DEVICE_STATUS_NO_HARDWARE_AVAILABLE`
(`nvkms-rm.c:1625`). `cl0073.h` defines only the class id (no `*_ALLOC_PARAMETERS` struct), so
`NULL` is the correct/observed choice (D04.2 #4). Control target for detection/EDID (S07/S09).

**`NVC372_DISPLAY_SW` (0xc372) — SKIPPABLE, NULL params.**
[EVIDENCE] `clc372sw.h:30` defines only the class id (no FINN params). Allocated under
`deviceHandle` with `NVC372_DISPLAY_SW, NULL` at `nvkms-evo3.c:6748`. Used only for IMP /
`NVC372_CTRL_CMD_IS_MODE_POSSIBLE` (S07). A hardcoded known-good 2560×1440 timing bypasses IMP
for bring-up ⇒ skippable for first pixel.

**`NVC670_DISPLAY` (0xc670) — ESSENTIAL; alloc-param struct exists but driver passes NULL.**
[EVIDENCE] Struct `NVC670_ALLOCATION_PARAMETERS { NvU32 numHeads; NvU32 numSors; NvU32
numDsis; }` at `clc670.h:36-40`. But nvkms passes `NULL`: `nvkms-evo.c:4492-4496`
(`nvRmApiAlloc(client, pDevEvo->deviceHandle, pDevEvo->displayHandle, pDevEvo->dispClass,
NULL)`), with the comment *"Evo object (parent of all other NV50 display stuff)"* at
`nvkms-evo.c:4488`. `dispClass == NVC670_DISPLAY` for GA102 (F07.7).
[INFERENCE] With `NULL` params RM/GSP supplies default head/SOR/DSI counts; a from-scratch
client may pass `NULL` and read the real counts back via `NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS`
(F07.7) rather than populating the struct. **Answer to the brief's "NULL params?": yes — NULL
is the observed/working choice even though a 3-field struct exists.**

---

### F07.2 — Channel objects + alloc-param structs (core / window / window-imm / cursor)

Two shared FINN structs in `nvos.h` are reused for **all** NVDisplay channels — the per-class
headers `clc67d/e/b/a.h` do **not** define their own allocation params.

**DMA channels (CORE `c67d`, WINDOW `c67e`, WINDOW_IMM `c67b`)** use
`NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS` [EVIDENCE `nvos.h:2417-2432`]:
```c
typedef struct {
    NvV32    channelInstance;            // 0 for CORE; window number for windows
    NvHandle hObjectBuffer;              // ctx dma handle for the DMA push buffer (REQUIRED)
    NvHandle hObjectNotify;              // ctx dma handle for error/notify area
    NvU32    offset;                     // initial PUT/GET offset, usually 0
    NvP64    pControl NV_ALIGN_BYTES(8); // OUT: virt addr of UDISP GET/PUT regs
    NvU32    flags;                      // CONNECT_PB_AT_GRAB: bit 1:1, YES=0 / NO=1
} NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS;
```
`CONNECT_PB_AT_GRAB` flag defs at `nvos.h:2428-2430` (`_YES=0x0`, `_NO=0x1`, field `1:1`) ⇒
`flags=0` means "connect PB at grab = YES".

How nvkms fills it per DMA channel [EVIDENCE `nvkms-rm.c:2648,2678-2682`]:
`channelInstance = instance` (`:2678`), `hObjectBuffer = buffer->dma.ctxHandle` (the
pushbuffer ctxdma, `:2680`), `offset = 0` (`:2682`). `pControl` is obtained afterward by
mapping the channel's 4 KB control page: `nvRmApiMapMemory(... channel_handle, 0,
dmaControlLen ...) → buffer->control[sd]` [`nvkms-rm.c:2702-2706`, `dmaControlLen = 0x1000` at
`:2604`]. The pushbuffer memory is allocated once (for `sd==0`) and shared across subdevices
(`nvkms-rm.c:2645-2659`, per S08). Generic allocator: `RmAllocEvoChannel(pDevEvo, channelMask,
instance, class)` [`nvkms-rm.c:2594`].

**PIO channel (CURSOR `c67a`)** uses `NV50VAIO_CHANNELPIO_ALLOCATION_PARAMETERS`
[EVIDENCE `nvos.h:2434-2440`]:
```c
typedef struct {
    NvV32    channelInstance;            // per head
    NvHandle hObjectNotify;              // ctx dma for errors (observed 0)
    NvP64    pControl NV_ALIGN_BYTES(8); // OUT: virt addr of PIO control region
} NV50VAIO_CHANNELPIO_ALLOCATION_PARAMETERS;
```
[EVIDENCE] Cursor alloc `nvAllocCursorEvo` (`nvkms-cursor.c:321`): loop `head < numHeads`
(`:325`), `channelInstance = head` (`:330`), class = `pDevEvo->cursorHal->klass` (`:340`) =
`NVC67A_CURSOR_IMM_CHANNEL_PIO` (`nvkms-cursor3.c:108`), parent `displayHandle`; then map the
PIO control region → `cursorPio[head]` (`:369`). PIO register layout (`Free`@0x8,
`Update`@0x200, `SetCursorHotSpotPointOut`@0x208) is `clc67a.h:34-43`. **Note:** the cursor is
PIO — methods are written by **direct MMIO stores into the mapped control struct**, not via a
DMA pushbuffer/method-header (contrast F07.3–F07.5). Skippable for first pixel.

**Observed channel handles + instances [EVIDENCE — D04.1].** All children of
`NVC670_DISPLAY (hParent=0x00010011)`:
- CORE `c67d`: `hObject=0x00010016`, `channelInstance 0` (`...decoded-full.txt:646`;
  instance from `CAP/payload-trace.txt:154` w8=0).
- WINDOW `c67e` ×8: `hObject=0x0001001b…0x00010061`, `channelInstance = 0..7`
  (`...decoded-full.txt:712-796`; `payload-trace.txt:175,177,179,181,183,185,187,189`).
- WINDOW_IMM `c67b` ×8: instances 0..7 (`...decoded-full.txt:718-802`).
- CURSOR `c67a` ×4: `hObject=0x00010067…0x0001006a`, `channelInstance = head 0..3`
  (`...decoded-full.txt:808-826`; `payload-trace.txt:191-194`).

[INFERENCE — D04.1] Object-id gaps (~5 per DMA channel, e.g. core `0x10016` → window0
`0x1001b`) are the channel's **pushbuffer memory + pushbuffer ctxdma + notifier memory +
notifier ctxdma** — CPU-RM-side objects that do **not** appear as `GSP_RM_ALLOC` (fn=103); no
`NV01_CONTEXT_DMA`/`NV01_MEMORY_SYSTEM` alloc appears for channels in the trace (the only
display-client memory alloc is `NV01_MEMORY_VIRTUAL`, `...decoded-full.txt:403`). Their
**physical** location reaches GSP via F07.8, not via alloc.

---

### F07.3 — An EVO DMA channel = a pushbuffer in memory (method-header encoding)

Each EVO **DMA** channel is a ring of 32-bit dwords in a memory surface (sysmem on this card —
F07.8), plus a small MMIO **control** region exposing `PUT`/`GET`.

- The HW reads from `GET` up to `PUT` (byte offsets into the surface). The CPU writes methods
  at the write cursor (`pb.buffer`), then advances `PUT` to publish them (F07.5).
- `pControl[sd]` (the mapped 4 KB page) holds `PUT` at offset **0x0** and `GET` at **0x4**.
  [EVIDENCE per-class: `clc67d.h:78,80` (`NVC67D_PUT=0x0`, `GET=0x4`); `clc67e.h:51,53`
  (`NVC67E_PUT=0x0`, `GET=0x4`). Generic class used by the DMA layer: `cl917d.h:767,769`
  (`NV917D_PUT=0x0`, `NV917D_GET=0x4`).]

**Method-header dword encoding** (the dword preceding the data dwords). Identical across the
NVDisplay classes and the generic `NV_UDISP_DMA_*` defs the push code actually uses:

| field | bits | meaning |
|------|------|---------|
| `OPCODE` | `31:29` | `METHOD=0`, `JUMP=1`, `NONINC_METHOD=2`, `SET_SUBDEVICE_MASK=3` |
| `METHOD_COUNT` | `27:18` | number of data dwords that follow (≤ 0x3FF) |
| `METHOD_OFFSET` | `13:2` | method's **byte** offset ≫2 (i.e. dword index) |

[EVIDENCE per-class: `clc67d.h:61-67`, `clc67e.h:38-44`, `clc67b.h:38-44` — `..._DMA_OPCODE
31:29`, `..._DMA_OPCODE_METHOD 0x0`/`_JUMP 0x1`/`_NONINC_METHOD 0x2`/`_SET_SUBDEVICE_MASK 0x3`,
`..._DMA_METHOD_COUNT 27:18`, `..._DMA_METHOD_OFFSET 13:2`.]
[EVIDENCE generic, used by the push macros — **corrected lines** `nvkms-dma.h:240-247`]:
`NV_UDISP_DMA_OPCODE 31:29` (`:240`), `_OPCODE_METHOD 0x0` (`:241`), `NV_UDISP_DMA_METHOD_COUNT
27:18` (`:242`), `NV_UDISP_DMA_METHOD_OFFSET 13:2` (`:247`). The source comment at
`nvkms-dma.h:243-246` states `METHOD_OFFSET` is `13:2` for nvdisplay (the comment says `c3*`; `c5*`/`c6*` are
likewise `13:2` per `clc67d.h:67`/`clc67e.h:44`) and
`11:2` for older classes, and nvkms always uses the wider `13:2`. (The older generic
`NV917D_DMA_METHOD_OFFSET` is indeed `11:2` at `cl917d.h:756`; the DMA layer only borrows
`NV917D` for `PUT`/`GET`/`SET_SUBDEVICE_MASK`, not for building method headers.)

Special control dwords in the same stream:
- `JUMP` to offset 0 = `0x20000000` (`OPCODE=JUMP`, `JUMP_OFFSET=0`), written to wrap the ring
  [EVIDENCE — **corrected lines** `nvkms-dma.c:198,213` write literal `0x20000000`; opcode
  `clc67d.h:63`, `JUMP_OFFSET 11:2` `clc67d.h:70`].
- `SET_SUBDEVICE_MASK` dword for SLI broadcast targeting [EVIDENCE `nvkms-dma.c:370-372`,
  built from `NV917D_DMA_OPCODE_SET_SUBDEVICE_MASK` + `..._VALUE 11:0` (`cl917d.h:763-764`)].

---

### F07.4 — Writing methods into the pushbuffer (+ our RVGEVO / RVGEVOD hooks)

Two inline primitives in `nvkms-dma.h`:

- **Start a method** — `nvDmaSetStartEvoMethod(pChannel, method, count)` writes the single
  header dword [EVIDENCE — **corrected lines** `nvkms-dma.h:253-296`]:
  - `methodDwords = method >> 2` (`:272`) with `nvAssert((method & 0x3)==0)` (`:274`) —
    methods are 4-aligned;
  - reserves room: `countPlusHeader = count + 1` (`:270`); if `fifo_free_count <=
    countPlusHeader` it calls `nvEvoMakeRoom` (`:285-287`);
  - emits header = `DRF_DEF(_UDISP,_DMA,_OPCODE,_METHOD) | DRF_NUM(_UDISP,_DMA,_METHOD_COUNT,
    count) | DRF_NUM(_UDISP,_DMA,_METHOD_OFFSET, methodDwords)` (`:289-292`);
  - decrements `fifo_free_count` by `countPlusHeader` (`:294`).
- **Append a data dword** — `nvDmaSetEvoMethodData(pChannel, data)`: `*(pb.buffer)=data;
  pb.buffer++` [EVIDENCE — **corrected lines** `nvkms-dma.h:127-128`]. The 64-bit helper
  `nvDmaSetEvoMethodDataU64` writes HI32 then LO32 [`nvkms-dma.h:131-137`].

So a method push is: `nvDmaSetStartEvoMethod(ch, METHOD_OFF, N)` then N ×
`nvDmaSetEvoMethodData(ch, ...)` then a kickoff (F07.5).

**OUR instrumentation — TWO hooks (corrected; S08 documented only the first):**
- `RVGEVO M` (per-method) — inside `nvDmaSetStartEvoMethod`, logged before the header is
  written [EVIDENCE — **corrected lines** `nvkms-dma.h:263-267`]:
  ```c
  if (rvg_evo_trace && rvg_evo_count < 200000) {
      rvg_evo_count++;
      nvEvoLog(EVO_LOG_WARN, "RVGEVO M ch=0x%08x off=0x%04x cnt=%u\n",
               pChannel->channelMask, method, count);
  }
  ```
  `ch = pChannel->channelMask`, `off = method byte offset`, `cnt = data-dword count`. Source of
  every `RVGEVO` line in `CAP/evo-trace.txt`.
- `RVGEVOD` (per-data-word) — inside `nvDmaSetEvoMethodData` [EVIDENCE `nvkms-dma.h:121-126`]:
  ```c
  if (rvg_evo_trace && rvg_evo_cur_remain > 0 && rvg_evo_dcount < 60000) {
      rvg_evo_dcount++;  rvg_evo_cur_remain--;
      nvEvoLog(EVO_LOG_WARN, "RVGEVOD off=0x%04x data=0x%08x\n", rvg_evo_cur_off, data);
      rvg_evo_cur_off += 4;
  }
  ```
  `nvDmaSetStartEvoMethod` primes `rvg_evo_cur_off = method` / `rvg_evo_cur_remain = count`
  (`nvkms-dma.h:261-262,295`), so each data word is attributed to its method's offset. This is
  the source of the `RVGEVOD` lines in `CAP2/evo-data-trace.txt` (ADDENDUM) — i.e. the EVO
  **data words** (`off=… data=…`) used to recover measured modeset values.

All five trace variables are defined in `nvkms-dma.c:33-37`
(`rvg_evo_trace = NV_TRUE` by default at `:33`, `rvg_evo_count`/`rvg_evo_cur_off`/
`rvg_evo_cur_remain`/`rvg_evo_dcount` at `:34-37`) and externed at `nvkms-dma.h:111-115,249-250`.

**Implication for reimplementation:** the decoded EVO trace is literally the sequence of
`(channelMask, method, count)` tuples the driver pushed, plus each method's data dwords;
replaying those per channel (then kicking off) reproduces the modeset.

**[Apply CORRECTIONS #5 — EVO decoder caveat.]** The `decode_evo*.py` name columns can
mislabel offsets (a parametric method like `SET_PLANAR_STORAGE(b)` matches before
`SET_CONTEXT_DMA_ISO`/`SET_OFFSET`; core `SW_*`/window collisions). **Trust the raw
`off=`/`data=` and re-map via the class headers** (`clc67d.h`/`clc67e.h`). Verified header
offsets for the framebuffer bind (window `c67e`): `SET_CONTEXT_DMA_ISO(b)=0x240+b*4`
(`clc67e.h:181`), `SET_OFFSET(b)=0x260+b*4` (`clc67e.h:183`), `SET_PARAMS=0x22C` with
`FORMAT 7:0` and `FORMAT_X8R8G8B8=0xE6` (`clc67e.h:139,140,147`),
`SET_PLANAR_STORAGE(b)=0x230+b*4` with `PITCH 12:0` (`clc67e.h:177-178`). The bind **was**
captured [EVIDENCE D07/CORRECTIONS #5]: `SET_CONTEXT_DMA_ISO[0]=0x00010087`, `SET_OFFSET=0`,
`SET_PARAMS=0xE6` (X8R8G8B8), `SET_PLANAR_STORAGE[0]=0x100`. (Window method *content* is
S09/S10 scope; only the offsets/decoder caveat are owned here.)

---

### F07.5 — Advancing PUT / kicking the channel (`nvDmaKickoffEvo` → `EvoCoreKickoff`)

[EVIDENCE — **corrected lines** `nvkms-dma.c:44-126`.] After writing methods, the channel is
"kicked":

1. `nvDmaKickoffEvo(pChannel)` computes `putOffset = (char*)pb.buffer - (char*)pb.base` (byte
   offset of the write cursor, `:47`). If equal to the last `put_offset` it returns (nothing to
   do, `:49-51`); else calls `EvoCoreKickoff` (`:53`).
2. `EvoCoreKickoff(pb, putOffset)` (`:56-126`):
   - **vidmem case only** (`pDma->isBar1Mapping`, `:65`): copy the freshly written dwords from
     the sysmem shadow into each subdevice's BAR1 view (`:83-96`), then issue
     `NV0080_CTRL_CMD_DMA_FLUSH` with `targetUnit = _FLUSH_TARGET_UNIT_FB_ENABLE` (`:102-108`)
     so data lands in FB before the GPU fetches it.
   - memory barrier: `sfence` on x86_64 (`:115`).
   - publish: `push_buffer->put_offset = putOffset` (`:121`), then **for each subdevice** write
     the PUT register: `nvDmaStorePioMethod(pControl, NV917D_PUT, putOffset)` (`:122-125`).
     `nvDmaStorePioMethod` is a relaxed atomic 32-bit store to the mapped control page
     [`nvkms-dma.h:71-83`, `__atomic_store_n(..., __ATOMIC_RELAXED)`]. **This single MMIO store
     to `PUT` (offset 0x0) is what tells the EVO pusher to advance `GET→PUT` and execute.**

`GET` is read via `nvDmaLoadPioMethod(pControl, NV917D_GET)` [`nvkms-dma.c:129-133`;
`nvkms-dma.h:85-99`, `__atomic_load_n(..., __ATOMIC_ACQUIRE)`].

**Flow control / ring wrap** = `nvEvoMakeRoom` [EVIDENCE `nvkms-dma.c:186-255`]: when the write
cursor reaches `offset_max` it writes a `JUMP`-to-0 dword (`*(buffer)=0x20000000`, `:198`),
resets `buffer=base`, kicks off, and busy-waits (`nvkms_yield`, `:253`) until `GET` frees
enough `fifo_free_count`; a second wrap path writes `0x20000000` at `:213`; 5 s timeout warning
at `:244-249`. `NV_DMA_PUSHER_CHASE_PAD = 5` dwords [`nvkms-dma.c:39`].

**[EVIDENCE]** Subdevice mask is `nvEvoSetSubdeviceMask` [`nvkms-dma.c:356-374`]; the DMA layer
uses the **generic** `NV917D_*` symbols for `PUT`/`GET`/`SET_SUBDEVICE_MASK` because this layer
is class-agnostic — per-class method offsets (`HEAD_SET_*`, etc.) come from the `clc67x.h`
headers via the HAL.

---

### F07.6 — `UPDATE` method = the commit (latch + interlock)

Every channel has an `UPDATE` at byte offset **`0x200`** [EVIDENCE `clc67d.h:82` (core),
`clc67e.h:55` (window), `clc67b.h:55` (window-imm), `clc67a.h:47` (cursor)]. Writing `UPDATE`
latches the channel's accumulated "assembly" state into "active" state — the per-frame commit.
(For the DMA channels `UPDATE` is a pushed method; for the PIO cursor it is a direct write to
the mapped control struct at 0x200.)

`UPDATE` payload fields:
- Core [EVIDENCE `clc67d.h:82-94`]: `RELEASE_ELV` (`0:0`, `:91`) releases the
  End-of-Line-Vblank gate so scanout/latch proceeds; `SPECIAL_HANDLING` (`21:20`, `:83`, e.g.
  `_MODE_SWITCH=0x2` at `:86`); `INHIBIT_INTERRUPTS` (`24:24`, `:88`); `FLIP_LOCK_PIN`
  (`8:4`, `:94`).
- Window [EVIDENCE `clc67e.h:55-95`]: `RELEASE_ELV` (`0:0`, `:56`); `INTERLOCK_WITH_WIN_IMM`
  (`12:12`, `:93`).

**Interlock = atomic multi-channel commit.** `UpdateCoreC3` builds the core `UPDATE` and, just
before it, programs `SET_INTERLOCK_FLAGS` (cursor mask) and `SET_WINDOW_INTERLOCK_FLAGS`
(window mask) so the core's `UPDATE` waits for the listed channels' UPDATEs — all latch
together [EVIDENCE `nvkms-evo3.c:2656,2685,2688`]. The driver-wide commit `EvoUpdateC3`
[`nvkms-evo3.c:2954`] computes `coreInterlockMask = updateChannelMask & ~noCoreInterlockMask`
(`:2965-2966`), calls `UpdateCoreC3` (`:2992`), then updates each pending window interlocked to
core. (nvkms uses the `C37D`/`C37E` symbol spellings — values identical to `C67x` — and the
`C6` HAL reuses `EvoUpdateC3`: `nvEvoC6.Update = EvoUpdateC3` at `nvkms-evo3.c:7880`
(struct `nvEvoC6` starts `:7871`; the same `EvoUpdateC3` is shared by `nvEvoC3` `:7722` /
`nvEvoC5` `:7801`).)

[INFERENCE] Minimal commit ordering for first pixel: program core (mode) → `UPDATE` core;
program window (surface) → `UPDATE` window. Interlock is an optimization for tear-free atomic
flips, **not required** for a static pixel; a non-interlocked window `UPDATE`
(`interlockMask=0`) is sufficient.

---

### F07.7 — Channel ↔ head/window mapping (first pixel = head 0 + window 0)

**HAL class selection (GA102 = Ampere `C6`).** The disp table is scanned and the first class
the GPU advertises wins. The `ENTRY` macro sets `.class = NV<prefix>70_DISPLAY` and
`.coreChannelClass = NV<prefix>7D_CORE_CHANNEL_DMA` [EVIDENCE `nvkms-hal.c:75-79`]; the table
has Ada `ENTRY_NVD(C7,C6,...)` (`:192`) and **Ampere `ENTRY_NVD(C6,C6,...)`** (`:194`);
selection assigns `pDevEvo->dispClass` / `coreChannelClass` (`:214-220`) ⇒ for GA102
`dispClass = NVC670_DISPLAY`, `coreChannelClass = NVC67D_CORE_CHANNEL_DMA`. Window classes are
chosen by capability, first match wins: `{ NVC67E_WINDOW_CHANNEL_DMA,
NVC67B_WINDOW_IMM_CHANNEL_DMA }` [EVIDENCE `nvkms-rm.c:3159-3160`]. Cursor class =
`NVC67A_CURSOR_IMM_CHANNEL_PIO` [`nvkms-cursor3.c:108`].

**Channel-mask bit layout** (`NVEvoChannelMask`, 64-bit) [EVIDENCE `nvkms-types.h:298-323`]:
`CORE` = bit 0 (`:298`); `WINDOW(n)` = bit `1+n` (`:302`, `__SIZE 32` `:303`); `CURSOR(n)` =
bit `33+n` (`:307`, `__SIZE 8` `:308`); `WINDOW_IMM` = bit 49 (`:322`). Helper inline
`NV_EVO_CHANNEL_MASK_WINDOW_NUMBER(mask)` extracts the window index [`nvkms-types.h:352`].

**Allocation instances.**
- Core: `RmAllocEvoChannel(pDevEvo, DRF_DEF64(_EVO,_CHANNEL_MASK,_CORE,_ENABLE), 0,
  coreChannelClass)` — mask bit0, instance 0 [EVIDENCE `nvkms-rm.c:3255-3258`].
- Windows: loop, `channelMask = WINDOW(win)`, `instance = win` [EVIDENCE `nvkms-rm.c:3185`].
- Cursors: loop `head < numHeads`, `instance = head` [EVIDENCE `nvkms-cursor.c:325,330`].

**Counts (queried, not assumed).** `numHeads` ← `NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS`
[EVIDENCE `nvkms-rm.c:893`]; `numWindows` set during probe [`nvkms-evo3.c:5887`]. GA102 ⇒
**4 heads, 8 windows** (CONTEXT_BRIEF §3/§4c; trace shows windows up to bit 8 = `WINDOW7`).

**Window→head binding.** Queried via
`NV0073_CTRL_CMD_SPECIFIC_GET_VALID_HEAD_WINDOW_ASSIGNMENT` → `windowHeadMask[win]`, stored in
`pDevEvo->headForWindow[win]` [EVIDENCE `nvkms-rm.c:953,958,998`]. Two modes:
- *flexible mapping*: `head = win >> 1` ⇒ windows `2N`/`2N+1` belong to head `N` [EVIDENCE
  `nvkms-rm.c:987-988`]. Equals the HW macros `NVC37D_WINDOW_MAPPED_TO_HEAD(w) = (w)>>1` and
  `NVC37D_GET_VALID_WINDOWMASK_FOR_HEAD(h) = (1<<(h*2)) | (1<<(h*2+1))` [EVIDENCE
  `clc67d.h:74-75`].
- *custom mapping*: `head = BIT_IDX_32(windowHeadMask)` [`nvkms-rm.c:994`].

⇒ **Minimal first-pixel mapping: head 0 + window 0** (since `0>>1 == 0`). Core `channelMask`
bit0 / instance 0; window `channelMask` bit1 / instance 0. The window is bound to its head
inside the core channel via `NVC67D_WINDOW_SET_CONTROL[win]` (per-window owner) — that
per-method programming is **S09/S10 scope**.

**[Apply CORRECTIONS #5 — channel identity.]** Trust the **source** allocation for channel
identity (core = mask bit0/instance 0; window N = `WINDOW(N)`/instance N), and the
stream-attributed `ch=pChannel->channelMask` from the `RVGEVO M` hook — **not** the older
`*-evo-decoded.txt` per-bucket `(core/window)` labels, which are unreliable (see TODO).

---

### F07.8 — Pushbuffer registration: `NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER`

This is the **CPU-RM → GSP/physical-RM** control that tells the display HW where a channel's
pushbuffer physically lives, so the EVO DMA pusher can fetch methods. It is the bridge between
"nvkms allocated a channel with `hObjectBuffer` (a pushbuffer ctxdma)" (F07.2) and the HW
actually pulling dwords.

Command id **`0x20800a58`** [EVIDENCE `ctrl/ctrl2080/ctrl2080internal.h:1361`]. Params
[EVIDENCE `ctrl2080internal.h:1365-1373`]:
```c
typedef struct NV2080_CTRL_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER_PARAMS {
    NvU32  addressSpace;                        // ADDR_SYSMEM(1) or ADDR_FBMEM(2)
    NV_DECLARE_ALIGNED(NvU64 physicalAddr, 8);  // PB base physical addr
    NV_DECLARE_ALIGNED(NvU64 limit, 8);         // PB size limit (≤ 4K)
    NvU32  cacheSnoop;
    NvU32  hclass;                              // channel class (e.g. 0xc67d/0xc67e)
    NvU32  channelInstance;
    NvBool valid;
} NV2080_CTRL_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER_PARAMS;
```
`addressSpace` values `ADDR_UNKNOWN=0 / ADDR_SYSMEM=1 / ADDR_FBMEM=2`
[EVIDENCE `ctrl/ctrl0080/ctrl0080fb.h:167-169`].

**Producer [EVIDENCE `disp_channel.c:727-788`, `kdispSetPushBufferParamsToPhysical_IMPL`].**
For a DMA channel (`pDispChannel->bIsDma`, `:755`) it resolves `hObjectBuffer` to its
ContextDma (`ctxdmaGetByHandle`, `:757`), then fills: `limit = pBufferContextDma->Limit`
(`:766`); `addressSpace = memdescGetAddressSpace(...)` (`:767`) which **must** be `ADDR_SYSMEM`
or `ADDR_FBMEM` else `DBG_BREAKPOINT()`/`NV_ERR_GENERIC` (`:768-772`); `physicalAddr =
memdescGetPhysAddr(...)` (`:774`); `cacheSnoop` (`:775`); `valid = NV_TRUE` (`:776`). For
non-DMA channels `valid = NV_FALSE` (`:780`). It then issues the control on
`pGpu->hInternalClient` / `pGpu->hInternalSubdevice` (`:783-785`).

**[EVIDENCE — D04.3] This control is issued immediately BEFORE every channel `GSP_RM_ALLOC`**,
on the internal subdevice handle `0xabcd2080` (`...decoded-full.txt:643→646` core,
`:709→712` window0, repeating through `:823→826`). Decoded captured payloads:

| channel | line | addressSpace | physicalAddr | limit | hclass | inst | valid |
|---|---|---|---|---|---|---|---|
| CORE `c67d` | `payload-trace.txt:154` | `0x1` SYSMEM | `0xc7e8e000` | `0xfff` (4 KB) | `0xc67d` | `0` | `1` |
| WINDOW `c67e` #0 | `payload-trace.txt:175` | `0x1` SYSMEM | `0xc7e8c000` | `0xfff` | `0xc67e` | `0` | `1` |
| CURSOR `c67a` #0 | `payload-trace.txt:191` | `0x0` UNKNOWN | `0x0` | `0x0` | `0xc67a` | `0` | `0` |

⇒ on this GA102 the EVO pushbuffers are **sysmem, 4 KB each** (`limit=0xfff`), spaced 0x2000
apart; PIO cursor channels register with `valid=0` / no pushbuffer (`payload-trace.txt:191-194`).
`hInternalClient`/`hInternalSubdevice` are **GSP-assigned, read back from fn=65** — do NOT
hardcode (CORRECTIONS #7: `hInternalClient=0xc2000005`, `hInternalSubdevice=0xabcd2080`).

[INFERENCE → implementer action] A from-scratch GSP client must, for each DMA channel:
allocate a (sysmem) pushbuffer, then emit `0x20800a58` with `addressSpace=ADDR_SYSMEM(1)`,
`physicalAddr`=PB phys, `limit`=PB size−1, `hclass`=channel class, `channelInstance`,
`valid=TRUE` — **before** the channel `_ALLOC`, or the pusher will not fetch methods.

---

### F07.9 — Minimal channel-bring-up order (first pixel, head 0 + window 0)

Channel-relevant slice of the observed alloc order (`...decoded-full.txt:385-712`), interleaving
each DMA channel's mandatory pre-alloc pushbuffer control. (Root/device/subdevice/
`NV01_MEMORY_VIRTUAL`/`NV04_DISPLAY_COMMON` precede this — see D04.4.)

```
…  GSP_RM_ALLOC  NV04_DISPLAY_COMMON   params NULL                       # hParent=<device>
1. GSP_RM_ALLOC  NVC670_DISPLAY        params NULL                       # hParent=<device> -> hObject=<disp>
   (CPU-side: create CORE pushbuffer ctxdma in sysmem)
2. GSP_RM_CONTROL NV2080_..._DISPLAY_CHANNEL_PUSHBUFFER (0x20800a58)      # addr=1,phys,limit=0xfff,hclass=0xc67d,inst=0,valid=1
3. GSP_RM_ALLOC  NVC67D_CORE_CHANNEL_DMA  params NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS
                                          { channelInstance=0, hObjectBuffer=<core PB ctxdma>, offset=0, flags=0 }  # hParent=<disp>
   (CPU-side: create WINDOW0 pushbuffer ctxdma in sysmem)
4. GSP_RM_CONTROL NV2080_..._DISPLAY_CHANNEL_PUSHBUFFER (0x20800a58)      # hclass=0xc67e, inst=0, valid=1
5. GSP_RM_ALLOC  NVC67E_WINDOW_CHANNEL_DMA params NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS
                                          { channelInstance=0, hObjectBuffer=<win0 PB ctxdma>, offset=0, flags=0 } # hParent=<disp>
```
Then map each channel's 4 KB control page (PUT@0x0 / GET@0x4, F07.2/F07.3), push the modeset
(CORE) + surface bind (WINDOW), kick off (write PUT, F07.5), and `UPDATE` each
(`RELEASE_ELV=TRUE`, F07.6). `NVC372_DISPLAY_SW`, extra windows/window-imm/cursors are omitted.

---

### Minimal-path notes (first pixel on GA102)

**Essential**
- `NV04_DISPLAY_COMMON` (0x73, NULL) — else device reports `NO_HARDWARE_AVAILABLE`
  (`nvkms-rm.c:1625`); needed for head-count/EDID (S07).
- `NVC670_DISPLAY` (0xc670, NULL) — parent of all channels.
- `NVC67D_CORE_CHANNEL_DMA` ×1 (instance 0, mask bit0) — head/raster/SOR + controlling `UPDATE`.
- `NVC67E_WINDOW_CHANNEL_DMA` ×1 — **window 0** (instance 0, mask bit1) — binds + scans out the
  framebuffer; head 0 owns windows 0/1 (`win>>1`).
- Per DMA channel: a sysmem pushbuffer ctxdma (`hObjectBuffer`), a mapped 4 KB control page
  (PUT@0x0 / GET@0x4), and the pushbuffer **physical** registration via `0x20800a58`.
- Push mechanics: header-dword encoding (F07.3), `nvDmaSetStartEvoMethod` + data dwords, then
  `nvDmaKickoffEvo` (write PUT). Commit each channel with `UPDATE` (0x200), `RELEASE_ELV=TRUE`.

**Skippable for first pixel**
- `NVC372_DISPLAY_SW` (IMP/`IS_MODE_POSSIBLE`): hardcode a known-good 2560×1440 timing instead.
- `NVC67B_WINDOW_IMM_CHANNEL_DMA` (per window) and `NVC67A_CURSOR_IMM_CHANNEL_PIO` (per head):
  immediate-flip + cursor; one non-immediate window `UPDATE` suffices for a static square.
- Windows 1–7, heads 1–3, SLI subdevice-mask logic, syncpts/semaphores, interlock flags
  (`interlockMask=0`).
- BAR1/vidmem shadow-copy + `DMA_FLUSH` path is taken only when the PB is in vidmem; a **sysmem**
  PB (`isBar1Mapping == FALSE`) writes directly and skips it (`nvkms-dma.c:65`) — and this
  card's PBs are sysmem (F07.8), so this is the natural choice.

---

### Evidence cited

Class headers (`SRC/src/common/sdk/nvidia/inc/class/`):
- `clc670.h:32` — `NVC670_DISPLAY=0xc670`; `clc670.h:36-40` — `NVC670_ALLOCATION_PARAMETERS{numHeads,numSors,numDsis}`.
- `clc67d.h:34` — `NVC67D_CORE_CHANNEL_DMA=0xc67d`.
- `clc67d.h:61-67` — core DMA opcode (`OPCODE 31:29`, `_METHOD 0x0`/`_JUMP 0x1`/`_NONINC 0x2`/`_SET_SUBDEVICE_MASK 0x3`, `METHOD_COUNT 27:18`, `METHOD_OFFSET 13:2`); `clc67d.h:70-71` — `JUMP_OFFSET 11:2`, `SET_SUBDEVICE_MASK_VALUE 11:0`.
- `clc67d.h:74-75` — `NVC37D_WINDOW_MAPPED_TO_HEAD(w)=(w)>>1`, `GET_VALID_WINDOWMASK_FOR_HEAD(h)`.
- `clc67d.h:78,80,82` — `NVC67D_PUT=0x0`, `GET=0x4`, `UPDATE=0x200`; `clc67d.h:83,86,88,91,94` — UPDATE fields (`SPECIAL_HANDLING 21:20`/`_MODE_SWITCH`, `INHIBIT_INTERRUPTS 24:24`, `RELEASE_ELV 0:0`, `FLIP_LOCK_PIN 8:4`).
- `clc67e.h:34` — `NVC67E_WINDOW_CHANNEL_DMA=0xc67e`; `clc67e.h:38-44` — window DMA opcode.
- `clc67e.h:51,53,55` — `NVC67E_PUT=0x0`, `GET=0x4`, `UPDATE=0x200`; `clc67e.h:56,93` — `RELEASE_ELV 0:0`, `INTERLOCK_WITH_WIN_IMM 12:12`.
- `clc67e.h:131-138` — `SET_STORAGE=0x228` has **only** `BLOCK_HEIGHT 3:0` (no `MEMORY_LAYOUT`) — confirms CORRECTIONS #9.
- `clc67e.h:139,140,147` — `SET_PARAMS=0x22C`, `FORMAT 7:0`, `FORMAT_X8R8G8B8=0xE6` — confirms CORRECTIONS #5; `clc67e.h:177-178` — `SET_PLANAR_STORAGE(b)=0x230+b*4`, `PITCH 12:0`; `clc67e.h:181` — `SET_CONTEXT_DMA_ISO(b)=0x240+b*4`; `clc67e.h:183` — `SET_OFFSET(b)=0x260+b*4`.
- `clc67b.h:34` — `NVC67B_WINDOW_IMM_CHANNEL_DMA=0xc67b`; `clc67b.h:38-44` — opcode; `clc67b.h:55` — `UPDATE=0x200`.
- `clc67a.h:32` — `NVC67A_CURSOR_IMM_CHANNEL_PIO=0xc67a`; `clc67a.h:34-43` — PIO struct (`Free`@0x8, `Update`@0x200, `SetCursorHotSpotPointOut`@0x208); `clc67a.h:45,47` — `FREE=0x8`, `UPDATE=0x200`.
- `clc372sw.h:30` — `NVC372_DISPLAY_SW=0xc372` (class id only, no params).
- `cl917d.h:756` — `NV917D_DMA_METHOD_OFFSET 11:2` (older width); `cl917d.h:763-764` — `OPCODE_SET_SUBDEVICE_MASK 0x3`, `SET_SUBDEVICE_MASK_VALUE 11:0`; `cl917d.h:767,769,771` — `NV917D_PUT=0x0`, `GET=0x4`, `UPDATE=0x80`.

Alloc-param + control structs (`SRC/src/common/sdk/nvidia/inc/`):
- `nvos.h:2417-2432` — `NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS`; `nvos.h:2428-2430` — `CONNECT_PB_AT_GRAB` (`1:1`, YES=0/NO=1).
- `nvos.h:2434-2440` — `NV50VAIO_CHANNELPIO_ALLOCATION_PARAMETERS`.
- `ctrl/ctrl2080/ctrl2080internal.h:1361` — `NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER=0x20800a58`; `ctrl2080internal.h:1365-1373` — params struct.
- `ctrl/ctrl0080/ctrl0080fb.h:167-169` — `ADDR_UNKNOWN=0/ADDR_SYSMEM=1/ADDR_FBMEM=2`.

DMA push layer (`SRC/src/nvidia-modeset/`) — **line numbers corrected vs S08**:
- `include/nvkms-dma.h:37` — `nvDmaKickoffEvo` proto.
- `nvkms-dma.h:71-83` — `nvDmaStorePioMethod` (atomic PUT store, `__ATOMIC_RELAXED`); `nvkms-dma.h:85-99` — `nvDmaLoadPioMethod` (`__ATOMIC_ACQUIRE`).
- `nvkms-dma.h:111-115` — externs `rvg_evo_trace/count/cur_off/cur_remain/dcount`; `nvkms-dma.h:121-126` — **`RVGEVOD` data-word hook** (in `nvDmaSetEvoMethodData`); `nvkms-dma.h:127-128` — buffer write; `nvkms-dma.h:131-137` — `nvDmaSetEvoMethodDataU64`.
- `nvkms-dma.h:240-247` — `NV_UDISP_DMA_*` header fields (`OPCODE 31:29`/`_METHOD 0x0`/`METHOD_COUNT 27:18`/`METHOD_OFFSET 13:2`); `nvkms-dma.h:243-246` — 13:2-vs-11:2 comment.
- `nvkms-dma.h:253-296` — `nvDmaSetStartEvoMethod` (`RVGEVO M` hook **:263-267**; `count+1` :270; `method>>2` :272; 4-align assert :274; `nvEvoMakeRoom` :285-287; header build :289-292; fifo decrement :294).
- `src/nvkms-dma.c:33-37` — `rvg_evo_*` defs (`rvg_evo_trace=NV_TRUE` :33); `nvkms-dma.c:39` — `NV_DMA_PUSHER_CHASE_PAD 5`.
- `nvkms-dma.c:44-54` — `nvDmaKickoffEvo` (`putOffset=buffer-base` :47); `nvkms-dma.c:56-126` — `EvoCoreKickoff` (isBar1Mapping :65; copy :83-96; `NV0080_CTRL_CMD_DMA_FLUSH` :102-108; `sfence` :115; `put_offset` :121; PUT store loop `nvDmaStorePioMethod(NV917D_PUT)` :122-125).
- `nvkms-dma.c:129-133` — `EvoCoreReadGet` (`NV917D_GET`).
- `nvkms-dma.c:186-255` — `nvEvoMakeRoom` (ring-wrap `0x20000000` writes :198,213; 5 s timeout :244-249).
- `nvkms-dma.c:356-374` — `nvEvoSetSubdeviceMask` (dword build :370-372).

Channel/object alloc + mapping (`SRC/src/nvidia-modeset/`):
- `src/nvkms-rm.c:50` — include `cl0073.h`; `nvkms-rm.c:893` — `numHeads` via `NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS`.
- `nvkms-rm.c:953,958,987-988,994,998` — window→head (`GET_VALID_HEAD_WINDOW_ASSIGNMENT`; flexible `head=win>>1`; custom `BIT_IDX_32`; `headForWindow[win]`).
- `nvkms-rm.c:1603-1611,1625` — `NV04_DISPLAY_COMMON,NULL` alloc under `deviceHandle`; `NO_HARDWARE_AVAILABLE`.
- `nvkms-rm.c:2594` — `RmAllocEvoChannel`; `:2604` — `dmaControlLen=0x1000`; `:2648,2678-2682` — `NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS` fill (`channelInstance`/`hObjectBuffer=dma.ctxHandle`/`offset=0`); `:2702-2706` — map 4 KB control page.
- `nvkms-rm.c:3159-3160` — window/imm class table (GA102 → `NVC67E`/`NVC67B`); `:3185` — window alloc (`WINDOW(win)`/instance win); `:3255-3258` — core alloc (`CORE_ENABLE`/instance 0/`coreChannelClass`).
- `src/nvkms-evo.c:4488-4496` — `NVC670_DISPLAY` alloc (`dispClass`, parent `deviceHandle`, `NULL`).
- `src/nvkms-evo3.c:2656,2685,2688` — `UpdateCoreC3` (`SET_INTERLOCK_FLAGS`/`SET_WINDOW_INTERLOCK_FLAGS`/UPDATE); `:2954,2965-2966,2992` — `EvoUpdateC3` (`coreInterlockMask=updateChannelMask & ~noCoreInterlockMask`); `:5887` — `numWindows`; `:6748` — `NVC372_DISPLAY_SW,NULL`; `:7880` — `nvEvoC6` HAL `Update`=`EvoUpdateC3` (struct `:7871`; same fn shared by `nvEvoC3` `:7722` / `nvEvoC5` `:7801`).
- `src/nvkms-hal.c:75-79` — ENTRY macro (`class=NV<p>70_DISPLAY`, `coreChannelClass=NV<p>7D_CORE_CHANNEL_DMA`); `:192,194` — Ada `C7,C6` / Ampere `C6,C6`; `:214-220` — `dispClass`/`coreChannelClass` selection.
- `src/nvkms-cursor.c:321,325,330,340,369` — `nvAllocCursorEvo` (loop head<numHeads, `channelInstance=head`, `cursorHal->klass`, parent `displayHandle`, `cursorPio[head]`); `src/nvkms-cursor3.c:108` — `nvEvoCursorC6.klass=NVC67A_CURSOR_IMM_CHANNEL_PIO`.
- `include/nvkms-types.h:298,302-303,307-308,322` — channel-mask bits (CORE 0, WINDOW 1+n/__SIZE 32, CURSOR 33+n/__SIZE 8, WINDOW_IMM 49); `nvkms-types.h:352` — `NV_EVO_CHANNEL_MASK_WINDOW_NUMBER` (inline fn).

RM-side producer (`SRC/src/nvidia/`):
- `src/kernel/gpu/disp/disp_channel.c:727-788` — `kdispSetPushBufferParamsToPhysical_IMPL` (`bIsDma` :755; `ctxdmaGetByHandle` :757; `limit` :766; `addressSpace` SYSMEM/FBMEM-or-`DBG_BREAKPOINT` :767-772; `physicalAddr` :774; `cacheSnoop` :775; `valid` TRUE :776 / FALSE :780; control on `hInternalClient`/`hInternalSubdevice` :783-785).

Trace artifacts (decoded/captured):
- `AN/20260530-110540-open-capture-decoded-full.txt:403,406,634,637,646,712-796,718-802,808-826` — object tree handles + parents (channels child of `NVC670 0x10011`); `:643→646,709→712,823→826` — `0x20800a58` issued before each channel alloc.
- `CAP/payload-trace.txt:154,175,191-194` — pushbuffer registration words (core/window SYSMEM 4 KB valid=1; cursor valid=0).
- `CAP/evo-trace.txt` — `RVGEVO M` per-method lines (hook `nvkms-dma.h:263-267`); `CAP2=…115116…/evo-data-trace.txt` — `RVGEVOD` data-word lines (hook `nvkms-dma.h:121-126`).
- D07/CORRECTIONS #5 — captured framebuffer bind: `SET_CONTEXT_DMA_ISO[0]=0x00010087`, `SET_OFFSET=0`, `SET_PARAMS=0xE6`, `SET_PLANAR_STORAGE[0]=0x100`.

---

### Open questions / TODO

- **[UNCERTAIN/TODO] Older `*-evo-decoded.txt` channel buckets are unreliable.** Its C67D/C67E
  per-bucket labels and `ch`→window-index decode disagree with the mask-bit layout
  (`nvkms-types.h:298-323`) — e.g. `ch=0x2`/`0x8` buckets contain `NVC67D_*` methods though
  there is only one core channel. Trust the source allocation (F07.7) and the raw
  `ch=pChannel->channelMask` from `RVGEVO M`. Follow-up: inspect the decoder script that
  produced `*-evo-decoded.txt` and reconcile against raw `CAP/evo-trace.txt`. (Applies
  CORRECTIONS #5; superseded for values by the channel-attributed `evo-full.txt` in the ADDENDUM.)
- **[TODO] `NVC670_ALLOCATION_PARAMETERS` defaults when NULL is passed.** nvkms passes `NULL`
  (`nvkms-evo.c:4496`); confirm GA102 `numHeads/numSors/numDsis` defaults from the RM-side
  `NVC670` constructor (search `SRC/src/nvidia/.../disp` for class `0xc670`). For bring-up, read
  counts back via `NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS`.
- **[TODO] `hObjectNotify` for DMA channels.** nvkms supplies a notifier ctxdma (handle-gap,
  F07.2) but the captured core/window registrations did not expose it; verify whether a minimal
  client can pass `hObjectNotify=0` (cursor PIO uses 0, `payload-trace.txt:191`).
- **[TODO] `flags`/`offset` for DMA channels.** nvkms leaves `flags=0` ⇒ `CONNECT_PB_AT_GRAB_YES`
  (`nvos.h:2428-2430`) and `offset=0` (`nvkms-rm.c:2682`); confirm these are correct for a
  from-scratch client (alloc-param **values** are [INFERENCE] — the `RVGTRACE alloc` hook logs
  only `hClass/hParent/hObject`, not `pAllocParams`; struct *layouts* are [EVIDENCE]).
- **[TODO] UPDATE latch timing.** `RELEASE_ELV` semantics and whether a core `UPDATE` alone
  (no interlock) reliably latches a window surface on this HW — verify against the S09/S10
  modeset ordering and the captured 60 Hz vs (EDID-derived) 144 Hz sequences.
- **[TODO] PIO cursor `UPDATE` path.** `clc67a.h:47` defines `UPDATE=0x200` in the mapped PIO
  control struct (not a pushbuffer method); if cursor is ever needed, confirm the direct-MMIO
  write semantics differ from the DMA channels' pushed `UPDATE`.


## F08 — Display Detection, EDID, Connectors/SOR, DisplayPort & Mode Validation

Merged + judge-verified from drafts **S09** (detect/EDID/SOR/IMP control flow), **D05**
(exhaustive display `GSP_RM_CONTROL` catalog with measured I/O), and **D08** (DisplayPort
detection + link-training deep-dive). Scope: the `GSP_RM_CONTROL` (RPC fn=76) control plane that
**finds the monitor, reads its EDID, routes it to an output resource (SOR), trains the DisplayPort
link, and validates the mode** for the GA102/NVDisplay path. The EVO modeset that consumes these
results (`HEAD_SET_*` / window `SET_*`) is **F09** — cross-referenced, not duplicated; the object
tree + channel/pushbuffer plumbing is **F07**; RPC wire mechanics + handle scheme is **F05**.

Paths: `SRC=/home/flare/dev/gpu-repro/open-gpu-kernel-modules`,
`AN=/home/flare/dev/gpu-repro/analysis`,
`CAP=/home/flare/dev/gpu-repro/traces/20260530-112551-open-capture` (control **inputs**:
`payload-trace.txt`, first 16 u32 of each `params` struct captured at `rpcRmApiControl_GSP`),
`CAP2=/home/flare/dev/gpu-repro/traces/20260530-115116-open-capture` (control **outputs**:
`rpc-resp-trace.txt` = `RVGRESP` reply dwords; `evo-data-trace.txt`; channel-attributed
`AN/...-115116-...-evo-full.txt`). Census counts are from
`AN/20260530-110540-open-capture-decoded-full.txt` (the **minimal** bring-up pass).

Honest-label legend: **[EVIDENCE]** = backed by a cited file:line / trace dword;
**[INFERENCE]** = reasoned from cited evidence (basis stated); **[UNCERTAIN/TODO]** = not
confirmable from the captures (what to check stated). Each control is tagged **ESSENTIAL**
(needed for first pixel on this GA102 + the DP panel) or **informational** (read but skippable for
one static DP mode).

> **Judge note (verification status).** Every struct, cmd id, bitfield, and enum below was
> re-read against the cited headers; every measured value was re-decoded from the trace dwords.
> The **measured values in D05/D08 hold** and the input timings in S09 hold. Substantive
> changes applied during reconciliation (detailed inline, flagged ⚠):
> 1. **`DP_GET_CAPS.maxLinkRate` enum (the headline fix).** S09.3(b)'s inline comment
>    "`1=5.40(HBR2) 2=2.70(HBR) 3=1.62(RBR)`" is the **stale doc comment** (`ctrl0073dp.h:1758-1760`)
>    and is **wrong**. The authoritative `#define` enum is **increasing** (`1=1.62 … 4=8.10`,
>    `ctrl0073dp.h:1810-1815`); the measured value **4 = HBR3 (8.10)**, confirmed by the DP library
>    decode (`dp_evoadapter.cpp:293-300`). D05/D08 already corrected this; S09's inline text is
>    superseded here.
> 2. **`DFP_ASSIGN_SOR` is *not* absent.** S09.4/D05.2 call it "0 calls / default VBIOS routing /
>    skippable" — true only for the **minimal** census. In the **full modeset** (CAP2) it **is**
>    issued (`rpc-resp-trace.txt:838,840`), driven by `nvkms-modeset.c:2035`→`nvAssignSOREvo`; so
>    `CROSS_BAR_SUPPORTED` **is** set on GA102 (resolves S09 TODO-4). Reclassified ESSENTIAL for a
>    real (especially multi-display) modeset.
> 3. **Head numbering.** D05.2's `GET_ACTIVE` reading ("head 2 → 0x800 … matches the modeset") is
>    a **pre-modeset readback** (ran ~9 s before LT/modeset) and is the **opposite** of the captured
>    modeset. The channel-attributed EVO stream is authoritative: **head 3 = DP `0x800`**, **head 2
>    = HDMI `0x2000`** — matching **CORRECTIONS #1**.
> 4. **DRM connector 88.** CONTEXT_BRIEF §3 / ADDENDUM-#1 "modetest set the mode on connector 88"
>    is **contradicted** by `modetest-set-*.txt` (both captures: *"failed to find mode … for
>    connector 88"*). Connector **88 = DP-1 = disconnected**; the connected DP is **93 = DP-2**.
> 5. **144 Hz is partly on-wire.** CORRECTIONS #6 ("144 Hz timing is EDID-derived, not on-wire")
>    is true only for the EVO modeset **data words**; the 144 Hz raster/pclk **were** sent to
>    `IS_MODE_POSSIBLE` (`payload-trace.txt:278`, reply `:366`). Refined accordingly.

---

### F08.0 — TL;DR: the two displays, the DP target, the trained link

**[EVIDENCE]** Exactly **two** displays are connected; the connect-state probe of the candidate
mask `0x0000ff00` returns `0x00002800` (`rpc-resp-trace.txt:284`):

| RM `displayId` | Signal | Output resource | DRM connector | Head (captured modeset) | Pixel clock (60 Hz) | Trained link |
|---|---|---|---|---|---|---|
| **`0x800`** | **DisplayPort** (AOC AG241QG4, 2560×1440/144) | **SOR 1, protocol DP_B** | **93 = DP-2** (connected) | **head 3** | **241.5 MHz** | **2 lanes @ HBR2** (60 Hz); **4 lanes @ HBR2** needed for 144 Hz |
| `0x2000` | HDMI/TMDS | SOR 0 (SINGLE_TMDS_A) | 96 = HDMI-A-2 (connected) | head 2 | 241.7 MHz | n/a (TMDS) |
| — | — | — | **88 = DP-1 = *disconnected*** | — | — | — |

Sources: SOR routing `rpc-resp-trace.txt:123` (`OR_GET_INFO` 0x800→index 1/type 2/proto 9) +
`:838` (`DFP_ASSIGN_SOR` sorAssignList[0]=0x2000, [1]=0x800); head/pclk/displayId
`evo-full.txt:4711,4681` (head 3→0x800@241.5 MHz) and `:4780,4750` (head 2→0x2000@241.7 MHz);
connectors `modetest-list.txt:15,71,149`; trained link `rpc-resp-trace.txt:344`
(`GET_LINK_CONFIG` laneCount=2, linkBW=0x14) + `:849,853` (`DP_CTRL` assess 4×HBR2 then optimize
2×HBR2, both `err=0`).

**First-pixel target = `displayId 0x800` (DP-2, SOR 1, DP_B).** The head index is the
implementer's choice (this capture used head 3 because the HDMI panel held head 2 / SOR 0); for a
single-display StelluxOS bring-up, **use head 0**.

**The sink, not the GPU, caps the link at HBR2.** GPU/SOR advertises HBR3 (`maxLinkRate=4`) and the
DFP port advertises HBR3/4-lane (`DFP_GET_INFO=0x0208001b`), but the panel's DPCD
`MAX_LINK_RATE=0x14` (HBR2) is the ceiling (`rpc-resp-trace.txt:288`).

---

### F08.1 — Object handles (where each control is sent)

**[EVIDENCE]** All `NV0073_*` (detect/EDID/SOR/DP) controls target the **`NV04_DISPLAY_COMMON`**
(class `0x73`) handle — `hObject=0x0001000d` for the nvkms client (`rpc-resp-trace.txt:142`, etc.),
`hObject=caf00003` under the secondary nvidia-drm client (client `c1d00008`, `:118`). It is a child of **DEVICE** (sibling of
the subdevice), per **CORRECTIONS #4 / F07**. `NVC372_*` (`IS_MODE_POSSIBLE`) targets the
**`NVC372_DISPLAY_SW`** (class `0xc372`) handle `hObject=0x00010010` (`rpc-resp-trace.txt:365`).
`NV2080_*` display-internal controls target the **subdevice** (boot context
`hClient=0xc2000005`/`hObject=0xabcd2080`, `rpc-resp-trace.txt:35`).

---

### F08.2 — NV2080 display-engine inventory (ESSENTIAL prerequisite)

These run against the subdevice and tell the driver what display HW exists before any NV0073 probe.
Outputs are **[EVIDENCE]** from `CAP2/rpc-resp-trace.txt`.

| cmd id | name | first-pixel | measured output |
|---|---|---|---|
| `0x20800a01` | `INTERNAL_DISPLAY_GET_STATIC_INFO` | **ESSENTIAL** | `feHwSysCap=0xf0f`, **`windowPresentMask=0xff`** (8 windows), **`numHeads=4`**, `i2cPort=0x10` (`:35`) |
| `0x20800a4b` | `INTERNAL_DISPLAY_GET_IP_VERSION` | **ESSENTIAL** | `ipVersion=0x04010000` → [INFERENCE] NVDisplay IP v04.01 (selects the EVO **C67x** class path) (`:11`) |
| `0x20800a49` | `INTERNAL_DISPLAY_WRITE_INST_MEM` | **ESSENTIAL** | `instMemPhysAddr=0x2_728b0000`, `instMemSize=0x10000` (64 KB), `addrSpace=2` (`:38`) |
| `0x20800a58` | `INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER` ×21 | **ESSENTIAL** | per-channel bind; core: `addressSpace=1, physAddr=0xce5f6000, limit=0xfff, hclass=0xc67d, inst=0, valid=1` (`:219`; input `payload-trace.txt:154`) |

Struct refs: `ctrl2080internal.h` 71-79 (`GET_STATIC_INFO`), 886-891 (`WRITE_INST_MEM`),
916-918 (`GET_IP_VERSION`), 1365-1373 (`CHANNEL_PUSHBUFFER`). Channel registration detail = **F07**.

---

### F08.3 — Connector / display detection (NV0073 system · specific · dfp)

Enumeration order as captured (`AN/...-decoded-full.txt:409-619`). Measured outputs cited to
`CAP2/rpc-resp-trace.txt`.

| Control (cmd id) | struct | first-pixel | measured I/O |
|---|---|---|---|
| `SYSTEM_GET_NUM_HEADS` (`0x730102`) | `ctrl0073system.h:135-139` | **ESSENTIAL** | `numHeads=4` (`:142`) |
| `SYSTEM_GET_SUPPORTED` (`0x730120`) | `:310-314` | **ESSENTIAL** | `displayMask=displayMaskDDC=0x0000ff00` (`:118`) |
| `SYSTEM_GET_CONNECT_STATE` (`0x730122`) | `:392-397` | **ESSENTIAL** | in `displayMask=0xff00,flags=0` (`payload:196`); out **`0x2800`** = `0x800`+`0x2000` connected (`:284`) |
| `SPECIFIC_OR_GET_INFO` (`0x73028b`) | `ctrl0073specific.h:1082-1096` | **ESSENTIAL** | 0x800 → index **1**, type **2**(SOR), proto **9**(DP_B) (`:123`) — see F08.5 |
| `DFP_GET_INFO` (`0x731140`) | `ctrl0073dfp.h:122-127` | **ESSENTIAL** | 0x800 → `flags=0x0208001b` (DP/4-lane/HBR3-port) (`:175`) — see F08.6 |
| `SPECIFIC_GET_TYPE` (`0x730240`) | `ctrl0073specific.h:70-74` | informational | 0x800 → `displayType=2` (DFP) (`:177`) |
| `SPECIFIC_GET_CONNECTOR_DATA` (`0x730250`) | `:404-416` | informational | 0x800 → PRESENT, `DDCPartners=0x1000`, `data[0]={index=2,type=0x46(DP_EXT),loc=2}` (`:171`) |
| `SPECIFIC_GET_PCLK_LIMIT` (`0x73028a`) | `:987-993` | informational (IMP is authority) | 0x800 → `0x28bdb0`=**2,670,000 KHz**; TMDS → `0x28488`=165,000 (`:208,206`) |
| `SYSTEM_GET_CAPS_V2`/`GET_ALL_HEAD_MASK`/`GET_VALID_HEAD_WINDOW_ASSIGNMENT`/`GET_BOOT_DISPLAYS`/`GET_INTERNAL_DISPLAYS`/`QUERY_DISPLAY_IDS_WITH_MUX` | — | informational | static caps / masks; not load-bearing for one DP mode |

The `GET_NUM_HEADS`/`GET_SUPPORTED`/`GET_CONNECT_STATE` structs (verbatim):

```135:139:SRC/src/common/sdk/nvidia/inc/ctrl/ctrl0073/ctrl0073system.h
typedef struct NV0073_CTRL_SYSTEM_GET_NUM_HEADS_PARAMS {
    NvU32 subDeviceInstance;
    NvU32 flags;
    NvU32 numHeads;
} NV0073_CTRL_SYSTEM_GET_NUM_HEADS_PARAMS;
```
```392:397:SRC/src/common/sdk/nvidia/inc/ctrl/ctrl0073/ctrl0073system.h
typedef struct NV0073_CTRL_SYSTEM_GET_CONNECT_STATE_PARAMS {
    NvU32 subDeviceInstance;
    NvU32 flags;
    NvU32 displayMask;      // in: probe mask; out: connected subset
    NvU32 retryTimeMs;
} NV0073_CTRL_SYSTEM_GET_CONNECT_STATE_PARAMS;
```

**[EVIDENCE]** `DFP_GET_INFO` is how the driver learns the link is DisplayPort — `flags._SIGNAL`
(2:0) `=3=DISPLAYPORT` (`ctrl0073dfp.h:130-134`). For `displayId 0x800`, `flags=0x0208001b` decodes
(field defs `ctrl0073dfp.h:130-196`): `_SIGNAL=3` (DP), `_LANE=3` (`_QUAD`, 4-lane,
`:141`), `_DP_LINK_BW=4` (`_8_10GBPS`=HBR3, `:177`), `_HDMI_CAPABLE=0`,
`_DP_POST_CURSOR2_DISABLED=1` (`:190`). The five TMDS ports read `0x00105300` (`_SIGNAL=TMDS`,
`_HDMI_CAPABLE=1`). DFP_GET_INFO reports **port capability**, not the trained value.

---

### F08.4 — EDID read (ESSENTIAL — source of every timing number)

Control `SPECIFIC_GET_EDID_V2` (`0x730245`), struct `ctrl0073specific.h:151-157`; `flags._COPY_CACHE`
(0:0, `_NO`=read fresh via DDC) / `_READ_MODE` (1:1, `_COOKED/_RAW`) (`:159-165`).

**[EVIDENCE]** nvkms reads fresh over DDC (`_COPY_CACHE_NO`):
```1126:1138:SRC/src/nvidia-modeset/src/nvkms-dpy.c
    getEdidParams->subDeviceInstance = pDispEvo->displayOwner;
    getEdidParams->displayId = nvDpyEvoGetConnectorId(pDpyEvo);
    getEdidParams->flags = NV0073_CTRL_SPECIFIC_GET_EDID_FLAGS_COPY_CACHE_NO;
    ...
    ret = nvRmApiControl(nvEvoGlobal.clientHandle,
                         pDevEvo->displayCommonHandle,
                         NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2,
                         getEdidParams, sizeof(*getEdidParams));
```

**[EVIDENCE]** Input `payload-trace.txt:251` `cmd=0x00730245 … w=… 00000800 00000000 …`
(`displayId=0x800`, `flags=0` = COOKED/CACHE_NO). Output `rpc-resp-trace.txt:339`:
`bufferSize=0x180` (**384 bytes** = base + 2 extension blocks), then `edidBuffer` =
`ffffff00 00ffffff 2410e305 …` → EDID header `00 FF FF FF FF FF FF 00`, manufacturer `05 E3` =
**"AOC"**, product `0x2410`. (D05 upgraded S09's inferred-header to the full captured EDID.)

**[EVIDENCE] The mode list it yields** — `modetest-list.txt` connector 93 (DP-2):
```74:77:CAP/modetest-list.txt
  #0 2560x1440 59.95 2560 2608 2640 2720 1440 1443 1448 1481 241500 flags: phsync, pvsync; type: preferred
  #1 2560x1440 143.91 2560 2568 2600 2666 1440 1465 1473 1543 592000 flags: phsync, nvsync; type:
  #2 2560x1440 120.00 2560 2568 2600 2666 1440 1465 1473 1525 487870 flags: phsync, pvsync; type:
  #3 2560x1440 100.00 2560 2568 2600 2666 1440 1465 1473 1510 402560 flags: phsync, pvsync; type:
```
- **60 Hz** (preferred): `pclk=241500 KHz`, `hTotal=2720`, `vTotal=1481` → `241.5e6/(2720×1481)=59.95 Hz`. ✓
- **144 Hz**: `pclk=592000 KHz`, `hTotal=2666`, `vTotal=1543` → `592e6/(2666×1543)=143.91 Hz`. ✓

These exact numbers reappear, byte-for-byte, in IMP (F08.7) and the EVO `HEAD_SET_RASTER_*`
methods (F09). `SPECIFIC_SET_EDID_V2` (`0x730246`, `rpc-resp-trace.txt:338`) — nvkms caching the
EDID back into RM — is **informational** (not required for first pixel).

---

### F08.5 — Connectors, SOR routing & the two-monitor / head mapping (CORRECTIONS #1)

**[EVIDENCE] SOR routing** via `SPECIFIC_OR_GET_INFO` (`0x73028b`), struct
`ctrl0073specific.h:1082-1096`; enums: type `_SOR=2` (`:1101`), SOR protocol `_DP_B=9` (`:1118`),
`_SINGLE_TMDS_A=1` (`:1114`), dither `_OFF=3` (`:1134`), location `_CHIP=0` (`:1145`). Reply for
`0x800` (`rpc-resp-trace.txt:123`, params `…0800 0001 0002 0009 0003 ffffffff 0000 0000 0003`):
**index = 1 → SOR 1**, type 2 (SOR), protocol 9 (**DP_B**), ditherType 3 (OFF), location 0 (CHIP),
dcbIndex 3.

⚠ **[EVIDENCE — correction to S09.4/D05.2] `DFP_ASSIGN_SOR` (`0x731152`) IS issued in the full
modeset.** Struct `ctrl0073dfp.h:590-601`; SOR-type enum `_SINGLE=1` (`:517`). Reply
`rpc-resp-trace.txt:838` (displayId `0x800`) returns `sorAssignList[0]=0x2000, [1]=0x800` →
**SOR 0 ⇒ HDMI `0x2000`, SOR 1 ⇒ DP `0x800`** (and `:840` for `0x2000`). It is gated on
`CROSS_BAR_SUPPORTED` and driven from the modeset path:
```4746:4760:SRC/src/nvidia-modeset/src/nvkms-evo.c
    if (!NV0073_CTRL_SYSTEM_GET_CAP(pDevEvo->commonCapsBits,
                NV0073_CTRL_SYSTEM_CAPS_CROSS_BAR_SUPPORTED)) {
        return TRUE;
    }
    params.subDeviceInstance = pDispEvo->displayOwner;
    params.displayId = displayId;
    params.bIs2Head1Or = b2Heads1Or;
    params.sorExcludeMask = sorExcludeMask;
    ret = nvRmApiControl(nvEvoGlobal.clientHandle,
                         pDevEvo->displayCommonHandle,
                         NV0073_CTRL_CMD_DFP_ASSIGN_SOR,
                         &params, sizeof(params));
```
(call site `nvkms-modeset.c:2035`). Because the call fired, **`CROSS_BAR_SUPPORTED` is set on
GA102** (resolves S09 TODO-4). **first-pixel: ESSENTIAL for a real modeset** — it confirms/sets the
display→SOR routing `SOR_SET_CONTROL` (F09) needs; the SOR index from `OR_GET_INFO` is consistent
with it.

⚠ **[EVIDENCE — head mapping, supersedes D05.2] The captured modeset puts the DP panel on head 3.**
The channel-attributed EVO core-channel stream (the authoritative source per ADDENDUM/CORRECTIONS):
- `evo-full.txt:4711` `NVC67D_HEAD_SET_DISPLAY_ID[3] = 0x00000800` and `:4681`
  `HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX[3] = 0x0e64ff60 (241,500,000)` → **head 3 = DP `0x800` @ 241.5 MHz**.
- `evo-full.txt:4780` `HEAD_SET_DISPLAY_ID[2] = 0x00002000` and `:4750`
  `…FREQUENCY_MAX[2] = 0x0e680ca0 (241,700,000)` → **head 2 = HDMI `0x2000` @ 241.7 MHz**.

This matches **CORRECTIONS #1** and the HDMI panel's own EDID (60 Hz `pclk=241700`,
`modetest-list.txt:152`) vs the DP panel's `241500` (`:74`).

⚠ **[EVIDENCE — caveat] `SYSTEM_GET_ACTIVE` (`0x730126`) is a *pre-modeset* readback.** Struct
`ctrl0073system.h:646-651` (`{subDeviceInstance, head, flags, displayId}`). At
`rpc-resp-trace.txt:223,225` (t≈4606.03 s, **~9 s before** LT/modeset at t≈4615 s) it reports
**head 2 → 0x800, head 3 → 0x2000** — i.e. the *prior* desktop modeset, the **opposite** of the
modeset captured here. D05.2's "(matches ADDENDUM modeset)" parenthetical is therefore incorrect;
trust the EVO stream above. (The head reassignment between the two states is exactly why
`DFP_ASSIGN_SOR` runs.)

⚠ **[EVIDENCE — connector namespaces, supersedes BRIEF §3 / ADDENDUM-#1].** Three distinct
namespaces, reconciled:
- **DRM connector id** (`modetest-list.txt`): **88 = DP-1 = *disconnected*** (`:15`, 0 modes);
  **93 = DP-2 = connected** (`:71`, 32 modes = the AOC panel); 96 = HDMI-A-2 = connected (`:149`).
- **RM `displayId`**: `0x800` (DP, connected) / `0x2000` (HDMI). The DP panel = **93 (DP-2) =
  `0x800`** ([INFERENCE], basis: it is the sole connected DP and the only one carrying the
  2560×1440 @59.95/143.91 pair).
- **Head** (this capture): DP on head 3.

The BRIEF §3 "connector id 88" and ADDENDUM-#1 "modetest selects connector 88 and successfully set
the mode" do **not** hold: `modetest-set-pref.txt`/`modetest-set-144.txt` in **both** captures read
*"failed to find mode … for connector 88"* (88 is the disconnected DP-1). The captured EVO modeset
is the running desktop session, not a successful `modetest`-on-88. **Do not treat 88 as the DP
target; the target is `displayId 0x800` = DP-2 (93).**

---

### F08.6 — DisplayPort path: DPCD/AUX, GPU caps, link config, training

DP bring-up splits into an **AUX/DPCD transport** (`DP_AUXCH_CTRL`), a **PHY-training trigger**
(`DP_CTRL`, executed *inside GSP-RM*), and **policy** (rate/lane selection + fallback) owned by the
nvkms DisplayPort library.

| Step | Who | RM control | first-pixel |
|---|---|---|---|
| Take manual DP control | DP library | `DP_SET_MANUAL_DISPLAYPORT` (`0x731365`) | **ESSENTIAL** (DP) |
| Read GPU/SOR DP caps | DP library | `DP_GET_CAPS` (`0x731369`) | informational (bounds the link) |
| Read sink DPCD / EDID-over-I2C | DP library | `DP_AUXCH_CTRL` (`0x731341`) | **ESSENTIAL** (transport) |
| **Run CR/EQ PHY training** | **GSP-RM** | `DP_CTRL` (`0x731343`) | **ESSENTIAL** (trains the link) |
| Read back trained config | DP library | `DP_GET_LINK_CONFIG` (`0x731360`) | informational (read-back) |

**(a) `DP_SET_MANUAL_DISPLAYPORT`** — struct `ctrl0073dp.h:1671-1673` (just `subDeviceInstance`).
Header (`:1649-1651`): "Disables automatic watermark programming / automatic DP IRQ handling (CP
IRQ) / automatic retry on defers" — i.e. the nvkms DP library, not RM, now owns training. Issued by
`dp_evoadapter.cpp:126-128`; input `payload-trace.txt:95`, reply `rpc-resp-trace.txt:156`
(`subDev=0`, `status=0`). **[INFERENCE]** a client that never calls this leaves RM auto-training the
link (basis: header semantics) — a legitimate shorter path, but only the manual path was captured
(TODO-F08-3).

**(b) ⚠ `DP_GET_CAPS` — the corrected `maxLinkRate`.** Struct:
```1785:1800:SRC/src/common/sdk/nvidia/inc/ctrl/ctrl0073/ctrl0073dp.h
typedef struct NV0073_CTRL_CMD_DP_GET_CAPS_PARAMS {
    NvU32                          subDeviceInstance;
    NvU32                          sorIndex;
    NvU32                          maxLinkRate;
    NvU32                          dpVersionsSupported;
    NvU32                          UHBRSupported;
    NvBool                         bIsMultistreamSupported;
    NvBool                         bIsSCEnabled;
    NvBool                         bHasIncreasedWatermarkLimits;
    NvBool                         bIsPC2Disabled;
    NvBool                         isSingleHeadMSTSupported;
    NvBool                         bFECSupported;
    NvBool                         bIsTrainPhyRepeater;
    NvBool                         bOverrideLinkBw;
    NV0073_CTRL_CMD_DSC_CAP_PARAMS DSC;
} NV0073_CTRL_CMD_DP_GET_CAPS_PARAMS;
```
**[EVIDENCE]** Reply `rpc-resp-trace.txt:158`: `sorIndex=0xffffffff`, **`maxLinkRate=4`**,
`dpVersionsSupported=3`, `UHBRSupported=0`, `bIsMultistreamSupported=1`,
`bHasIncreasedWatermarkLimits=1`, `bFECSupported=1`, `bIsTrainPhyRepeater=1`, `bOverrideLinkBw=1`.

The `maxLinkRate` **field is an increasing enum**, contradicting the stale prose comment at
`ctrl0073dp.h:1758-1760` ("1 signifies 5.40"):
```1810:1815:SRC/src/common/sdk/nvidia/inc/ctrl/ctrl0073/ctrl0073dp.h
#define NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE                           2:0
#define NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_NONE                          (0x00000000U)
#define NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_1_62                          (0x00000001U)
#define NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_2_70                          (0x00000002U)
#define NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_5_40                          (0x00000003U)
#define NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_8_10                          (0x00000004U)
```
and the DP library decodes the value, proving the increasing order (not the comment):
```293:300:SRC/src/common/displayport/src/dp_evoadapter.cpp
        if (FLD_TEST_DRF(0073, _CTRL_CMD_DP_GET_CAPS, _MAX_LINK_RATE, _1_62, params.maxLinkRate))
            _maxLinkRateSupportedGpu = RBR; //in Hz
        else if (FLD_TEST_DRF(0073, _CTRL_CMD_DP_GET_CAPS, _MAX_LINK_RATE, _2_70, params.maxLinkRate))
            _maxLinkRateSupportedGpu = HBR; //in Hz
        else if (FLD_TEST_DRF(0073, _CTRL_CMD_DP_GET_CAPS, _MAX_LINK_RATE, _5_40, params.maxLinkRate))
            _maxLinkRateSupportedGpu = HBR2; //in Hz
        else if (FLD_TEST_DRF(0073, _CTRL_CMD_DP_GET_CAPS, _MAX_LINK_RATE, _8_10, params.maxLinkRate))
            _maxLinkRateSupportedGpu = HBR3; //in Hz
```
**⇒ measured `maxLinkRate=4` = `_8_10` = HBR3 (8.10 Gb/s).** GA102's GPU/SOR supports **HBR3 +
DP1.2/1.4 + MST + FEC**. `dpVersionsSupported` bits: `_DP1_2`(0:0), `_DP1_4`(1:1) (`:1802-1807`).
`sorIndex=0xffffffff` is an unset/"any" sentinel (the call site sets it from `getSorIndex()` before
SOR assignment; `dp_evoadapter.cpp:262-306`) — resolves S09 TODO-5. **informational** (it only
*bounds* the link; the sink DPCD is the real cap).

**(c) `DP_AUXCH_CTRL` — the DPCD/I2C transport.** Struct `ctrl0073dp.h:156-166`; `cmd._TYPE`(3:3)
`_I2C=0/_AUX=1`, `_REQ_TYPE`(1:0) `_WRITE=0/_READ=1`, `addr` 21-bit; `replyType`(3:0)
`_ACK=0…_I2CDEFER=8` (`:168-191`). Census: 43 calls (minimal) / 48 (full modeset). Issued from
`dp_evoadapter.cpp:762`. **[EVIDENCE] Sink receiver caps** — AUX READ DPCD `0x0` (`cmd=0x9`),
input `payload-trace.txt:200`, reply `rpc-resp-trace.txt:288` `data=12 14 c4 01 … 01 …`,
`size=0x10`, `replyType=0` (ACK):
- DPCD `0x00` `DPCD_REV=0x12` → **DP 1.2** sink.
- DPCD `0x01` `MAX_LINK_RATE=0x14` → **HBR2 (5.40)** — the panel ceiling (DPCD units ×0.27 GHz).
- DPCD `0x02` `MAX_LANE_COUNT=0xc4` → **4 lanes**, `TPS3_SUPPORTED`, `ENHANCED_FRAME_CAP`.
- DPCD `0x06` `CHANNEL_CODING=0x01` (8b/10b).

**[EVIDENCE] Post-training link status** — AUX READ DPCD `0x200C` (ESI region),
`rpc-resp-trace.txt:294` `data=77 00 01`: `LANE0_1_STATUS=0x77` (lane 0 & 1 each
`CR_DONE|EQ_DONE|SYMBOL_LOCKED`), `LANE2_3_STATUS=0x00` (lanes 2,3 idle), `ALIGN_STATUS=0x01`
(interlane aligned) → independent confirmation of a **2-lane trained, aligned** link.
The SOURCE_OUI write to DPCD `0x300` NACKs (harmless), HDCP probes (`0x68xxx`), DSC caps
(`0x60/0xb0/0x90` → sink DSC=`0`), LTTPR (`0xF0000` → none), and the MST-disable writes
(`0x111`) are all **informational** for an unprotected SST pixel.

**(d) `DP_CTRL` — the training trigger (GSP-RM does the handshake).** Struct `ctrl0073dp.h:508-516`;
`data._SET_LANE_COUNT`(4:0)∈{0,1,2,4,8}, `_SET_LINK_BW`(15:8) `0x06=1.62 0x0a=2.70 0x14=5.40
0x1e=8.10` (`:573-587`); `cmd` flags `_SET_LANE_COUNT`(0)/`_SET_LINK_BW`(1)/`_SET_ENHANCED_FRAMING`(7)/
`_FAKE_LINK_TRAINING`(12:11)/`_TRAIN_PHY_REPEATER`(13) (`:518-561`); `err` reports
`_CLOCK_RECOVERY`(4)/`_CHANNEL_EQUALIZATION`(5)/`_CR_DONE_LANE`(11:8)/`_LINK_TRAINING`(31)
(`:602-641`). **[EVIDENCE]** measured negotiation on `0x800`, all `err=0`:

| line | `cmd` | `data` | decode |
|---|---|---|---|
| `:835` | `0x2083` | `0x0600` | lanes=0, BW=1.62, ENH_FRAMING+TRAIN_PHY_REPEATER → link reset/power-down |
| `:849` | `0x2083` | **`0x1404`** | **lanes=4, BW=HBR2** → assess link at sink max; `err=0` ✓ |
| `:851` | `0x2883` | `0x0600` | `_FAKE_LINK_TRAINING=_DONOT_TOGGLE` → power-manage between attempts |
| `:853` | `0x2083` | **`0x1402`** | **lanes=2, BW=HBR2** → final optimized config for 60 Hz; `err=0` ✓ |

The DP library owns the **policy** (`getMaxLinkConfig` = min(GPU HBR3, sink HBR2, 4 lanes) =
`dp_connectorimpl.cpp:964-986`; `assessLink` `:3501`; `trainLinkOptimized` `:4118`); GSP-RM owns the
**mechanism** — `EvoMainLink::train` builds `dpCtrlData` and calls `DP_CTRL`
(`dp_evoadapter.cpp:1084-1096`), and **RM performs the per-step CR/EQ DPCD writes
(TRAINING_PATTERN_SET @0x102, drive/preemph) — these are NOT in the client AUX log.** *Key
StelluxOS consequence: you do not implement the DPCD training state machine; supply target
lanes+BW to `DP_CTRL` and check `err`.*

**(e) `DP_GET_LINK_CONFIG`** — struct `ctrl0073dp.h:1403-1408`; only `_LINK_BW_1_62`(0x6)/`_2_70`(0xa)
are `#define`d (`:1416-1417`), so `0x14` is read as the DPCD `LINK_BW_SET` unit (×0.27 GHz → 5.40 =
HBR2), corroborated by `DP_CTRL`'s `_SET_LINK_BW_5_40GBPS=0x14` (`:586`). **[EVIDENCE]** reply
`rpc-resp-trace.txt:344` `laneCount=2, linkBW=0x14` ⇒ **active 60 Hz link = 2 lanes @ HBR2**
(resolves S09 TODO-3). **informational** (read-back).

**Bandwidth math (required `pixelClock×bpp`; usable `lanes×rate×8/10`; 8 bpc=24 bpp, sink DSC=0):**

| Mode | required | min sink-legal config (≤HBR2) | usable | margin |
|---|---|---|---|---|
| 2560×1440 @ 60 (241.5 MHz) | 5.796 Gb/s | **2 × HBR2** | 8.64 Gb/s | +49% |
| 2560×1440 @ 144 (592.0 MHz) | 14.208 Gb/s | **4 × HBR2** | 17.28 Gb/s | +22% |

- **60 Hz** → the driver picked **2×HBR2** (`:853`, `:344`) — the measured trained config. **[EVIDENCE]**
- **144 Hz** → only **4×HBR2** fits under the sink's HBR2 ceiling. The **4×HBR2 assessment trained
  with `err=0`** (`:849`), so **144 Hz is reachable on this exact link.** **[EVIDENCE]** That the
  *active* 144 Hz modeset uses 4×HBR2 is **[INFERENCE]** (bandwidth math + assessment; the 144 Hz
  active modeset was not separately captured — TODO-F08-1). 10 bpc @144 needs 17.76 > 17.28 Gb/s
  and the sink lacks HBR3/DSC ⇒ **144 Hz is 8 bpc only** [INFERENCE].

---

### F08.7 — Mode validation: `NVC372_CTRL_CMD_IS_MODE_POSSIBLE` (IMP) — ESSENTIAL

**[EVIDENCE]** cmd `0xc3720101` (`ctrlc372chnc.h:39`), target `hObject=0x00010010`, `paramsSize=1924`,
issued **×121** (census). It is the GPU "is this config within mempool/dispclk/bandwidth limits"
gate; a config that fails IMP is never programmed. Top-level struct (answer = `bIsPossible`, plus
`dispClkKHz`):
```472:512:SRC/src/common/sdk/nvidia/inc/ctrl/ctrlc372/ctrlc372chnc.h
typedef struct NVC372_CTRL_IS_MODE_POSSIBLE_PARAMS {
    NVC372_CTRL_CMD_BASE_PARAMS base;
    NvU8                        numHeads;
    NvU8                        numWindows;
    NVC372_CTRL_IMP_HEAD        head[NVC372_CTRL_MAX_POSSIBLE_HEADS];      // MAX = 8
    NVC372_CTRL_IMP_WINDOW      window[NVC372_CTRL_MAX_POSSIBLE_WINDOWS];  // MAX = 32
    NvU32                       options;
    NvU32                       testMclkFreqKHz;
    NvBool                      bIsPossible;            // <- the answer
    NvBool                      bIsOSLDPossible[NVC372_CTRL_MAX_POSSIBLE_HEADS];
    NvU32                       minImpVPState;
    NvU32                       minPState;
    NvU32                       minRequiredBandwidthKBPS;
    NvU32                       floorBandwidthKBPS;
    NvU32                       minRequiredHubclkKHz;
    NvU32                       vblankIncreaseInLinesForOSLDMode[NVC372_CTRL_MAX_POSSIBLE_HEADS];
    NvU32                       wakeUpRgLineForOSLDMode[NVC372_CTRL_MAX_POSSIBLE_HEADS];
    NvU32                       worstCaseMargin;
    NvU32                       dispClkKHz;             // <- dispclk IMP picked for the mode
    char                        worstCaseDomain[8];
    NvBool                      bUseCachedPerfState;
} NVC372_CTRL_IS_MODE_POSSIBLE_PARAMS;
```
Per-head input fields an implementer must fill (`ctrlc372chnc.h:393-448`): `headIndex`,
`maxPixelClkKHz`, `rasterSize{width,height}`, `rasterBlankStart{X,Y}`, `rasterBlankEnd{X,Y}`,
`rasterVertBlank2{yStart,yEnd}`, `control{master/slaveLockMode,master/slaveLockPin}`, `lut`,
`cursorSize32p`, …; per-window `ctrlc372chnc.h:451-465` (`windowIndex`, `owningHead`,
`formatUsageBound`=bitmask of `NVC372_CTRL_FORMAT_*`, e.g. `RGB_PACKED_4_BPP=0x4` `:518`).
`options`: `_GET_MARGIN`(0x1)/`_NEED_MIN_VPSTATE`(0x2) (`:467-468`). Fail reasons:
`_NOT_ENOUGH_MEMPOOL=1, _VBLANK_TOO_SMALL=3, _INSUFFICIENT_BANDWIDTH=5, _DISPCLK_TOO_LOW=6`
(`:535-547`).

**[EVIDENCE] nvkms fills these from the chosen timing** (Ampere EVO3 HAL), then issues the control:
```3051:3065:SRC/src/nvidia-modeset/src/nvkms-evo3.c
    pImpHead->maxPixelClkKHz = pTimings->pixelClock;
    pImpHead->rasterSize.width           = pTimings->rasterSize.x;
    pImpHead->rasterSize.height          = pTimings->rasterSize.y;
    pImpHead->rasterBlankStart.X         = pTimings->rasterBlankStart.x;
    pImpHead->rasterBlankStart.Y         = pTimings->rasterBlankStart.y;
    pImpHead->rasterBlankEnd.X           = pTimings->rasterBlankEnd.x;
    ...
    pImpHead->control.masterLockMode = NV_DISP_LOCK_MODE_NO_LOCK;
    pImpHead->control.masterLockPin = NV_DISP_LOCK_PIN_UNSPECIFIED;
```
(IMP control issued at `nvkms-evo3.c:3276`, `NVC372_CTRL_CMD_IS_MODE_POSSIBLE`). The very same
`pTimings` is later emitted as `HEAD_SET_RASTER_SIZE` (F09).

**[EVIDENCE] IMP and the modeset consume byte-identical EDID timing.** Captured `head[0]` input
(`payload-trace.txt:277`, struct offsets per the layout above) decodes exactly to the **60 Hz**
EDID row:
- `maxPixelClkKHz = 0x0003af5c = 241500`; `rasterSize = 0x0aa0 × 0x05c9 = 2720 × 1481`;
  `rasterBlankStart = (0xa6f,0x5c5) = (2671,1477)`; `rasterBlankEnd = (0x6f,0x25) = (111,37)`;
  `control` lockModes `0` (`_NO_LOCK`), lockPins `0x10` (`_UNSPECIFIED`).
- These equal the EVO 60 Hz dwords (`RASTER_SIZE=0x05c90aa0`, `RASTER_BLANK_START=0x05c50a6f`,
  `RASTER_BLANK_END=0x0025006f`; F09 / ADDENDUM) and `modetest-list.txt:74`. ✓

⚠ **[EVIDENCE] 144 Hz is validated on-wire too** (refines CORRECTIONS #6): the *next* IMP call
(`payload-trace.txt:278`, reply `:366`) carries `maxPixelClkKHz=0x90880=592000`,
`rasterSize=0xa6a×0x607=2666×1543` — the 144 Hz EDID timing. So the 144 Hz raster/pclk are
**[EVIDENCE]** in the IMP trace; only the 144 Hz **EVO modeset data words** were uncaptured.

**[UNCERTAIN/TODO] The answer fields are not captured.** `bIsPossible`/`dispClkKHz` sit deep in the
1924-byte struct (after `head[8]`+`window[32]`); the `RVGRESP` dump truncates ~word 18, so the reply
(`rpc-resp-trace.txt:365`) only re-shows `base/numHeads/numWindows/head[0]`. The modeset proceeded
(visible pixels), so `bIsPossible=TRUE` is implied but unverified from the dword (TODO-F08-A).

> Companion `IS_MODE_POSSIBLE_OR_SETTINGS` (`0xc3720102`, `ctrlc372chnc.h:565`) checks raw OR pixel
> clock and is **explicitly not used for DP SORs** ("handled by displayport library", `:570-572`) —
> not observed in the trace. **informational.**

---

### F08.8 — Minimal first-pixel path (GA102 + DP `0x800`), ordered

**ESSENTIAL (do these):**
1. `INTERNAL_DISPLAY_GET_STATIC_INFO` / `GET_IP_VERSION` / `WRITE_INST_MEM` /
   `CHANNEL_PUSHBUFFER` (NV2080) — display HW inventory + channel binds (F08.2; detail F07).
2. `SYSTEM_GET_NUM_HEADS` → 4 (use **head 0**). `SYSTEM_GET_SUPPORTED` → mask `0xff00`.
   `SYSTEM_GET_CONNECT_STATE` → connected `0x2800`; **DP target = `0x800`**.
3. `DFP_GET_INFO(0x800)` → confirm `_SIGNAL=DISPLAYPORT` (`flags=0x0208001b`).
4. `SPECIFIC_OR_GET_INFO(0x800)` → **SOR 1 / protocol DP_B** (feeds `SOR_SET_CONTROL`, F09);
   `DFP_ASSIGN_SOR` confirms/sets the routing (CROSS_BAR is supported here).
5. `SPECIFIC_GET_EDID_V2(0x800)` → the 2560×1440 timing rows (60/144 Hz).
6. DP link: `DP_SET_MANUAL_DISPLAYPORT` → read sink DPCD via `DP_AUXCH_CTRL` (caps @0x0) → **`DP_CTRL`
   with `data={lanes,linkBW}`** = `{2,0x14}` for 60 Hz or `{4,0x14}` for 144 Hz, check `err==0`
   (GSP-RM trains). Optionally read back with `DP_GET_LINK_CONFIG`.
7. `IS_MODE_POSSIBLE` with `numHeads=1, head[0]={EDID timing, _NO_LOCK/_UNSPECIFIED}`, `numWindows=1,
   window[0]={owningHead=0, formatUsageBound incl. RGB_PACKED_4_BPP}` → require `bIsPossible=TRUE`,
   consume `dispClkKHz`.

**informational / skippable for one static DP mode:** `GET_CAPS_V2`, `GET_ALL_HEAD_MASK`,
`GET_VALID_HEAD_WINDOW_ASSIGNMENT`, `GET_CONNECTOR_DATA`, `GET_TYPE`, `GET_PCLK_LIMIT` (IMP is
authority), `GET_ACTIVE`, `GET_BOOT_DISPLAYS`, `SET_EDID_V2`, `SET_OD_PACKET`, HDMI/backlight/
direct-mode/adaptive-sync/dongle/I2C-portid probes, `DP_GET_CAPS` (bounds only), `DP_GET_LINK_CONFIG`
(read-back), the SOURCE_OUI/HDCP/DSC/LTTPR/MST AUX traffic, and `IS_MODE_POSSIBLE_OR_SETTINGS`.

**Hard requirement:** the timing fed to `IS_MODE_POSSIBLE.head[0]` and to F09's `HEAD_SET_RASTER_*`
must be **byte-identical** EDID values (proven: `nvkms-evo3.c` routes the same `pTimings` to both;
241500/2720×1481 appears in IMP input, IMP reply, and the EVO dwords). The link must carry
`pixelClock×bpp`: **2×HBR2 @8 bpc for 60 Hz**, **4×HBR2 @8 bpc for 144 Hz**.

---

### Evidence cited

Headers (`SRC/src/common/sdk/nvidia/inc/ctrl/`):
- `ctrl0073/ctrl0073system.h`: 131/135-139 (`GET_NUM_HEADS`), 306/310-314 (`GET_SUPPORTED`),
  388/392-397/400-412 (`GET_CONNECT_STATE`+flags), 642/646-651 (`GET_ACTIVE`).
- `ctrl0073/ctrl0073specific.h`: 66/70-80 (`GET_TYPE`+types), 145/151-157/159-169 (`GET_EDID_V2`+flags),
  404-416 (`GET_CONNECTOR_DATA`), 987-993 (`GET_PCLK_LIMIT`),
  1078/1082-1096/1099-1146 (`OR_GET_INFO`+type/proto/dither/location enums).
- `ctrl0073/ctrl0073dfp.h`: 118/122-127 (`DFP_GET_INFO`), 130-196 (flag fields: SIGNAL 130-136,
  LANE 137-142, HDMI_CAPABLE 149-151, DP_LINK_BW 173-177, POST_CURSOR2 190-192),
  516-519 (SOR types), 584/590-601 (`DFP_ASSIGN_SOR`).
- `ctrl0073/ctrl0073dp.h`: 151/156-166/168-191 (`DP_AUXCH_CTRL`+cmd/reply), 504/508-516/518-641
  (`DP_CTRL`+cmd/data/err, `_SET_LINK_BW` 0x14=5.40 @586), 1399/1403-1417 (`DP_GET_LINK_CONFIG`),
  1667/1671-1673 + 1649-1651 (`DP_SET_MANUAL_DISPLAYPORT`+semantics),
  **1758-1760 (stale maxLinkRate comment) vs 1781/1785-1800 + 1802-1807 + 1810-1815
  (`DP_GET_CAPS` struct + versions + the authoritative increasing `_MAX_LINK_RATE` enum)**.
- `ctrlc372/ctrlc372chnc.h`: 36-37 (MAX heads/windows), 39 (cmd id), 393-448 (`IMP_HEAD`),
  451-465 (`IMP_WINDOW`), 467-468 (options), 472-512 (`IS_MODE_POSSIBLE_PARAMS`),
  516-532 (formats), 535-547 (fail reasons), 565-572 (`OR_SETTINGS` "not for DP SOR").
- `ctrl2080/ctrl2080internal.h`: 71-79/886-891/916-918/1365-1373 (`GET_STATIC_INFO`/`WRITE_INST_MEM`/
  `GET_IP_VERSION`/`CHANNEL_PUSHBUFFER`).

Source (`SRC/src/...`):
- `common/displayport/src/dp_evoadapter.cpp`: 126-128 (SET_MANUAL), 262-306 + **293-300
  (`maxLinkRate`→RBR/HBR/HBR2/HBR3 decode)**, 762 (AUXCH), 1084-1096 (`train`→`DP_CTRL`).
- `common/displayport/src/dp_connectorimpl.cpp`: 964-986 (`getMaxLinkConfig`), 3501 (`assessLink`),
  4118 (`trainLinkOptimized`).
- `nvidia-modeset/src/nvkms-dpy.c`: 1126-1138 (EDID read, `_COPY_CACHE_NO`).
- `nvidia-modeset/src/nvkms-evo.c`: 4739-4769 (`nvAssignSOREvo`, CROSS_BAR gate + `DFP_ASSIGN_SOR`).
- `nvidia-modeset/src/nvkms-evo3.c`: 3051-3065 (`AssignPerHeadImpParams`), 3201/3271/3276 (IMP issue).
- `nvidia-modeset/src/nvkms-modeset.c`: 2035 (`nvAssignSOREvo` call site).

Artifacts:
- `CAP/payload-trace.txt` (inputs): 95 (SET_MANUAL), 97 (GET_CAPS sorIndex=0xffffffff),
  154 (CHANNEL_PUSHBUFFER core), 196 (CONNECT_STATE 0xff00), 197-205 (AUXCH burst), 200 (DPCD0 read),
  251 (EDID 0x800), 256 (LINK_CONFIG 0x800), 277 (IMP head[0] 60 Hz), 278 (IMP head[0] 144 Hz).
- `CAP2/rpc-resp-trace.txt` (outputs): 11 (IP_VERSION 0x04010000), 35 (GET_STATIC_INFO:
  windowPresentMask=0xff/numHeads=4), 38 (WRITE_INST_MEM), 118 (GET_SUPPORTED 0xff00/0xff00),
  123 (OR_GET_INFO 0x800→SOR1/DP_B), 142 (NUM_HEADS=4), 156 (SET_MANUAL), **158 (GET_CAPS
  maxLinkRate=4=HBR3/DP1.4/MST/FEC)**, 175/176 (DFP_GET_INFO 0x800=0x0208001b; 159/160 = the other DP port displayId 0x100,
  identical 0x0208001b port caps), 177 (GET_TYPE=DFP),
  208 (PCLK_LIMIT 0x800=2.67 GHz), 219 (CHANNEL_PUSHBUFFER c67d), 223/225 (GET_ACTIVE pre-modeset:
  head2→0x800, head3→0x2000), 284 (CONNECT_STATE out 0x2800), 288 (DPCD caps 12 14 c4 01),
  294 (link status 77 00 01 @ DPCD 0x200C), 338/339 (EDID 0x800→AOC/0x2410, bufferSize 0x180),
  344 (LINK_CONFIG 2 lanes/0x14 HBR2), 365 (IMP reply 60 Hz, truncated), 366 (IMP reply 144 Hz),
  835/849/851/853 (DP_CTRL: reset→4×HBR2 assess→fake-LT→2×HBR2 final, all err=0),
  838/840 (DFP_ASSIGN_SOR: SOR0=0x2000, SOR1=0x800).
- `AN/20260530-115116-open-capture-evo-full.txt`: 4681 (head3 pclk 241.5 MHz), 4711 (head3
  DISPLAY_ID=0x800), 4750 (head2 pclk 241.7 MHz), 4780 (head2 DISPLAY_ID=0x2000).
- `CAP/modetest-list.txt`: 15 (88=DP-1 disconnected), 71 (93=DP-2 connected), 74-77 (DP modes
  incl. 60/144 Hz), 149-153 (96=HDMI-A-2 connected, 60 Hz pclk 241700).
- `CAP/modetest-set-pref.txt`, `CAP/modetest-set-144.txt`, and the CAP2 equivalents: all read
  *"failed to find mode … for connector 88"* (88 disconnected).
- `AN/20260530-110540-open-capture-decoded-full.txt`: census counts (IS_MODE_POSSIBLE=121,
  DP_AUXCH_CTRL=43, OR_GET_INFO=26, DFP_GET_INFO=14, GET_CONNECT_STATE=10, GET_EDID_V2=4;
  `DFP_ASSIGN_SOR=0` in this minimal pass — but present in the full modeset, see CAP2:838/840).

---

### Open questions / TODO
- **TODO-F08-A (IMP answer not captured):** `bIsPossible`/`dispClkKHz`/`minRequiredBandwidthKBPS`
  lie past the ~31-dword `RVGRESP` truncation. Dump the full reply (≥1980 B) or add a targeted
  post-RPC trace of those offsets in `NVC372_CTRL_IS_MODE_POSSIBLE_PARAMS` (`ctrlc372chnc.h:472-512`).
- **TODO-F08-1 (144 Hz active link):** the 4×HBR2 *assessment* is measured (`err=0`); capture a real
  144 Hz modeset's `DP_CTRL`/`DP_GET_LINK_CONFIG` to confirm the active trained config is 4×HBR2.
- **TODO-F08-2 (bpc):** confirm the programmed bit depth is 8 bpc (EDID supports it; no 10 bpc/YCbCr
  path) — gates the 144 Hz headroom claim.
- **TODO-F08-3 (implicit DP path):** verify that skipping `DP_SET_MANUAL_DISPLAYPORT` lets RM
  auto-train during SOR attach (only the manual/library path was captured).
- **TODO-F08-4 (displayId↔DRM connector):** the `0x800`↔DP-2(93) mapping is [INFERENCE] (no direct
  namespace map on-wire). Confirm via the nvidia-drm connector→displayId table if a hard mapping is
  needed. BRIEF §3 "connector 88" should be treated as the disconnected DP-1, not the target.
- **TODO-F08-5 (counts are capture-specific):** the census (110540) shows `DFP_ASSIGN_SOR=0` and 43
  AUXCH; the full modeset (115116) has `DFP_ASSIGN_SOR` present and 48 AUXCH. State which capture when
  quoting counts; the minimal-pass census understates the modeset's control set.
- **TODO-F08-6 (HDMI sink, out of scope):** `0x2000`→SOR 0 is from `DFP_ASSIGN_SOR`; its protocol
  (SINGLE_TMDS_A) was not separately read via `OR_GET_INFO` here (informational; not the DP target).


## F09 — Modeset & Scanout: EVO Method Program, 144Hz & Draw-a-Pixel

Final, judge-verified merge of **S10** (method program, header-only capture), **D06** (measured
CORE field values), **D07** (measured WINDOW field values + draw-a-pixel), and **D10** (pixel-clock /
raster math + VPLL). This is the complete first-pixel critical path for the RTX 3080 (GA102,
nvdisplay 4.0): the field-level CORE modeset program (measured 60 Hz, computed 144 Hz), the WINDOW
scanout program (surface bind, `X8R8G8B8`, stride, `UPDATE`), the "draw a red pixel" pseudocode for
StelluxOS, and how the pixel clock/VPLL is actually synthesized. Allocation/ctxdma (S-mem), channel
& pushbuffer plumbing (S08), and detect/EDID/head-assignment (S-detect) are cross-referenced, not
duplicated.

Every value below was re-verified by this judge against the class headers (`clc67d.h`/`clc67e.h`),
the channel-attributed measured capture (`evo-full.txt`), the open-source logic (`nvkms-evo.c`/
`nvkms-evo3.c`), and recomputed arithmetic. **Honest tiers** (per CONTEXT_BRIEF §6):
- **[MEASURED]** = literal data word captured on this GA102 (`…115116…evo-full.txt`, channel-attributed).
- **[EDID]** = read from the monitor's mode list (`modetest-list.txt`).
- **[EVIDENCE]** = field/formula quoted from the source tree (`path:line`).
- **[INFERENCE]** = computed/reasoned from the above (states from what).
- **[TODO]/[UNCERTAIN]** = not confirmable from the files.

---

### F09.0 Verification verdict & what changed from the drafts

**Verdict: the drafts are accurate.** All header offsets/bitfields, all measured 60 Hz/window
dwords, every cited `nvkms` formula, and all arithmetic (60 Hz packing, 144 Hz computed packing,
refresh = pclk/(hTotal·vTotal)) verified **bit-for-bit**. Corrections applied during the merge:

| # | Field / claim | S10 (header-only) said | Final (measured / verified) | Basis |
|---|---|---|---|---|
| A | core `UPDATE` value | `SPECIAL_HANDLING=MODE_SWITCH` ⇒ `0x200001` | **`0x1`** (`RELEASE_ELV=TRUE`, `SPECIAL_HANDLING=NONE`) | [MEASURED] evo-full:4974 |
| B | SOR protocol (live panel) | `DP_A` (0x8) | **`DP_B` (0x9)** | [MEASURED] evo-full:4710 (`0x908`) |
| C | window `SET_STORAGE` | has `MEMORY_LAYOUT=PITCH/BLOCKLINEAR` | **no `MEMORY_LAYOUT` on Ampere**; only `BLOCK_HEIGHT` | [EVIDENCE] clc67e.h:131-138; evo3.c:7936 cap FALSE |
| D | window `SET_COMPOSITION_CONTROL` | "disable / single opaque" (implied 0) | **`0x00010000`** (`BYPASS=ENABLE`) | [MEASURED] evo-full:4866 |
| E | "draw a pixel" dword | `0x00FF00FF` labelled "ARGB" (= magenta) | **`0x00FF0000`** = pure red, `X8R8G8B8` | [EVIDENCE] clc67e.h:147 + [INFERENCE] byte order |
| F | 60 Hz field values | [INFERENCE] from EDID+formula | **[MEASURED]** (upgraded) | [MEASURED] evo-full:4674-4721 |
| G | base `HEAD_SET_PIXEL_CLOCK_FREQUENCY` | (only `_MAX` "isolated"; D10 called it an EVIDENCE-GAP) | **[MEASURED]** 241.5/241.7 MHz at off `0x2c0c`/`0x280c` — D10's "gap" was the decoder *name-column* collision (CORRECTIONS #5); re-mapped by offset it is captured | [MEASURED] evo-full:4679,4748 |
| H | head↔SOR↔displayId (S10 TODO) | unknown | **head 3 → SOR 1 → DP_B → displayId 0x800** = live DP panel | [MEASURED] D06.1 |
| I | how many monitors | "one panel, two heads?" | **two** connected outputs (heads 2 & 3); first-pixel target = the **DP** panel | [MEASURED] D06.1/D10.3, CORRECTIONS #1 |
| J | 144 Hz dwords | [INFERENCE] | **kept [INFERENCE]/EDID-derived** (the `2560x1440-144` set never re-modeset) | CORRECTIONS #6; modetest-set-144.txt:1 |

Nothing was *removed* as fabricated — only the C37E-inherited `MEMORY_LAYOUT` field (C) is dropped as
not-applicable on Ampere, and the magenta literal (E) is corrected to red.

**Decoder caveat (CORRECTIONS #5 — applied throughout).** `decode_evo_full.py` resolves an offset to
the *first* indexed `NAME(b)` define in header order with `(off-base)%stride==0` and index `<64`, so
several per-head/per-window methods print a **wrong name**. The `off=`/`data=` columns are
authoritative; **every method identity below is re-mapped from the captured `off=` via the class
header**, not the printed name. Three load-bearing examples that were mislabeled in the dump:

| captured `off=` | dump's (WRONG) name | correct method (by header offset) |
|---|---|---|
| `0x2c0c` | `WINDOW_SET_MAX_INPUT_SCALE_FACTOR[56]` | `HEAD_SET_PIXEL_CLOCK_FREQUENCY[3]` (`0x200C+3·0x400`) |
| `0x2c00` | `WINDOW_SET_CONTROL[56]` | `HEAD_SET_PROCAMP[3]` (`0x2000+3·0x400`) |
| `0x0320` | `SET_GET_BLANKING_CTRL[56]` | `SOR_SET_CONTROL[1]` (`0x300+1·0x20`) |
| `0x0240` | `SET_PLANAR_STORAGE[4]` | `SET_CONTEXT_DMA_ISO[0]` (`0x240+0·4`) |
| `0x0260` | `SET_PLANAR_STORAGE[12]` | `SET_OFFSET[0]` (`0x260+0·4`) |

---

### F09.1 The targets — two monitors, one live DP panel (first-pixel target)

[MEASURED] The captured modeset is **one atomic core update programming two heads** (`RASTER_SIZE`
written exactly twice), i.e. two connected monitors:

| head | displayId | RASTER_SIZE | base pclk (= `_MAX`) | SOR (owner/protocol) | output |
|---|---|---|---|---|---|
| **3** | `0x800` | `0x05c90aa0` | `0x0e64ff60` = **241.5 MHz** / 59.95 Hz | SOR1 `0x908` → HEAD3 / **DP_B** | **DP panel (AOC AG241QG4), 144 Hz-capable — TARGET** |
| 2 | `0x2000` | `0x05c90aa0` | `0x0e680ca0` = **241.7 MHz** / 60.00 Hz | SOR0 `0x104` → HEAD2 / SINGLE_TMDS_A | HDMI/TMDS second monitor (not on the first-pixel path) |

[INFERENCE→for first pixel] **Program exactly one head: HEAD 3** → SOR 1 → protocol DP_B →
`HEAD_SET_DISPLAY_ID=0x800` → pclk 241.5 MHz. Both heads share identical `2720×1481` raster, so the
only per-head deltas are pclk, displayId, and SOR/protocol.

**Connector-id namespaces differ — state all three, do not call any "disconnected" unqualified**
(CONTEXT_BRIEF §3 reconcile, CORRECTIONS #1):
- **RM `displayId`** (the EVO/`HEAD_SET_DISPLAY_ID` token, unambiguous & measured): **`0x800`** = DP panel.
- **DRM/`modetest` connector id**: brief §3 names **88**; the `112551` capture shows **93 "DP-2"**
  connected (`modetest-list.txt:71`), and the second monitor as **96 "HDMI-A-2"**. The `115116`
  `modetest` `2560x1440-144` request targeted connector **88** and **failed** (it never re-modeset).
- **DRM connector index** (separate again). [TODO] the definitive head↔OR↔connector binding is the
  `IS_MODE_POSSIBLE`/head-assignment RPC (S-detect), not the EVO trace.

---

### F09.2 Field-encoding rules (the packing an implementer must reproduce)

1. **WxH / X,Y packing (core raster & viewport):** width/X in low 15 bits `[14:0]`, height/Y in
   `[30:16]` ⇒ `dword = (Y << 16) | X`. (clc67d.h:829-839, 817-824.)
   Exception: `VIEWPORT_POINT_OUT_ADJUST` is signed `X[15:0]/Y[31:16]` (clc67d.h:826-827).
   **Window** `SET_SIZE*` packing is `WIDTH[15:0]/HEIGHT[31:16]` (16-bit each; clc67e.h:129-130).
2. **`RASTER_SIZE` is literal totals** (NOT decremented): `WIDTH=hTotal`, `HEIGHT=vTotal`.
3. **Sync/blank are 0-based** ("hw adds one"), so every sync/blank edge is the natural count **−1**.
   Forward DRM→EVO derivation (authoritative formula, [EVIDENCE]):
```5445:5450:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia-modeset/src/nvkms-evo.c
    pTimings->rasterSyncEnd.x           = hSyncWidth - 1;
    pTimings->rasterSyncEnd.y           = vSyncWidth - 1;
    pTimings->rasterBlankStart.x        = hBlankStart - 1;
    pTimings->rasterBlankStart.y        = vBlankStart - 1;
    pTimings->rasterBlankEnd.x          = hBlankEnd - 1;
    pTimings->rasterBlankEnd.y          = vBlankEnd - 1;
```
   with `hSyncWidth=hSE−hSS`, `hBlankEnd=hTot−hSS`, `hBlankStart=hVis+(hTot−hSS)` (V analogous;
   nvkms-evo.c:5427-5437), and `pixelClock` stored in **kHz** (`HzToKHz`, nvkms-evo.c:5395).
4. **Active recovery / sanity:** `active = blankStart − blankEnd` (X and Y). 60 Hz: `(2671−111,
   1477−37) = 2560×1440` ✓.
5. **Pixel clock:** `HERTZ[30:0] = pixelClock_kHz × 1000`; bit `[31] ADJ1000DIV1001` (NTSC ×1000/1001)
   = FALSE here. `refresh = HERTZ / (hTotal × vTotal)`.

---

### F09.3 CORE channel (NVC67D) modeset program — field-level, MEASURED 60 Hz (HEAD 3 = live panel)

Per-head hardware address `= base + head·0x400`; for head 3 add `0xC00`. Every "data" cell is a real
captured dword. Offsets/bitfields are quoted from `clc67d.h` and were verified exact.

| # | Method (clc67d.h base) | head-3 off | MEASURED data | decoded fields | evo-full | src |
|---|---|---|---|---|---|---|
| 1 | `HEAD_SET_PROCAMP` (0x2000) | `0x2c00` | `0x00000000` | `COLOR_SPACE[1:0]=RGB(0)` | 4694/4721 | clc67d.h:491-493 |
| 2 | `HEAD_SET_CONTROL` (0x2008) | `0x2c08` | `0x00000000` | `STRUCTURE[1:0]=PROGRESSIVE(0)` | 4686 | clc67d.h:567-569 |
| 3 | `HEAD_SET_PIXEL_CLOCK_FREQUENCY` (0x200C) | `0x2c0c` | `0x0e64ff60` | `HERTZ[30:0]=241,500,000`; `ADJ1000DIV1001[31]=0` | 4679 | clc67d.h:693-695 |
| 4 | `HEAD_SET_PIXEL_CLOCK_CONFIGURATION` (0x201C) | `0x2c1c` | `0x00000000` | `NOT_DRIVER[0]=FALSE`; `HOPPING=DISABLE` | 4680 | clc67d.h:727-728 |
| 5 | `HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX` (0x2028) | `0x2c28` | `0x0e64ff60` | `HERTZ[30:0]=241,500,000` (= base) | 4681 | clc67d.h:739-740 |
| 6 | `HEAD_SET_DISPLAY_ID(h,0)` (0x2020) | `0x2c20` | `0x00000800` | `CODE[31:0]=0x800` (binds head→RM displayId) | 4711 | clc67d.h:737-738 |
| 7 | `HEAD_SET_RASTER_SIZE` (0x2064) | `0x2c64` | `0x05c90aa0` | `WIDTH=2720` (hTotal); `HEIGHT=1481` (vTotal) | 4674 | clc67d.h:828-830 |
| 8 | `HEAD_SET_RASTER_SYNC_END` (0x2068) | `0x2c68` | `0x0004001f` | `X=31` (hSyncW−1); `Y=4` (vSyncW−1) | 4675 | clc67d.h:831-833 |
| 9 | `HEAD_SET_RASTER_BLANK_END` (0x206C) | `0x2c6c` | `0x0025006f` | `X=111`; `Y=37` (end-of-blank/start-of-active) | 4676 | clc67d.h:834-836 |
| 10 | `HEAD_SET_RASTER_BLANK_START` (0x2070) | `0x2c70` | `0x05c50a6f` | `X=2671`; `Y=1477` (start-of-blank/end-of-active) | 4677 | clc67d.h:837-839 |
| 11 | `HEAD_SET_VIEWPORT_SIZE_IN` (0x204C) | `0x2c4c` | `0x05a00a00` | `WIDTH=2560`; `HEIGHT=1440` (= active) | 4712 | clc67d.h:819-821 |
| 12 | `HEAD_SET_VIEWPORT_SIZE_OUT` (0x2058) | `0x2c58` | `0x05a00a00` | `WIDTH=2560`; `HEIGHT=1440` (no scale) | 4714 | clc67d.h:822-824 |
| 13 | `HEAD_SET_VIEWPORT_POINT_IN` (0x2048) | `0x2c48` | `0x00000000` | `X=0; Y=0` | 4719 | clc67d.h:816-818 |
| 14 | `HEAD_SET_VIEWPORT_POINT_OUT_ADJUST` (0x205C) | `0x2c5c` | `0x00000000` | `X=0; Y=0` (no pan) | 4713 | clc67d.h:825-827 |
| 15 | `HEAD_SET_RASTER_HBLANK_DELAY` (0x2364) | `0x2f64` | `0x00000000` | `BLANK_START=0; BLANK_END=0` (no DSC/FRL) | 4685 | clc67d.h:1323-1325 |
| 16 | `SOR_SET_CONTROL(or=1)` (0x0300+or·0x20) | `0x0320` | `0x00000908` | `OWNER_MASK[7:0]=0x08=HEAD3`; `PROTOCOL[11:8]=0x9=DP_B`; `DE_SYNC_POLARITY[16]=POSITIVE_TRUE(0)` | 4710 | clc67d.h:301-322 |
| 17 | `UPDATE` (0x0200) | `0x0200` | `0x00000001` | `RELEASE_ELV[0]=TRUE`; `SPECIAL_HANDLING[21:20]=NONE(0)` | 4974 | clc67d.h:82-93 |

**HEAD 2 deltas** (same offsets −`0x400`): `PIXEL_CLOCK_FREQUENCY`/`_MAX` = `0x0e680ca0` (**241.7
MHz**, off `0x280c`/`0x2828`, evo-full:4748/4750); `DISPLAY_ID` = `0x2000` (off `0x2820`, :4780);
`SOR_SET_CONTROL(or=0)` = `0x104` → HEAD2 / SINGLE_TMDS_A (off `0x0300`, :4779). Raster/viewport
dwords identical to head 3.

Load-bearing field defs, verbatim:
```694:697:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class/clc67d.h
#define NVC67D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_HERTZ                             30:0
#define NVC67D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_ADJ1000DIV1001                    31:31
#define NVC67D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_ADJ1000DIV1001_FALSE              (0x00000000)
#define NVC67D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_ADJ1000DIV1001_TRUE               (0x00000001)
```
```301:327:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class/clc67d.h
#define NVC67D_SOR_SET_CONTROL(a)                                               (0x00000300 + (a)*0x00000020)
#define NVC67D_SOR_SET_CONTROL_OWNER_MASK                                       7:0
#define NVC67D_SOR_SET_CONTROL_OWNER_MASK_HEAD2                                 (0x00000004)
#define NVC67D_SOR_SET_CONTROL_OWNER_MASK_HEAD3                                 (0x00000008)
#define NVC67D_SOR_SET_CONTROL_PROTOCOL                                         11:8
#define NVC67D_SOR_SET_CONTROL_PROTOCOL_SINGLE_TMDS_A                           (0x00000001)
#define NVC67D_SOR_SET_CONTROL_PROTOCOL_DP_A                                    (0x00000008)
#define NVC67D_SOR_SET_CONTROL_PROTOCOL_DP_B                                    (0x00000009)
#define NVC67D_SOR_SET_CONTROL_DE_SYNC_POLARITY                                 16:16
#define NVC67D_SOR_SET_CONTROL_DE_SYNC_POLARITY_POSITIVE_TRUE                   (0x00000000)
#define NVC67D_SOR_SET_CONTROL_PIXEL_REPLICATE_MODE                            21:20
```
```82:93:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class/clc67d.h
#define NVC67D_UPDATE                                                           (0x00000200)
#define NVC67D_UPDATE_SPECIAL_HANDLING                                          21:20
#define NVC67D_UPDATE_SPECIAL_HANDLING_NONE                                     (0x00000000)
#define NVC67D_UPDATE_SPECIAL_HANDLING_MODE_SWITCH                              (0x00000002)
#define NVC67D_UPDATE_RELEASE_ELV                                               0:0
#define NVC67D_UPDATE_RELEASE_ELV_TRUE                                          (0x00000001)
```

[EVIDENCE] The host assembles these via the inherited `C37D` macros (identical offsets to `C67D`);
the raster/pclk burst is one `nvkms` routine:
```1299:1317:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia-modeset/src/nvkms-evo3.c
    nvDmaSetStartEvoMethod(pChannel, NVC37D_HEAD_SET_RASTER_SIZE(head), 1);
    nvDmaSetEvoMethodData(pChannel,
        DRF_NUM(C37D, _HEAD_SET_RASTER_SIZE, _WIDTH, pTimings->rasterSize.x) |
        DRF_NUM(C37D, _HEAD_SET_RASTER_SIZE, _HEIGHT, pTimings->rasterSize.y));
    nvDmaSetStartEvoMethod(pChannel, NVC37D_HEAD_SET_RASTER_SYNC_END(head), 1);
    nvDmaSetEvoMethodData(pChannel,
        DRF_NUM(C37D, _HEAD_SET_RASTER_SYNC_END, _X, pTimings->rasterSyncEnd.x) |
        DRF_NUM(C37D, _HEAD_SET_RASTER_SYNC_END, _Y, pTimings->rasterSyncEnd.y));
    nvDmaSetStartEvoMethod(pChannel, NVC37D_HEAD_SET_RASTER_BLANK_END(head), 1);
    nvDmaSetEvoMethodData(pChannel,
        DRF_NUM(C37D, _HEAD_SET_RASTER_BLANK_END, _X, pTimings->rasterBlankEnd.x) |
        DRF_NUM(C37D, _HEAD_SET_RASTER_BLANK_END, _Y, pTimings->rasterBlankEnd.y));
    nvDmaSetStartEvoMethod(pChannel, NVC37D_HEAD_SET_RASTER_BLANK_START(head), 1);
    nvDmaSetEvoMethodData(pChannel,
        DRF_NUM(C37D, _HEAD_SET_RASTER_BLANK_START, _X, pTimings->rasterBlankStart.x) |
        DRF_NUM(C37D, _HEAD_SET_RASTER_BLANK_START, _Y, pTimings->rasterBlankStart.y));
```
```2188:2193:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia-modeset/src/nvkms-evo3.c
    nvDmaSetStartEvoMethod(pChannel, NVC37D_SOR_SET_CONTROL(orIndex), 1);
    nvDmaSetEvoMethodData(pChannel,
        DRF_NUM(C37D, _SOR_SET_CONTROL, _OWNER_MASK, headMask) |
        DRF_NUM(C37D, _SOR_SET_CONTROL, _PROTOCOL, hwProtocol) |
        DRF_DEF(C37D, _SOR_SET_CONTROL, _DE_SYNC_POLARITY, _POSITIVE_TRUE) |
        DRF_DEF(C37D, _SOR_SET_CONTROL, _PIXEL_REPLICATE_MODE, _OFF));
```

[MEASURED] **Interlock.** The real commit is window-interlocked: core sets
`SET_WINDOW_INTERLOCK_FLAGS=0xf0` (off `0x21c`, evo-full:4973) then `UPDATE=0x1` (:4974); each active
window sets `SET_INTERLOCK_FLAGS=0x1` + `SET_WINDOW_INTERLOCK_FLAGS=0xf0` before its own `UPDATE`
(:4975-4986), so head modeset + window flips latch atomically. **For first-pixel v1 you may skip the
interlock entirely** and do core `UPDATE` then window `UPDATE` sequentially (S08.6: `interlockMask=0`
is valid) — see F09.10.

---

### F09.4 144 Hz substitution — same program, two knobs changed (EDID-derived, [INFERENCE])

[EDID] The 144 Hz mode for the DP panel (`modetest-list.txt:75`, capture 112551):
```74:75:/home/flare/dev/gpu-repro/traces/20260530-112551-open-capture/modetest-list.txt
  #0 2560x1440 59.95 2560 2608 2640 2720 1440 1443 1448 1481 241500 flags: phsync, pvsync; type: preferred
  #1 2560x1440 143.91 2560 2568 2600 2666 1440 1465 1473 1543 592000 flags: phsync, nvsync; type: 
```
[INFERENCE — computed via the F09.2 formula; **not captured**, the `2560x1440-144` set failed]:

| Method | 60 Hz MEASURED dword (decode) | 144 Hz computed dword (decode) |
|---|---|---|
| `RASTER_SIZE` | `0x05c90aa0` (2720×1481) | **`0x06070a6a`** (2666×1543) |
| `RASTER_SYNC_END` | `0x0004001f` (X31,Y4) | **`0x0007001f`** (X31,Y7) |
| `RASTER_BLANK_END` | `0x0025006f` (X111,Y37) | **`0x004d0061`** (X97,Y77) |
| `RASTER_BLANK_START` | `0x05c50a6f` (X2671,Y1477) | **`0x05ed0a61`** (X2657,Y1517) |
| `PIXEL_CLOCK_FREQUENCY.HERTZ` (+`_MAX`) | `0x0e64ff60` (241.5 MHz) | **`0x23493400`** (592 MHz) |
| `VIEWPORT_SIZE_IN/OUT`, PROCAMP, CONTROL, PCLK_CONFIG, DISPLAY_ID, SOR | unchanged | unchanged |

Self-checks: active `= (2657−97)×(1517−77) = 2560×1440` ✓; `refresh = 592e6/(2666·1543) = 143.91`
✓. **Only `PIXEL_CLOCK_FREQUENCY(+_MAX)` and the four raster dwords change** between 60↔144;
everything else is byte-identical. [CAVEAT — CORRECTIONS #6] these 144 Hz dwords are computed from
EDID + the verified 60 Hz formula, not on-wire:
```1:1:/home/flare/dev/gpu-repro/traces/20260530-115116-open-capture/modetest-set-144.txt
failed to find mode "2560x1440-144.00Hz" for connector 88
```
The DP link must carry ≥592 MHz; gate with `GET_PCLK_LIMIT` + `IS_MODE_POSSIBLE` (F09.6). NOTE
[CORRECTIONS #1]: no on-screen 144 Hz test pattern was ever rendered — both `modetest -s` runs
**failed** on the disconnected connector 88 (the `awk '/connected/'` bug matched "dis**connected**").
The captured EVO methods are the driver's **load-time** modeset on the connected heads (valid data);
the 144 Hz dwords in F09.4 are **EDID-derived [INFERENCE]**, not on-wire.

---

### F09.5 Pixel-clock + raster math, with refresh self-checks

**Refresh identity** (CONTEXT_BRIEF §4e): `refresh = pixelClock / (hTotal × vTotal)`. hTotal/vTotal
are the **raster totals**, not the active area.

| Mode | pixelClock (Hz) | hT × vT | product | refresh | tier |
|---|---|---|---|---|---|
| 60 — DP-2 (head 3) | 241,500,000 | 2720 × 1481 | 4,028,320 | **59.95** | [MEASURED]+[EDID] |
| 60 — HDMI (head 2) | 241,700,000 | 2720 × 1481 | 4,028,320 | **60.00** | [MEASURED]+[EDID] |
| 144 — DP-2 | 592,000,000 | 2666 × 1543 | 4,113,638 | **143.91** | [EDID]/[INFERENCE] |

**60 Hz packing self-check (matches captured dwords bit-for-bit):**
`RASTER_SIZE = (1481<<16)|2720 = 0x05c90aa0` ✓; `SYNC_END=(4<<16)|31=0x0004001f` ✓;
`BLANK_END=(37<<16)|111=0x0025006f` ✓; `BLANK_START=(1477<<16)|2671=0x05c50a6f` ✓.
`0x0e64ff60 = 241,500,000` ✓; `0x0e680ca0 = 241,700,000` ✓.

**144 Hz packing self-check:** `RASTER_SIZE=(1543<<16)|2666=0x06070a6a` ✓;
`SYNC_END=(7<<16)|31=0x0007001f` ✓; `BLANK_END=(77<<16)|97=0x004d0061` ✓;
`BLANK_START=(1517<<16)|2657=0x05ed0a61` ✓; `0x23493400 = 592,000,000` ✓.

[EVIDENCE] These derived values equal the captured 60 Hz dwords bit-for-bit — confirming the
`nvkms-evo.c` formula + `clc67d.h` packing **is** what produced the on-wire values.

---

### F09.6 How the pixel clock is actually set — VPLL via GSP-RM (host only *requests* HERTZ)

[EVIDENCE] The host **does not touch any PLL register**. It writes the desired Hz into the core
channel and lets firmware synthesize it:
```1338:1354:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia-modeset/src/nvkms-evo3.c
    nvDmaSetStartEvoMethod(pChannel, NVC37D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(head), 1);
    nvDmaSetEvoMethodData(pChannel,
        DRF_NUM(C37D, _HEAD_SET_PIXEL_CLOCK_FREQUENCY, _HERTZ,
                pTimings->pixelClock * 1000) |
        DRF_DEF(C37D, _HEAD_SET_PIXEL_CLOCK_FREQUENCY, _ADJ1000DIV1001,_FALSE));
    nvDmaSetStartEvoMethod(pChannel, NVC37D_HEAD_SET_PIXEL_CLOCK_CONFIGURATION(head), 1);
    nvDmaSetEvoMethodData(pChannel,
        DRF_DEF(C37D, _HEAD_SET_PIXEL_CLOCK_CONFIGURATION, _NOT_DRIVER, _FALSE) |
        DRF_DEF(C37D, _HEAD_SET_PIXEL_CLOCK_CONFIGURATION, _HOPPING, _DISABLE) |
        DRF_DEF(C37D, _HEAD_SET_PIXEL_CLOCK_CONFIGURATION, _HOPPING_MODE, _VBLANK));
    nvDmaSetStartEvoMethod(pChannel, NVC37D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(head), 1);
    nvDmaSetEvoMethodData(pChannel,
        DRF_NUM(C37D, _HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX, _HERTZ,
                pTimings->pixelClock * 1000) |
        DRF_DEF(C37D, _HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX, _ADJ1000DIV1001,_FALSE));
```
- `NOT_DRIVER=FALSE` ⇒ the **RM/driver owns the clock** (not VBIOS); `HOPPING=DISABLE` ⇒ no spread.
  [MEASURED] this dword = `0x0` on both heads (evo-full:4680,4749).
- `ADJ1000DIV1001=FALSE` ⇒ 59.95/143.91 come from the EDID pixel clock itself, not the ×1000/1001 flag.

[EVIDENCE] The only VPLL token the host emits is a **reference-clock preference hint** via a SW-spare
method — `NO_PREF` normally, `QSYNC` only for genlock:
```28:30:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class/clc37dswspare.h
#define NVC37D_HEAD_SET_SW_SPARE_A_CODE_VPLL_REF                                1:0
#define NVC37D_HEAD_SET_SW_SPARE_A_CODE_VPLL_REF_NO_PREF                        (0x00000000)
#define NVC37D_HEAD_SET_SW_SPARE_A_CODE_VPLL_REF_QSYNC                          (0x00000001)
```
```2122:2136:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia-modeset/src/nvkms-evo3.c
            if (external) {
                pDevEvo->gpus[sd].setSwSpareA[head] =
                    FLD_SET_DRF(C37D, _HEAD_SET_SW_SPARE_A_CODE, _VPLL_REF, _QSYNC,
                                pDevEvo->gpus[sd].setSwSpareA[head]);
            } else {
                pDevEvo->gpus[sd].setSwSpareA[head] =
                    FLD_SET_DRF(C37D, _HEAD_SET_SW_SPARE_A_CODE, _VPLL_REF, _NO_PREF,
                                pDevEvo->gpus[sd].setSwSpareA[head]);
```
[INFERENCE] No VPLL/PLL divider programming exists in the open CPU-side display tree (a search under
`src/nvidia/src/kernel/gpu/disp` finds none). Combined with the method-based request path, the actual
PLL divider programming for the requested `HERTZ` is performed downstream by **GSP-RM** (closed
firmware on this card), triggered by the core `UPDATE`. The host never writes PLL MMIO.

**Two gates the mode must pass first** (both [EVIDENCE]):
1. **`GET_PCLK_LIMIT`** — per-link max pixel clock, cached on the dpy:
```619:634:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia-modeset/src/nvkms-dpy.c
    ret = nvRmApiControl(nvEvoGlobal.clientHandle,
                         pDevEvo->displayCommonHandle,
                         NV0073_CTRL_CMD_SPECIFIC_GET_PCLK_LIMIT,
                         &params, sizeof(params));
    ...
    pDpyEvo->maxPixelClockKHz = params.orPclkLimit;
    pDpyEvo->maxSingleLinkPixelClockKHz = pDpyEvo->maxPixelClockKHz;
```
   A mode is rejected if `pixelClock > maxSingleLinkPixelClockKHz` (nvkms-dpy.c:2579-2580); on DP the
   real ceiling is the **link** rate (HBR2/HBR3 lanes — S-DP). 592,000 kHz must be ≤ this.
2. **`IS_MODE_POSSIBLE` (IMP)** — built from the raster + pixel clock; returns go/no-go, required
   memory bandwidth, and the **dispclk** the pipe will run at:
```3274:3292:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia-modeset/src/nvkms-evo3.c
    ret = nvRmApiControl(nvEvoGlobal.clientHandle,
                         pDevEvo->rmCtrlHandle,
                         NVC372_CTRL_CMD_IS_MODE_POSSIBLE,
                         pImp, sizeof(*pImp));
    ...
    if (ret != NV_OK || !pImp->bIsPossible) {
        goto done;
    }
    result = TRUE;
done:
    pOutput->possible = result;
    if (pOutput->possible) {
        pOutput->minRequiredBandwidthKBPS = pImp->minRequiredBandwidthKBPS;
        pOutput->floorBandwidthKBPS = pImp->floorBandwidthKBPS;
    }
```
   `dispClkKHz` (distinct from the pixel/VPLL clock — it's the compositor clock) is IMP-selected; for
   dGPU it is one of the VBIOS-supplied fixed frequencies (ctrlc372chnc.h:376-379). For the
   first-pixel path (1 head, 1 window, no scaling) the demand is tiny and IMP passes at either rate.

---

### F09.7 WINDOW channel (NVC67E) scanout program — field-level, MEASURED

[MEASURED] One active window flip (`ch=0x80`), the first-pixel-essential methods in logical order.
Offsets/bitfields from `clc67e.h` (verified exact); `nvkms` emits via inherited `C37E` macros.

| # | Method (clc67e.h) | off | MEASURED data | decode | evo-full |
|---|---|---|---|---|---|
| 1 | `SET_PRESENT_CONTROL` | `0x0308` | `0x00000000` | `MIN_PRESENT_INTERVAL[3:0]=0`; `BEGIN_MODE[6:4]=NON_TEARING(0)` | 4822 |
| 2 | `SET_CONTEXT_DMA_ISO[0]` | `0x0240` | `0x00010087` | `HANDLE[31:0]` = fb ctxdma handle (modetest's; **yours differs** [INFERENCE]) | 4823 |
| 3 | `SET_OFFSET[0]` | `0x0260` | `0x00000000` | `ORIGIN[31:0]` = byte offset `>>8` = 0 (surface at ctxdma base) | 4824 |
| 4 | `SET_SIZE` | `0x0224` | `0x05a00a00` | `WIDTH[15:0]=2560`; `HEIGHT[31:16]=1440` | 4835 |
| 5 | `SET_SIZE_IN` | `0x0298` | `0x05a00a00` | `2560×1440` (input viewport) | 4836 |
| 6 | `SET_SIZE_OUT` | `0x02a4` | `0x05a00a00` | `2560×1440` (no scale) | 4837 |
| 7 | `SET_STORAGE` | `0x0228` | `0x00000000` | `BLOCK_HEIGHT[3:0]=ONE_GOB(0)`. **No `MEMORY_LAYOUT` on C6** (F09.8) | 4838 |
| 8 | `SET_PLANAR_STORAGE[0]` | `0x0230` | `0x00000100` | `PITCH[12:0]=256` → `256×64 = 16384 B` stride (padded; tight=`0xA0`) | 4839 |
| 9 | `SET_PARAMS` | `0x022c` | `0x000000e6` | `FORMAT[7:0]=0xE6=X8R8G8B8` | 4844 |
| 10 | `SET_COMPOSITION_CONTROL` | `0x02ec` | `0x00010000` | `BYPASS[16]=ENABLE(1)`; `COLOR_KEY_SELECT[1:0]=DISABLE`; `DEPTH[11:4]=0` | 4866 |
| 11 | `UPDATE` | `0x0200` | `0x00000001` | `RELEASE_ELV[0]=TRUE` (commit → scanout) | 4983 |

[MEASURED] The second active window (`ch=0x20`) is byte-identical and binds the **same** surface
(`ISO[0]=0x00010087`, `SET_SIZE=0x05a00a00`, `PARAMS=0xe6`, evo-full:4905,4917,4926). The two
inactive windows (`ch=0x40`,`0x100`) have `ISO[0]=0` (evo-full:4876,4958). **First pixel programs one
window.**

Field defs, verbatim:
```128:140:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class/clc67e.h
#define NVC67E_SET_SIZE                                                         (0x00000224)
#define NVC67E_SET_SIZE_WIDTH                                                   15:0
#define NVC67E_SET_SIZE_HEIGHT                                                  31:16
#define NVC67E_SET_STORAGE                                                      (0x00000228)
#define NVC67E_SET_STORAGE_BLOCK_HEIGHT                                         3:0
#define NVC67E_SET_STORAGE_BLOCK_HEIGHT_NVD_BLOCK_HEIGHT_ONE_GOB                (0x00000000)
// ... BLOCK_HEIGHT_{TWO,FOUR,EIGHT,SIXTEEN,THIRTYTWO}_GOBS (133-138); NO _MEMORY_LAYOUT field ...
#define NVC67E_SET_PARAMS                                                       (0x0000022C)
#define NVC67E_SET_PARAMS_FORMAT                                                7:0
```
```146:148:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class/clc67e.h
#define NVC67E_SET_PARAMS_FORMAT_A8R8G8B8                                       (0x000000CF)
#define NVC67E_SET_PARAMS_FORMAT_X8R8G8B8                                       (0x000000E6)
#define NVC67E_SET_PARAMS_FORMAT_A8B8G8R8                                       (0x000000D5)
```
```177:184:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class/clc67e.h
#define NVC67E_SET_PLANAR_STORAGE(b)                                            (0x00000230 + (b)*0x00000004)
#define NVC67E_SET_PLANAR_STORAGE_PITCH                                         12:0
#define NVC67E_SET_SEMAPHORE_RELEASE_HI                                         (0x0000023C)
#define NVC67E_SET_SEMAPHORE_RELEASE_HI_VALUE                                   31:0
#define NVC67E_SET_CONTEXT_DMA_ISO(b)                                           (0x00000240 + (b)*0x00000004)
#define NVC67E_SET_CONTEXT_DMA_ISO_HANDLE                                       31:0
#define NVC67E_SET_OFFSET(b)                                                    (0x00000260 + (b)*0x00000004)
#define NVC67E_SET_OFFSET_ORIGIN                                                31:0
```
[EVIDENCE] `SET_OFFSET = nvCtxDmaOffsetFromBytes(offset) = offset >> 8`, with a 1024 B alignment
assert (`NV_SURFACE_OFFSET_ALIGNMENT_SHIFT=10`):
```145:151:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia-modeset/include/nvkms-utils.h
static inline NvU64 nvCtxDmaOffsetFromBytes(NvU64 ctxDmaOffset)
{
    nvAssert((ctxDmaOffset & ((1 << NV_SURFACE_OFFSET_ALIGNMENT_SHIFT) - 1))
             == 0);

    return (ctxDmaOffset >> 8);
}
```
[INFERENCE] `X8R8G8B8` (0xE6) component order MSB→LSB is `X[31:24] R[23:16] G[15:8] B[7:0]`, so a
pixel dword is `0x00RRGGBB`; **pure red = `0x00FF0000`**. (`A8R8G8B8=0xCF` also works for an opaque
buffer; write `A=0xFF`.)

---

### F09.8 Ampere `SET_STORAGE` / pitch — the subtle, measured part

[EVIDENCE] On C67E (Ampere) `SET_STORAGE` defines **only** `BLOCK_HEIGHT[3:0]` — there is **no
`MEMORY_LAYOUT` field** (clc67e.h:131-138). The **older** `clc37e.h` *does* have it (so S10.4's
`MEMORY_LAYOUT=PITCH/BLOCKLINEAR` came from the inherited C37E def, not C67E):
```123:125:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class/clc37e.h
#define NVC37E_SET_STORAGE_MEMORY_LAYOUT                                        4:4
#define NVC37E_SET_STORAGE_MEMORY_LAYOUT_BLOCKLINEAR                            (0x00000000)
#define NVC37E_SET_STORAGE_MEMORY_LAYOUT_PITCH                                  (0x00000001)
```
[EVIDENCE] and the layout bit is **gated off** for Ampere — `nvkms` only sets `MEMORY_LAYOUT` when
`caps.supportsSetStorageMemoryLayout`, which is **FALSE** for the Ampere HAL `nvEvoC6`
(nvkms-evo3.c:7871 struct; **:7936 = FALSE**, vs C3/C5 = TRUE):
```3792:3804:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia-modeset/src/nvkms-evo3.c
    storage = 0;
    if (pHwState->pSurfaceEvo[NVKMS_LEFT]->layout ==
        NvKmsSurfaceMemoryLayoutBlockLinear) {
        const NvU32 blockHeight = pHwState->pSurfaceEvo[NVKMS_LEFT]->log2GobsPerBlockY;
        storage |= DRF_NUM(C37E, _SET_STORAGE, _BLOCK_HEIGHT, blockHeight);
        if (pDevEvo->hal->caps.supportsSetStorageMemoryLayout) {
            storage |= DRF_DEF(C37E, _SET_STORAGE, _MEMORY_LAYOUT, _BLOCKLINEAR);
        }
    } else if (pDevEvo->hal->caps.supportsSetStorageMemoryLayout) {
        storage |= DRF_DEF(C37E, _SET_STORAGE, _MEMORY_LAYOUT, _PITCH);
    }
```
⇒ On GA102, `SET_STORAGE` carries **only `BLOCK_HEIGHT`** (measured `0x0 = ONE_GOB`). **Pitch-vs-
block-linear is NOT in the window method on Ampere** — it is conveyed by the scanout surface's
allocation ("kind"/PTE), see Open questions. `SET_PLANAR_STORAGE` units (either layout ⇒ 64 B
granule, pitch-linear ⇒ `pitch>>6`, with `nvAssert((pitch & 63)==0)`):
```3828:3838:/home/flare/dev/gpu-repro/open-gpu-kernel-modules/src/nvidia-modeset/src/nvkms-evo3.c
        pitch = pHwState->pSurfaceEvo[NVKMS_LEFT]->planes[planeIndex].pitch;
        if (pHwState->pSurfaceEvo[NVKMS_LEFT]->layout ==
            NvKmsSurfaceMemoryLayoutBlockLinear) {
            nvDmaSetEvoMethodData(pChannel,
                DRF_NUM(C37E, _SET_PLANAR_STORAGE, _PITCH, pitch));
        } else {
            nvAssert((pitch & 63) == 0);
            nvDmaSetEvoMethodData(pChannel,
                DRF_NUM(C37E, _SET_PLANAR_STORAGE, _PITCH, pitch >> 6));
        }
```
[INFERENCE] Measured `0x100`(256)·64 = `16384 B` = 4096 px × 4 B → the modetest surface row pitch
was **padded** from 2560 (10240 B). A tight 2560-wide `X8R8G8B8` pitch-linear fb needs
`10240>>6 = 160 (0xA0)` — what StelluxOS should use for the simplest CPU paint.

---

### F09.9 The minimal critical path (PCI → pixel)

Essential-only; each step cites the owning section. **"skip for v1"** = not needed for one static
pixel on this GA102 + this DP panel.

1. **PCI/BAR map.** Enable MMIO+bus-master; map BAR0 (16 MB regs), BAR1 (256 MB VRAM aperture). [S-PCI, §3]
2. **GSP boot.** FWSEC/FRTS → Booter → RISC-V → `INIT_DONE` (~2.07 s). [S-GSP, §4a]
3. **RPC handshake.** `SET_SYSTEM_INFO`→`SET_REGISTRY`→`GET_GSP_STATIC_INFO`. [S-RPC, §4b]
4. **Object tree.** `NV01_ROOT`→`DEVICE`→`SUBDEVICE`→`NV04_DISPLAY_COMMON`(on DEVICE)→`NVC670_DISPLAY`
   → **`NVC67D` core** + **one `NVC67E` window**. [S-tree, §4c; CORRECTIONS #4]
5. **Detect + EDID + pick head/SOR.** `GET_CONNECT_STATE`,`GET_EDID_V2`,`GET_PCLK_LIMIT`,
   `IS_MODE_POSSIBLE`; pick **head 3 / SOR 1 / displayId 0x800** (measured). [S-detect, §4d]
   - skip for v1: DP-MST, the 2nd monitor, link-rate optimization beyond what the mode needs.
6. **Allocate + bind framebuffer ctxdma** (VRAM surface, 1024 B aligned, pitch-linear, kind matched). [S-mem]
7. **Push CORE program** (F09.3) for head 3: PROCAMP, CONTROL, PIXEL_CLOCK_FREQUENCY/_CONFIGURATION/
   _MAX, DISPLAY_ID, RASTER_SIZE/_SYNC_END/_BLANK_END/_BLANK_START, VIEWPORT_SIZE_IN/_OUT+POINT_IN,
   `SOR_SET_CONTROL(owner=HEAD3, DP_B)`.
   - skip for v1: scaler coeff tables, OLUT/OCSC/OCSC1, DITHER, OUTPUT_RESOURCE, DSC/HDMI-FRL,
     MIN_FRAME_IDLE, HBLANK_DELAY(=0), TILE_POSITION, cursor, SW_SPARE VPLL_REF, all heads but one.
8. **CORE `UPDATE = 0x1`** — raster goes live (blank screen).
9. **Push WINDOW program** (F09.7): SET_CONTEXT_DMA_ISO[0], SET_OFFSET[0], SET_SIZE/_IN/_OUT,
   SET_STORAGE(`0`/ONE_GOB), SET_PLANAR_STORAGE[0](`pitch>>6`), SET_PARAMS(`0xE6`),
   SET_COMPOSITION_CONTROL(`BYPASS=ENABLE`), SET_PRESENT_CONTROL(`0`).
   - skip for v1: CSC/FMT coeffs, ILUT, input scaler, semaphores/notifier, timestamps, scan
     direction, multi-plane ISO[1..5]/PLANAR[1..2], windows 1–7, window-imm (NVC67B), cursor (NVC67A).
10. **WINDOW `UPDATE = 0x1`** — window fetches the surface → **pixels scan out**.
11. **Write pixels** into the bound VRAM surface (CPU via BAR1) → visible. (write once; skip
    page-flip/double-buffer/vblank for v1.)

[INFERENCE] Minimum distinct EVO methods ≈ **16 core methods + 10 window methods + the 2 `UPDATE`s
(one per channel) = 28 method writes** (the pseudocode in F09.10 emits 26, skipping the two
confirmed-zero core methods `VIEWPORT_POINT_OUT_ADJUST` and `RASTER_HBLANK_DELAY`), versus the ~4930
the production driver emitted (mostly scaler-coeff tables and per-head/per-window bounds).

---

### F09.10 Draw-a-red-pixel — StelluxOS pseudocode (C-like)

[INFERENCE] Synthesized from the measured CORE (F09.3) + WINDOW (F09.7) programs and the `nvkms`
formulas. Assumes channels allocated + pushbuffers registered (S08) and a VRAM surface allocatable
(S-mem). Handle/offset/displayId are the caller's own. `evo_method`/`evo_data`/`evo_kick` are S08's
`nvDmaSetStartEvoMethod`/`...Data`/`nvDmaKickoffEvo`.

```c
/* ---- chosen mode: 2560x1440 (measured active = 0x05a00a00) ---- */
typedef struct { u32 hVis,hSS,hSE,hTot, vVis,vSS,vSE,vTot, pclkKHz; } mode_t;
static const mode_t MODE_60  = {2560,2608,2640,2720, 1440,1443,1448,1481, 241500}; /* MEASURED   */
static const mode_t MODE_144 = {2560,2568,2600,2666, 1440,1465,1473,1543, 592000}; /* EDID/INFER */

#define X8R8G8B8     0xE6u          /* clc67e.h:147                                */
#define RED          0x00FF0000u    /* 0x00RRGGBB (F09.7 [INFERENCE] byte order)   */
#define WH16(w,h)    ( ((w)&0xffffu) | (((h)&0xffffu)<<16) )   /* window SET_SIZE* */
#define WH15(w,h)    ( ((w)&0x7fffu) | (((h)&0x7fffu)<<16) )   /* core 15-bit pack */
#define HOFF(base)   ((base) + (u32)head*0x400u)               /* C67D per-head    */
#define M(ch,off,val) do { evo_method((ch),(off),1); evo_data((ch),(val)); } while (0)

/* DRM->EVO raster (nvkms-evo.c:5405-5450): sync starts at 0, fields are 0-based ("-1") */
static void evo_raster(const mode_t *m, u32 *rs,u32 *se,u32 *be,u32 *bs) {
    u32 seX=(m->hSE-m->hSS)-1,            seY=(m->vSE-m->vSS)-1;
    u32 beX=(m->hTot-m->hSS)-1,           beY=(m->vTot-m->vSS)-1;
    u32 bsX=(m->hVis+(m->hTot-m->hSS))-1, bsY=(m->vVis+(m->vTot-m->vSS))-1;
    *rs=WH15(m->hTot,m->vTot); *se=WH15(seX,seY); *be=WH15(beX,beY); *bs=WH15(bsX,bsY);
    /* 60Hz -> {0x05c90aa0,0x0004001f,0x0025006f,0x05c50a6f} (== captured) */
    /* 144  -> {0x06070a6a,0x0007001f,0x004d0061,0x05ed0a61}              */
}

int first_pixel(evo_ch *core, evo_ch *win, int head /*=3*/, int orIndex /*=1*/,
                u32 displayId /*=0x800*/, gpu_ctxdma *fbDma, const mode_t *m) {
    u32 rs,se,be,bs; evo_raster(m,&rs,&se,&be,&bs);
    u32 pclkHz = m->pclkKHz * 1000;                       /* nvkms-evo3.c:1341      */

    /* ---------- CORE modeset (NVC67D) : F09.3 ---------- */
    M(core, HOFF(0x2000), 0x0);                           /* PROCAMP   COLOR_SPACE=RGB        */
    M(core, HOFF(0x2008), 0x0);                           /* CONTROL   STRUCTURE=PROGRESSIVE  */
    M(core, HOFF(0x200C), pclkHz & 0x7fffffffu);          /* PIXEL_CLOCK_FREQUENCY HERTZ, ADJ=0*/
    M(core, HOFF(0x201C), 0x0);                           /* PIXEL_CLOCK_CONFIGURATION (driver)*/
    M(core, HOFF(0x2028), pclkHz & 0x7fffffffu);          /* PIXEL_CLOCK_FREQUENCY_MAX         */
    M(core, HOFF(0x2020), displayId);                     /* HEAD_SET_DISPLAY_ID(h,0)=0x800    */
    M(core, HOFF(0x2064), rs);                            /* RASTER_SIZE   = 0x05c90aa0        */
    M(core, HOFF(0x2068), se);                            /* RASTER_SYNC_END  = 0x0004001f     */
    M(core, HOFF(0x206C), be);                            /* RASTER_BLANK_END = 0x0025006f     */
    M(core, HOFF(0x2070), bs);                            /* RASTER_BLANK_START = 0x05c50a6f   */
    M(core, HOFF(0x204C), WH15(m->hVis,m->vVis));         /* VIEWPORT_SIZE_IN  = 0x05a00a00    */
    M(core, HOFF(0x2058), WH15(m->hVis,m->vVis));         /* VIEWPORT_SIZE_OUT = 0x05a00a00    */
    M(core, HOFF(0x2048), 0x0);                           /* VIEWPORT_POINT_IN (0,0)           */
    M(core, 0x0300 + (u32)orIndex*0x20,                   /* SOR_SET_CONTROL(or=1)=0x00000908  */
            (1u<<head) | (0x9u<<8) /*DP_B*/ | (0u<<16) /*POSITIVE_TRUE*/);
    M(core, 0x0200, 0x1);                                 /* UPDATE  RELEASE_ELV=1 (NOT MODE_SWITCH) */
    evo_kick(core);  evo_wait_notifier(core);             /* raster live; screen blanks        */

    /* ---------- allocate + paint the framebuffer RED ---------- */
    u32 pitch = ALIGN_UP(m->hVis * 4u, 64u);              /* 10240 (assert pitch&63==0)        */
    u64 fbSize = (u64)pitch * m->vVis;                    /* ~14.06 MiB                        */
    u64 fbVram = vram_alloc(fbSize, /*align*/1024);       /* NV_SURFACE_OFFSET_ALIGNMENT_SHIFT=10*/
    volatile u8 *fb = bar1_map(fbVram, fbSize);
    for (u32 y=0; y<m->vVis; y++) {                       /* whole screen red (or a 64x64 sq)  */
        volatile u32 *row = (volatile u32 *)(fb + (u64)y*pitch);
        for (u32 x=0; x<m->hVis; x++) row[x] = RED;
    }
    bar1_flush();
    u32 hFbIso       = fbDma->handle;                     /* our ctxdma (capture=modetest 0x10087) */
    u64 fbByteOffset = fbVram - fbDma->base;              /* 0 if surface == dma base          */

    /* ---------- WINDOW scanout (NVC67E) : F09.7 ---------- */
    M(win, 0x0308, 0x0);                                  /* SET_PRESENT_CONTROL NON_TEARING   */
    M(win, 0x0240, hFbIso);                               /* SET_CONTEXT_DMA_ISO[0] (bind fb)  */
    M(win, 0x0260, (u32)(fbByteOffset >> 8));             /* SET_OFFSET[0]  (>>8)              */
    M(win, 0x0224, WH16(m->hVis,m->vVis));                /* SET_SIZE     = 0x05a00a00         */
    M(win, 0x0298, WH16(m->hVis,m->vVis));                /* SET_SIZE_IN  = 0x05a00a00         */
    M(win, 0x02A4, WH16(m->hVis,m->vVis));                /* SET_SIZE_OUT = 0x05a00a00 (no scale)*/
    M(win, 0x0228, 0x0);                                  /* SET_STORAGE  BLOCK_HEIGHT=ONE_GOB */
    M(win, 0x0230, pitch >> 6);                           /* SET_PLANAR_STORAGE[0]=0xA0 (tight)*/
    M(win, 0x022C, X8R8G8B8);                             /* SET_PARAMS FORMAT=0xE6            */
    M(win, 0x02EC, 0x00010000);                           /* SET_COMPOSITION_CONTROL BYPASS=EN */
    M(win, 0x0200, 0x1);                                  /* UPDATE RELEASE_ELV=1 -> scanout   */
    evo_kick(win);   evo_wait_notifier(win);              /* window fetches surface -> pixels  */
    return 0;
}
```
Note: EVO methods are **not** RPCs — each is a `(header,data)` dword pair written into the channel's
DMA pushbuffer (BAR1/sysmem) and advanced via `PUT` (S08; nvkms-dma.h). GSP-RM owns channel setup and
synthesizes the VPLL on the core `UPDATE`, but not each method. The two `UPDATE`s are the only sync
points for a static pixel; the measured production path additionally **window-interlocks** them
(F09.3) — optional for v1.

---

### Minimal-path notes (essential vs skippable for first pixel on GA102)

- **Essential CORE (one head) + MEASURED v1 values:** `PROCAMP=0`(RGB), `CONTROL=0`(PROGRESSIVE),
  `PIXEL_CLOCK_FREQUENCY=pclkHz`, `PIXEL_CLOCK_CONFIGURATION=0`, `PIXEL_CLOCK_FREQUENCY_MAX=pclkHz`,
  `DISPLAY_ID=0x800`, the four raster dwords (`0x05c90aa0/0x0004001f/0x0025006f/0x05c50a6f`),
  `VIEWPORT_SIZE_IN=SIZE_OUT=0x05a00a00`, `VIEWPORT_POINT_IN=0`, `SOR_SET_CONTROL=(1<<head)|(DP_B<<8)`,
  `UPDATE=0x1`.
- **Essential WINDOW (measured):** `SET_CONTEXT_DMA_ISO[0]`, `SET_OFFSET[0]`, `SET_SIZE/_IN/_OUT`,
  `SET_STORAGE=0`, `SET_PLANAR_STORAGE[0]=pitch>>6`, `SET_PARAMS=0xE6`, `SET_PRESENT_CONTROL=0`,
  `SET_COMPOSITION_CONTROL=0x00010000`, `UPDATE=0x1`.
- **Confirmed-zero (write 0 or skip):** core `VIEWPORT_POINT_OUT_ADJUST`, `RASTER_HBLANK_DELAY`,
  `PIXEL_CLOCK_CONFIGURATION`, `SET_CONTROL(0x210)`; window `SET_POINT_IN`, semaphores/notifier/timestamps.
- **Skip for v1:** scaler coeff tables, OLUT/OCSC/OCSC1/CSC/FMT, DITHER (head3=`0x211`), MIN_FRAME_IDLE,
  OVERSCAN, TILE_POSITION, DSC/HDMI-FRL, HEAD/WINDOW_USAGE_BOUNDS, cursor (NVC67A), window-imm (NVC67B),
  windows 1–7, multi-plane ISO[1..5]/PLANAR[1..2], the second head, and window↔core interlock (do
  sequential UPDATEs).
- **Ampere specifics:** `SET_STORAGE` has **no** `MEMORY_LAYOUT` bit — program only `BLOCK_HEIGHT`;
  convey pitch-vs-blocklinear via the **surface allocation kind**. Use a tight pitch-linear surface
  (`PLANAR_STORAGE=pitch>>6=0xA0`) for the simplest CPU-via-BAR1 paint (no GOB swizzle).
- **144 Hz:** change **only** `PIXEL_CLOCK_FREQUENCY(+_MAX)=0x23493400` and the four raster dwords
  (`0x06070a6a/0x0007001f/0x004d0061/0x05ed0a61`); the window program is unchanged. Provision the DP
  link for ≥592 MHz (HBR2+) and pass `GET_PCLK_LIMIT`/`IS_MODE_POSSIBLE`.

---

### Evidence cited

Headers (verified exact, offsets + bitfields):
- `src/common/sdk/nvidia/inc/class/clc67d.h:82-93` (UPDATE RELEASE_ELV/SPECIAL_HANDLING), `:301-327`
  (SOR_SET_CONTROL OWNER_MASK HEAD2=4/HEAD3=8 / PROTOCOL TMDS_A=1/DP_A=8/DP_B=9 / DE_SYNC_POLARITY /
  PIXEL_REPLICATE_MODE), `:353-356` (WINDOW_SET_CONTROL base 0x1000, OWNER_HEAD__SIZE_1=8 — collision
  proof), `:491-496` (PROCAMP COLOR_SPACE_RGB=0), `:567-569` (HEAD_SET_CONTROL STRUCTURE/PROGRESSIVE),
  `:693-697` (PIXEL_CLOCK_FREQUENCY HERTZ 30:0 / ADJ1000DIV1001 31), `:727-728`
  (PIXEL_CLOCK_CONFIGURATION NOT_DRIVER), `:737-738` (DISPLAY_ID CODE 31:0), `:739-740`
  (PIXEL_CLOCK_FREQUENCY_MAX HERTZ), `:816-827` (VIEWPORT POINT_IN/SIZE_IN/SIZE_OUT/POINT_OUT_ADJUST),
  `:828-839` (RASTER_SIZE/SYNC_END/BLANK_END/BLANK_START WIDTH14:0/HEIGHT30:16), `:1323-1325`
  (RASTER_HBLANK_DELAY).
- `src/common/sdk/nvidia/inc/class/clc67e.h:55-56` (UPDATE/RELEASE_ELV), `:128-132` (SET_SIZE
  W15:0/H31:16, SET_STORAGE BLOCK_HEIGHT 3:0 — **no MEMORY_LAYOUT**), `:139-148` (SET_PARAMS FORMAT
  X8R8G8B8=0xE6/A8R8G8B8=0xCF/A8B8G8R8=0xD5), `:177-184` (SET_PLANAR_STORAGE PITCH 12:0 / ISO HANDLE /
  OFFSET ORIGIN — collision proof PLANAR(4)=ISO(0), PLANAR(12)=OFFSET(0)), `:185-191` (SET_POINT_IN,
  SET_SIZE_IN 0x298, SET_SIZE_OUT 0x2A4), `:204-212` (SET_COMPOSITION_CONTROL COLOR_KEY_SELECT 1:0 /
  DEPTH 11:4 / BYPASS 16:16 ENABLE=1), `:281-284` (SET_PRESENT_CONTROL MIN_PRESENT_INTERVAL 3:0 /
  BEGIN_MODE 6:4 NON_TEARING=0).
- `src/common/sdk/nvidia/inc/class/clc37e.h:115-125` (inherited SET_STORAGE incl. MEMORY_LAYOUT 4:4 —
  the field GA102/C67E omits).
- `src/common/sdk/nvidia/inc/class/clc37dswspare.h:28-30` (SW_SPARE_A_CODE_VPLL_REF NO_PREF/QSYNC).
- `src/common/sdk/nvidia/inc/ctrl/ctrlc372/ctrlc372chnc.h:376-379` (dispClkKHz, dGPU fixed-frequency doc).

Measured capture (channel-attributed; CORRECTIONS #5 offset re-map applied):
- `analysis/20260530-115116-open-capture-evo-full.txt:4674-4677` (head3 RASTER quad), `:4679` (head3
  base PIXEL_CLOCK_FREQUENCY off 0x2c0c=241.5 MHz — mislabeled, re-mapped), `:4680` (PCLK_CONFIG=0),
  `:4681` (head3 _MAX=241.5 MHz), `:4685` (HBLANK_DELAY=0), `:4686` (HEAD_SET_CONTROL=0), `:4694`/`:4721`
  (PROCAMP=0), `:4710` (SOR1=0x908 DP_B/HEAD3), `:4711` (DISPLAY_ID=0x800), `:4712`/`:4714` (VIEWPORT
  SIZE_IN/OUT=0x05a00a00), `:4713` (POINT_OUT_ADJUST=0), `:4719` (VIEWPORT_POINT_IN=0); head2 deltas
  `:4743-4746`,`:4748` (off 0x280c=241.7 MHz),`:4750`,`:4779` (SOR0=0x104 TMDS_A),`:4780`
  (DISPLAY_ID=0x2000); window `:4822` (PRESENT_CONTROL=0), `:4823` (ISO[0]=0x10087 off 0x0240), `:4824`
  (OFFSET[0]=0 off 0x0260), `:4835-4837` (SIZE/_IN/_OUT=0x05a00a00), `:4838` (STORAGE=0), `:4839`
  (PLANAR_STORAGE[0]=0x100), `:4844` (PARAMS=0xe6), `:4866` (COMPOSITION_CONTROL=0x10000 BYPASS),
  `:4905`,`:4917`,`:4926` (ch=0x20 mirror), `:4876`,`:4958` (ch=0x40/0x100 ISO[0]=0 inactive); commit
  `:4972-4974` (core interlock + UPDATE=1), `:4975-4986` (window interlock + UPDATEs=1), `:4653`
  (pre-commit UPDATE=1).
- `traces/20260530-112551-open-capture/modetest-list.txt:74` (60Hz 2720/1481/241500), `:75` (144Hz
  2666/1543/592000).
- `traces/20260530-115116-open-capture/modetest-list.txt:71` (DP-2 connected), `:74-75`, `:152`
  (HDMI-A-2 60.00/241700 = head 2).
- `traces/20260530-115116-open-capture/modetest-set-144.txt:1` (144 set failed on connector 88 ⇒ 144
  dwords are EDID-derived, CORRECTIONS #6).

Open-source logic (verified):
- `src/nvidia-modeset/src/nvkms-evo.c:5395` (pixelClock=HzToKHz, kHz), `:5405-5450` (DRM→EVO raster
  derivation, 0-based −1 rule).
- `src/nvidia-modeset/src/nvkms-evo3.c:1299-1317` (raster dword packers), `:1338-1354`
  (PIXEL_CLOCK_FREQUENCY/_CONFIGURATION/_MAX = pclk*1000), `:2109-2143` (EvoSetHeadRefClkC3 VPLL_REF
  SW_SPARE_A hint), `:2188-2193` (SOR_SET_CONTROL value), `:3274-3292` (IS_MODE_POSSIBLE +
  minRequiredBandwidthKBPS/floorBandwidthKBPS), `:3766-3771` (ISO+OFFSET per plane), `:3774-3787`
  (SET_SIZE/_IN/_OUT), `:3792-3804` (SET_STORAGE: BLOCK_HEIGHT + caps-gated MEMORY_LAYOUT),
  `:3828-3838` (PLANAR_STORAGE pitch units, pitch&63 assert), `:7871` (nvEvoC6 HAL), `:7936`
  (Ampere supportsSetStorageMemoryLayout=FALSE; cf. C3 `:7778`/C5 `:7857`=TRUE).
- `src/nvidia-modeset/src/nvkms-dpy.c:603-634` (GET_PCLK_LIMIT → maxPixelClockKHz=orPclkLimit),
  `:2579-2580` (reject pixelClock > maxSingleLinkPixelClockKHz).
- `src/nvidia-modeset/include/nvkms-utils.h:145-151` (nvCtxDmaOffsetFromBytes = >>8),
  `src/nvidia-modeset/include/nvkms-types.h:100` (NV_SURFACE_OFFSET_ALIGNMENT_SHIFT=10),
  `:2998-3001` (supportsSetStorageMemoryLayout cap bit).
- `decode_evo_full.py:28-31` (offset→name first-match-<64 — source of the mislabels).

Context: CONTEXT_BRIEF.md §3/§4a-§4e/§6/§8; CONTEXT_ADDENDUM.md (measured 60 Hz table; 144 Hz
EDID-derived caveat; base-pclk EVIDENCE-GAP — **resolved here as a decoder name-column artifact**);
CORRECTIONS.md #1 (two monitors / namespaces), #4 (DISPLAY_COMMON on DEVICE), #5 (offset re-map), #6
(144 Hz EDID-derived), #9 (no MEMORY_LAYOUT on Ampere), #10 (host requests HERTZ, GSP-RM programs VPLL).
Source drafts merged: S10, D06, D07, D10.

### Open questions / TODO

- [TODO] Capture a real 144 Hz modeset **with data words** to confirm `RASTER_SIZE=0x06070a6a` and
  `PIXEL_CLOCK_FREQUENCY=0x23493400` on the wire (F09.4 is computed; the `2560x1440-144` set failed on
  connector 88). Add a `nvDmaSetEvoMethodData` trace point.
- [UNCERTAIN] **How does C67E HW learn pitch vs block-linear on Ampere**, given `SET_STORAGE` omits
  `MEMORY_LAYOUT` (F09.8)? Resolve via the scanout surface PTE/ctxdma **kind** and IMP/usage-bounds
  (`nvKmsGetSurfaceMemoryFormatInfo`, nvkms-evo3.c:3806; S-mem). A StelluxOS pitch-linear surface must
  be allocated with the matching kind or scanout mis-tiles. Also confirm a from-scratch
  `PLANAR_STORAGE=0xA0` (tight) scans clean (capture used 0x100/16384 B padded).
- [TODO] `SET_CONTEXT_DMA_ISO[0]=0x10087` is modetest's RM handle; StelluxOS must allocate its own
  ctxdma and register the **window** pushbuffer (`NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER`,
  S08) before this program runs.
- [TODO] Confirm the literal `GET_PCLK_LIMIT.orPclkLimit` and the `IS_MODE_POSSIBLE` reply
  (`dispClkKHz`, `minRequiredBandwidthKBPS`, `floorBandwidthKBPS`) for both modes — present in
  `rpc-resp-trace.txt` (RVGRESP, 32-dword cap) but not yet decoded; verify 592,000 kHz ≤ limit.
- [TODO] Reconcile the DRM connector id across captures (brief §3 "88"; `112551` shows 93 DP-2 + 96
  HDMI-A-2 connected; `115116` modetest targeted 88 and failed). The RM-side identity (displayId
  `0x800`, head 3, SOR 1, DP_B) is measured and unambiguous; do not overwrite §3 — flag for S-detect.
- [UNCERTAIN] Capture shows a pre-commit core `UPDATE=0x1` (evo-full:4653) before raster programming,
  then the committing `UPDATE=0x1` (:4974) — a setup/commit pair. For v1 a single core UPDATE after the
  SETs is expected to suffice; verify no mandatory pre-modeset blank update is required on C67D
  (cross-ref `SET_GET_BLANKING_CTRL`, clc67d.h:293).
- [INFERENCE→verify] `X8R8G8B8` byte order (`0x00RRGGBB`) and `SET_COMPOSITION_CONTROL` BYPASS=ENABLE
  as the single-opaque-layer path — confirm against a real painted frame (red vs blue swap would
  indicate B/R order).
- [TODO] VRR/G-SYNC is **out of scope** for first pixel: the DP panel is `vrr_capable`
  (modetest-list.txt:145) but the measured `RASTER_SIZE.y=1481` shows **no** G-SYNC `+2` back-porch
  (nvkms-vrr.c:274-304), i.e. VRR was not engaged — keep `allowVrr=false`.


## F10 — The Minimal First-Pixel Critical Path & StelluxOS Module Layout

> **Authoritative final synthesis.** This section replaces draft D09. It does **not**
> re-derive register sequences, struct layouts, or method values — those live in the
> finalized sections **F01–F09** (this is **F10**). For historical reasons the per-step
> tags and tables below cite them by their **draft ids S01–S10**; resolve every S-id via
> the **draft→final map in F10.0** (note **S04+S05 merged into F04**, so S06→F05 … S10→F09).
> The verbatim draft subsections (§S0x.y) also remain on disk at `spec-drafts/S0x-*.md`.
> F10's job is to
> (a) give the verified, de-duplicated PCI→pixel checklist (each step: owning section,
> ESSENTIAL tag, single load-bearing primary cite, success check), (b) state exactly
> what NOT to build for v1, (c) map StelluxOS modules → sections → firmware blobs, and
> (d) rank the load-bearing risks + de-risks.
>
> **Verification status.** Every step below was checked against the section it claims to
> own, and the most load-bearing cites + every applied CORRECTION were re-verified
> against the *primary* source tree / traces (not just the drafts). See F10.0 for what
> changed vs D09.
>
> **Path conventions** (CONTEXT_BRIEF §5): `SRC=open-gpu-kernel-modules`,
> `CAP=traces/20260530-112551-open-capture` (boot + driver **load-time** modeset on the real
> heads; ≈1145 RPCs — per CORRECTIONS #1 both `modetest -s` runs FAILED, so this is **not** a
> modetest 144 Hz capture; the EVO methods are still valid load-time data),
> `CAP2=traces/20260530-115116-open-capture` (gap-fill: EVO data words + RPC replies),
> `AN=analysis`. Capture `110235`/`110540` = the **minimal bring-up** (663 RPCs).
>
> **Label legend** (CONTEXT_BRIEF §6): **[EVIDENCE]** = backed by a cited primary
> file:line or a section that cites it; **[INFERENCE]** = reasoned from cited evidence
> (says from what); **[TODO]** = unconfirmed (says what to check). Verified facts in
> CONTEXT_BRIEF §3/§4 are not contradicted; §3 discrepancies the captures expose are
> flagged in "Open questions / TODO", not overwritten.

---

### F10.0 — Reconciliation summary (verified / corrected vs D09)

**Cross-reference resolution — draft `S0x` → final `F0x` (READ FIRST).** Every "owning
section" tag in F10.1 and every cite in F10.2/F10.3/F10.4 names a **draft id (S01–S10)**.
The finalized, merged sections are the `spec-final/F01–F09` set. **S04 and S05 were merged
into F04**, which shifts all later ids by one. Resolve through this table:

| cited draft id | final section | topic |
|---|---|---|
| S01 | **F01** | context / scope / glossary |
| S02 | **F02** | OS prerequisites (PCI/BAR/DMA/timers) |
| S03 | **F03** | firmware blobs + FB/WPR layout |
| **S04 + S05** | **F04** | GSP/SEC2 boot (FWSEC-FRTS = §F04.5, SEC2 Booter = §F04.6, sequencer = §F04.8) |
| S06 | **F05** | RPC transport |
| S07 | **F06** | RM object model + bring-up RPCs |
| S08 | **F07** | display objects & channels |
| S09 | **F08** | detection / EDID / DP / IMP |
| S10 | **F09** | modeset / scanout / pixel |
| D09 | **F10** | this synthesis |

Subsection numbers differ where sections were merged/renumbered (S06→F05 uses new `§N`
headings; S09/S10→F08/F09 use new `§F0x.y` headings); consult the mapped F-section's own
headings, or read the verbatim draft subsection at `spec-drafts/S0x-*.md`. (D0x deep-dive
cites — D02/D03/D06/D07/D08/D10 — resolve to `spec-drafts/D0x-*.md`.)

The 30-step checklist and skip-list were sound on **structure and ordering** (all steps
map to a real section + a real primary cite; the dependency order matches the RVGBOOT
01→10 spine and the post-INIT_DONE control plane). Verified spot-checks against primary
sources (all exact): `pci_set_master` `nv-pci.c:653`; doorbell `NV_PGSP_QUEUE_HEAD(i)=
0x110c00+i*8` `dev_gsp.h:38`; `NVC372_CTRL_CMD_IS_MODE_POSSIBLE=0xc3720101`
`ctrlc372chnc.h:39`; `NVC67E_SET_PARAMS_FORMAT_X8R8G8B8=0xE6` `clc67e.h:147`; boot stage
markers `boot-trace.txt:727/1445/1778`.

**Corrections applied** (each evidence-backed; these *fix dependency/value misstatements*
in D09's checklist):

| # | Where | D09 said | FINAL (corrected) | Primary evidence |
|---|---|---|---|---|
| C1 | Step 28 SOR | `PROTOCOL=DP_A` | **`PROTOCOL=DP_B (0x9)`**, SOR **index 1**, for the live DP panel `displayId 0x800` → head 3 | measured `SOR_SET_CONTROL(1)=0x00000908` `AN/…115116…-evo-full.txt:4710`; `clc67d.h:318` (DP_B=9) [D06; CORRECTIONS #1] |
| C2 | Step 29 SET_STORAGE | `SET_STORAGE(PITCH)` (MEMORY_LAYOUT field) | **C67E `SET_STORAGE` has only `BLOCK_HEIGHT` (3:0); NO `MEMORY_LAYOUT` field** — pitch-vs-blocklinear is set by the surface alloc *kind*, not this method | `clc67e.h:131-138` (verified: only `BLOCK_HEIGHT`) [D07; CORRECTIONS #9] |
| C3 | Steps 27/29 pitch | computed `PLANAR_STORAGE=160` (10240 B) | keep formula `pitch>>6`, but **measured was `0x100`=256 → 16384 B stride** (driver padded); implementer supplies own surface, 64 B-aligned | `SET_PLANAR_STORAGE[0]=0x100` `…115116…-evo-full.txt`(evo-data:4839) [D07; CORRECTIONS #5] |
| C4 | Step 18 / RPC framing | (S06 §6) `header_version=0x00030000` | **`header_version=0x03000000`** (MAJOR in bits 31:24); `signature=0x43505256` | all 1143 replies `w[0]=0x03000000` [D02:344-352; CORRECTIONS #3] |
| C5 | RPC headline count | "663 RPCs" unqualified | **663** (minimal bring-up, `110235`) vs **≈1145** (1143 sync + 2 async; full modeset, `112551`) — always say which | counts re-verified in both captures [CORRECTIONS #2] |
| C6 | Step 28 144 Hz dwords | implied captured | **144 Hz dwords are EDID-derived [INFERENCE]** (`RASTER_SIZE=0x06070a6a`, `PIXEL_CLOCK_FREQUENCY=0x23493400`); only **60 Hz** dwords are on-wire [EVIDENCE] (`RASTER_SIZE=0x05c90aa0`, pclk 241.5 MHz) | `…evo-full.txt:4674`; `modetest-list.txt:74-75` [D06/D10; CORRECTIONS #6] |
| C7 | Step 22 handles | "hInternalClient/hInternalSubdevice" | values are **`hInternalClient=0xc2000005`, `hInternalSubdevice=0xabcd2080`** (read from fn=65, do not hardcode) | `GspStaticConfigInfo` `gsp_static_config.h:136-142` [D03; CORRECTIONS #7] |
| C8 | Steps 26/28 pclk | (clarify) | **host sets `HEAD_SET_PIXEL_CLOCK_FREQUENCY.HERTZ=kHz×1000`; no host PLL MMIO — GSP-RM programs the VPLL**; `GET_PCLK_LIMIT`+`IS_MODE_POSSIBLE` gate it | `nvkms-evo3.c:1338-1342` [D10; CORRECTIONS #10] |
| C9 | Step 30 paint | (D09 already fixed S10's magenta) | confirmed: `X8R8G8B8` red = **`0x00FF0000`** (S10 pseudocode's `0x00FF00FF` is magenta) | `clc67e.h:147` format; S10 §S10.5 |
| C10 | Whole doc | "the monitor" | **TWO monitors**: DP `0x800`→head3→SOR1→**DP_B**→AOC AG241QG4 (the **first-pixel target**); HDMI `0x2000`→head2→SOR0→SINGLE_TMDS_A | [D06/D10; CORRECTIONS #1] |
| C11 | Step 28 core `UPDATE` | `SPECIAL_HANDLING=MODE_SWITCH` (⇒ `0x200001`) | **`UPDATE=0x1`** (`RELEASE_ELV=TRUE`, `SPECIAL_HANDLING=NONE`) — D09/this checklist carried the stale header-only guess; the **measured** value is `0x1` | [MEASURED] `…evo-full.txt:4974`; `clc67d.h:82-93`; **F09 §F09.0 row A / §F09.10** |
| C12 | Step 29 window methods | omitted `SET_PRESENT_CONTROL` + `SET_COMPOSITION_CONTROL` | **add `SET_PRESENT_CONTROL=0` and `SET_COMPOSITION_CONTROL=0x00010000` (`BYPASS=ENABLE`)** to the essential window program (F09 lists both as essential) | [MEASURED] `…evo-full.txt:4866`; `clc67e.h:204-212`; **F09 §F09.7/§F09.9** ([INFERENCE→verify] BYPASS strictly required vs default 0) |

**Minor cite-attribution fixes** (not dependency errors): step 4's GFW `2.05 s` constant
is owned by **S04 §S04.7** (`kern_gpu_tu102.c:372-375`), with the timer *primitive* owned
by S02 §6; step 11's success is "reset writes `boot-trace.txt:5-11` present → progression
to stage 05" (the stage-04 marker prints *before* those writes); step 20's
DISPLAY_COMMON-parent correction is **CORRECTIONS #4** (= ADDENDUM #3).

**Nothing was removed** — all 30 steps are genuinely on the minimal path. Step 26
(`IS_MODE_POSSIBLE`) is the only one tagged **ESSENTIAL-but-bypassable** (see R5).

---

### F10.1 — The authoritative PCI → lit pixel checklist

Numbered continuously, dependency-ordered, de-duplicated across S02–S10. Every step is
on the minimal path. Each line gives: **owning section** · **ESSENTIAL tag** · **load-
bearing primary cite** (the thing an implementer types) · **success check**. The 10 GSP
boot stages (RVGBOOT 01–10) are the spine of steps 5–18 (`[EVIDENCE: S04 §S04.1]`).

#### Phase 0 — Host/OS substrate (no GPU traffic yet)

1. **PCI enumerate + set PCI_COMMAND Memory-Space + Bus-Master** on `10de:2216 @
   0000:0b:00.0`. · **[S02 §1] ESSENTIAL** · `pci_set_master` `SRC/kernel-open/nvidia/
   nv-pci.c:653` (BME so GSP can DMA sysmem rings/blobs). · **Success:** config dword
   `0x221610DE` / subsys `0x403F1458` (`CAP/nvidia-smi-q.txt:56-71`). *MSI/MSI-X NOT
   enabled — poll (F10.2).*
2. **ioremap BAR0 (16 MB, uncached) + volatile RD32/WR32; verify chip alive.** · **[S02
   §2] ESSENTIAL** · `osMapKernelSpace(…NV_MEMORY_UNCACHED…)` `SRC/…/osapi.c:3301-3304`.
   · **Success:** `NV_PMC_BOOT_0/42` read ≠ `0xFFFFFFFF` (GPU-lost probe `nv.c:4649`).
3. **DMA layer: coherent {cpu_va, dma_addr} alloc, 47-bit address space, store/full
   fence + WC-flush, phys/DMA-addr query.** · **[S02 §4] ESSENTIAL** · width 47 =
   `NV_GSP_GPU_MIN_SUPPORTED_DMA_ADDR_WIDTH` `SRC/…/osinit.c:158`; store fence (WAW)
   `message_queue_cpu.c:596-606`. · **Success:** every alloc validates `< 2^47`
   (`nv-dma.c:45-56`).
4. **Monotonic ns clock + busy/sleep delay** (boot polls run multi-second). · **[S02 §6]
   ESSENTIAL** (timer primitive); sizing constant from **S04 §S04.7** GFW timeout 2.05 s
   `kern_gpu_tu102.c:372-375`. · **Success:** a 2 s poll neither early-aborts nor spins
   forever (the `RmInitAdapter failed` class — CONTEXT_BRIEF §2; never trap/serialize
   MMIO).

#### Phase 1 — GSP cold boot to INIT_DONE (RVGBOOT 02→10)

5. **Extract VBIOS from PROM + parse FWSEC** (BIT → BIOSDATA version → FALCON_DATA →
   FWSEC V3/HS ucode). · **[S03 §3] ESSENTIAL** · `kgspParseFwsecUcodeFromVbiosImg_IMPL`
   `kernel_gsp_fwsec.c:1080`. · **Success:** parsed version prints `94.02.71.40.C4`
   (`CAP/boot-trace.txt:2`) = stage 02.
6. **Poll `GFW_BOOT.PROGRESS == COMPLETED`** (VBIOS devinit done; FB size now queryable).
   · **[S04 §S04.7] ESSENTIAL** · field `kernel_gsp_frts_tu102.c:497-503`. · **Success:**
   stage 03 marker (`CAP/boot-trace.txt:3`); `kmemsysGetUsableFbSize` returns a real
   `fbSize`.
7. **Load `gsp_ga10x.bin`, gate `.fwversion == 535.183.01`, take `.fwimage`, build
   radix3, copy `.fwsignature_ga10x` to a 256 B-aligned memdesc.** · **[S03 §2.1,§6]
   ESSENTIAL** · version gate `kernel_gsp.c:3306-3354`; radix3 `kernel_gsp.c:3456`. ·
   **Success:** version match (else `NV_ERR_INVALID_DATA`); `radix3[0].nPages==1`
   (`:3513`).
8. **Resolve bindata ucodes: Booter Load (SEC2 HS) + GSP-RM boot binary (SK+BL) and its
   `RM_RISCV_UCODE_DESC`.** · **[S03 §2.2,§2.4] ESSENTIAL** ·
   `kgspAllocateBooterLoadUcodeImage` `kernel_gsp_booter.c:434`; boot-bin
   `kernel_gsp.c:3145`. · **Success:** ucode images + `monitorCode/Data/manifestOffset`
   parsed; `bPartitionedFmc==FALSE` (separate Booter, GA102).
9. **Compute FB layout top-down; fully populate `GspFwWprMeta` (256 B); alloc libos
   init-args (UNCACHED) + GSP-args-cached (CACHED).** · **[S03 §5,§7] ESSENTIAL** ·
   `kgspCalculateFbLayout_TU102` `kernel_gsp_tu102.c:509-666`; `magic=0xdc3aae21371a60b3`
   `:636-639`. · **Success:** offsets aligned per S03 §5.2; WPR stays in the pre-scrubbed
   top ~256 MB (no scrubber on GA102, [S03 §5.4]).
10. **Build the RPC message-queue collection (one RM cmd + status pair) and place its
    `sharedMemPA` + queue offsets into GSP-args `messageQueueInitArguments` before
    bootstrap.** · **[S06 §2; S03 §7.2] ESSENTIAL** · `sharedMemPA = pPageTbl[0]`
    `message_queue_cpu.c:338`. · **Success:** [INFERENCE] GSP later links the status ring
    (step 16); failure surfaces there. *(Couples S06 transport to S03 boot-args — risk R4;
    exact carrier field is a TODO.)*
11. **GSP Falcon reset preamble + config** (`FALCON_ENGINE.RESET` 1→0, `BCR_CTRL`
    core-select FALCON, `FALCON_RM=chipId0`). · **[S04 §S04.4 / S05 §S05.4] ESSENTIAL** ·
    `boot-trace.txt:5-11`. · **Success:** reset writes `:5-11` present, then progression
    to stage 05 (the stage-04 marker `:4` prints *before* these writes).
12. **FWSEC-FRTS on the GSP Falcon → carve WPR2** via the Falcon DMA-load-and-go mechanic
    (`FBIF_TRANSCFG/CTL`, `DMACTL`, `DMATRFBASE`, 256 B `MOFFS/FBOFFS/CMD` loop with
    `0x614` IMEM / `0x600` DMEM, BROM PKC regs, `BOOTVEC`, `CPUCTL.STARTCPU`, wait-halt);
    DMEM-mapper `init_cmd=0x15`, `FWSECLIC_FRTS_CMD{offset4K, size 0x100, media FB(2)}`. ·
    **[S05 §S05.2; S04 §S04.3] ESSENTIAL** · verify `kernel_gsp_frts_tu102.c:454-482`. ·
    **Success:** `NV_PFB_PRI_MMU_WPR2_ADDR_HI != 0`, `_ADDR_LO == frtsOffset>>align`,
    scratch `0x0E`==0 ⇒ stage 05 (`boot-trace.txt:727`).
13. **Reset-into-RISC-V** (`BCR_CTRL=0x111` = RISCV|VALID|BRFETCH); program
    `QUEUE_HEAD(0)=0` and GSP `MAILBOX0/1 = phys(libos init-args)`. · **[S04 §S04.5]
    ESSENTIAL** · `boot-trace.txt:728-735`. · **Success:** stage 06 (`:731`).
14. **Stuff the two async openers `GSP_SET_SYSTEM_INFO(72)` + `SET_REGISTRY(73)` into the
    cmd ring (+ doorbell), before INIT_DONE.** · **[S07 §S07.1; S06 §12] ESSENTIAL** ·
    `rpc.c:1368, 1374-1378`. · **Success:** `CAP/rpc-trace.txt:1-2` (`send async fn=72/73`
    at t≈3077.728, i.e. *between* stage 06 `:731` and stage 07 `:1445`).
15. **Booter Load on SEC2** (`MAILBOX0/1 = phys(GspFwWprMeta)`; same DMA-load-and-go on
    base `0x840000`; BROM `CURR_UCODE_ID=3`, `MOD_SEL=RSA3K`; `STARTCPU`; wait-halt;
    require `MAILBOX0==0`). Then `FALCON_OS=appVersion`; read `RISCV_CPUCTL.ACTIVE_STAT`. ·
    **[S05 §S05.3; S04 §S04.4,§S04.6] ESSENTIAL** · `boot-trace.txt:1437-1447`. ·
    **Success:** stage 07 (`:1445`) + stage 08 (`:1447`).
16. **Link the status ring** (`msgqRxLink` spin until GSP ran its `msgqInit`, 4 s
    timeout). · **[S06 §8] ESSENTIAL** · `message_queue_cpu.c:348-424`. · **Success:**
    stage 09 (`boot-trace.txt:1448`); link returns OK (GSP is live on the queue).
17. **Service the GSP CPU sequencer while polling for INIT_DONE** — implement
    `run_cpu_sequencer` opcodes `REG_WRITE/REG_MODIFY/REG_POLL/DELAY_US/CORE_RESET/
    CORE_START/CORE_WAIT_FOR_HALT/CORE_RESUME`; `CORE_RESUME` re-resets GSP RISC-V,
    re-programs libos args, `StartCpu(SEC2)` via `CPUCTL_ALIAS`, polls
    `BSI_SECURE_SCRATCH_14`. · **[S04 §S04.8; S05 §S05.6] ESSENTIAL — load-bearing** ·
    `kernel_gsp.c:3819-3949`, `kernel_gsp_ga102.c:340-389`. · **Success:** sequencer writes
    drain (`boot-trace.txt:1449-1777`). **GSP cannot reach INIT_DONE without this (R3).**
    *(This is the "sequencer-servicing requirement during INIT_DONE" called out in the
    corrections; the exact opcode list is [INFERENCE] — no RVGSEQ trace yet.)*
18. **INIT_DONE received** (event `0x1001`) via the drain-and-dispatch `rpcRecvPoll`;
    doorbell each send with `GPU_REG_WR32(BAR0+0x110c00, 0)`. RPC wire header:
    `header_version=0x03000000`, `signature=0x43505256` **[C4]**. · **[S06 §7,§11]
    ESSENTIAL** · `kernel_gsp.c:3787-3796`; doorbell `dev_gsp.h:38-40`. · **Success:**
    stage 10 (`boot-trace.txt:1778`), `RPC_HDR->rpc_result==0`. Boot ≈ 2.06 s total.

#### Phase 2 — RM control plane + object tree (post-INIT_DONE)

19. **Sync `SET_GUEST_SYSTEM_INFO(1)` then `GET_GSP_STATIC_INFO(65)`.** 65 returns the
    internal client/subdevice handles + display caps. · **[S07 §S07.1] ESSENTIAL** ·
    `gsp_static_config.h:136-142`. · **Success:** both `recv result=0x0`
    (`CAP/rpc-trace.txt:3-6`); read back `hInternalClient=0xc2000005`,
    `hInternalSubdevice=0xabcd2080` **[C7]** (do not hardcode).
20. **Alloc the RM tree via `GSP_RM_ALLOC(103)`: `NV01_ROOT` → `NV01_DEVICE_0` →
    `NV20_SUBDEVICE_0`, and `NV04_DISPLAY_COMMON` parented to the DEVICE (NOT the
    subdevice).** · **[S07 §S07.5; CORRECTIONS #4] ESSENTIAL** ·
    `g_allclasses.h:192,224,228,498`. · **Success:** first display ctrl `cmd=0x00730120`
    (`SYSTEM_GET_SUPPORTED`) returns `0x0` (`AN/…110540…-decoded-full.txt:343`).

#### Phase 3 — Display engine objects + channels

21. **Alloc `NVC670_DISPLAY` (NULL params) under DEVICE; under it alloc `NVC67D` core ×1
    (instance 0, mask bit0) and one `NVC67E` window (instance 0, mask bit1); alloc
    `NVC372_DISPLAY_SW` for IMP.** · **[S08 §S08.1,§S08.7] ESSENTIAL** ·
    `nvkms-evo.c:4488-4496`; channel parent = `displayHandle` `nvkms-rm.c:2684-2688`. ·
    **Success:** alloc `result=0x0`; head0 owns window0/1 (`win>>1`, `clc67d.h:73-75`).
22. **Per DMA channel: allocate a pushbuffer ctxdma (`hObjectBuffer`), map its 4 KB
    control page (`PUT`@0x0 / `GET`@0x4), and register the PB **physical** addr/limit/
    aperture via `NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER` (`0x20800a58`) on
    `hInternalClient(0xc2000005)/hInternalSubdevice(0xabcd2080)`.** · **[S08 §S08.2,§S08.8]
    ESSENTIAL** · `disp_channel.c:727-788`. · **Success:** control page maps; the pusher
    fetches methods only after `valid=TRUE` registration.

#### Phase 4 — Detect / EDID / DP link / validate (target = DP panel `displayId 0x800`)

23. **Detect:** `GET_NUM_HEADS` → `GET_SUPPORTED` → `GET_CONNECT_STATE` (probe mask
    `0xff00`, find `displayId=0x800` DP) → `DFP_GET_INFO` (SIGNAL==DISPLAYPORT) →
    `SPECIFIC_OR_GET_INFO` (SOR index/protocol for `0x800` ⇒ **SOR 1 / DP_B**). · **[S09
    §S09.1,§S09.4] ESSENTIAL** · `ctrl0073system.h:392`, `ctrl0073dfp.h:130-135`. ·
    **Success:** probe `0xff00` at `CAP/payload-trace.txt:196`; `0x800` is the sole
    connected DP (it alone receives DP controls: EDID `:251`, LINK_CONFIG `:256`);
    `DFP_GET_INFO` reply `flags=0x0208001b` → `SIGNAL==3` (DP) (`CAP2/rpc-resp-trace.txt:175`).
24. **Read EDID:** `SPECIFIC_GET_EDID_V2` (`0x730245`, `displayId=0x800`, `COPY_CACHE_NO`)
    → the 2560×1440 timing rows. · **[S09 §S09.2] ESSENTIAL** · `nvkms-dpy.c:1128-1137`. ·
    **Success:** EDID decodes to **AOC AG241QG4**; mode list carries `@59.95/241500` and
    `@143.91/592000` (`CAP/modetest-list.txt:74-75`).
25. **DP bring-up:** `DP_SET_MANUAL_DISPLAYPORT` (`0x731365`) → `DP_AUXCH_CTRL` (`0x731341`,
    ×43) DPCD training burst → `DP_GET_LINK_CONFIG` readback. · **[S09 §S09.3] ESSENTIAL**
    (the readback itself is verification) · `ctrl0073dp.h:1671,156`. · **Success:** 60 Hz
    trains **2 lanes @ HBR2** (`GET_LINK_CONFIG` reply `laneCount=2, linkBW=0x14`,
    `CAP2/rpc-resp-trace.txt`, [D08]); **144 Hz needs 4 lanes @ HBR2** (sink caps DP1.2/
    HBR2/4-lane) **[C1]**.
26. **Validate:** `IS_MODE_POSSIBLE` (`0xc3720101`) on `NVC372_DISPLAY_SW` with 1 head + 1
    window + the EDID timing. · **[S09 §S09.5] ESSENTIAL — bypassable (R5)** ·
    `ctrlc372chnc.h:472-513`. · **Success:** `bIsPossible==TRUE`; consume the returned
    `dispClkKHz` for the modeset. *(Output `bIsPossible/dispClkKHz` was not captured —
    RVGRESP truncates to 32 dwords; read it at runtime. Bypassable only if you hardcode a
    known-good timing and set dispclk conservatively — see (b)/R5.)*

#### Phase 5 — Framebuffer + modeset + scanout (the pixel)

27. **Allocate + bind the framebuffer surface in VRAM (BAR1), 1024 B aligned, pitch-
    linear, `X8R8G8B8`.** · **[S10 §S10.4; S02 §2] ESSENTIAL** · `nvCtxDmaOffsetFromBytes`
    `nvkms-utils.h:145-151` (offset `>>8`); `NV_SURFACE_OFFSET_ALIGNMENT_SHIFT=10`. ·
    **Success:** surface phys known, ctxdma bound. Pitch must be 64 B-aligned: a tight
    2560×4 = 10240 ⇒ `PLANAR_STORAGE.PITCH = 10240>>6 = 160`; **measured driver value was
    `0x100`=256 (16384 B padded stride)** **[C3]** — implementers supply their own pitch.
28. **Push CORE modeset (NVC67D) for the chosen head, then CORE `UPDATE = 0x1`
    (`RELEASE_ELV=TRUE`, `SPECIAL_HANDLING=NONE` — NOT `MODE_SWITCH`; measured) [C11].** Methods: `PROCAMP`, `HEAD_SET_CONTROL`,
    `PIXEL_CLOCK_FREQUENCY(+CONFIGURATION+MAX)`, `DISPLAY_ID(=0x800)`, `RASTER_SIZE/
    _SYNC_END/_BLANK_END/_BLANK_START`, `VIEWPORT_SIZE_IN/_OUT/_POINT_IN`, **`SOR_SET_
    CONTROL`(`PROTOCOL=DP_B(0x9)`, `OWNER_MASK=1<<head`, on SOR index 1)**; kick `PUT`. ·
    **[S10 §S10.2,§S10.3] ESSENTIAL** · `nvkms-evo3.c:2188-2193` (SOR), `:1338-1342`
    (`HERTZ = kHz×1000`; **host sets HERTZ only — GSP-RM programs the VPLL, no host PLL
    MMIO** **[C8]**). · **Success:** raster goes live (screen blanks, no sync fault);
    refresh = pclk/(hT·vT). **[C1] measured 60 Hz dwords [EVIDENCE]:** `SOR_SET_CONTROL(1)
    =0x00000908` (OWNER=HEAD3/DP_B), `RASTER_SIZE=0x05c90aa0` (2720×1481), pclk 241.5 MHz,
    core `UPDATE=0x1` (`AN/…115116…-evo-full.txt:4710,4674,4974`). **144 Hz dwords [INFERENCE/EDID-derived]:**
    `RASTER_SIZE=0x06070a6a` (2666×1543), `PIXEL_CLOCK_FREQUENCY=0x23493400` (592 MHz)
    **[C6]**.
29. **Push WINDOW scanout (NVC67E), then WINDOW `UPDATE` (`RELEASE_ELV`).** Methods:
    `SET_CONTEXT_DMA_ISO[0]`, `SET_OFFSET[0]`, `SET_SIZE`, `SET_SIZE_IN`, `SET_SIZE_OUT`,
    **`SET_STORAGE` (BLOCK_HEIGHT only — NO MEMORY_LAYOUT field; layout comes from the
    surface alloc kind)** **[C2]**, `SET_PLANAR_STORAGE[0]=pitch>>6`, `SET_PARAMS(FORMAT
    =0xE6)`, **`SET_PRESENT_CONTROL=0`**, **`SET_COMPOSITION_CONTROL=0x00010000`
    (`BYPASS=ENABLE`; measured, [INFERENCE→verify] strictly required vs default 0)** **[C12]**;
    kick `PUT`. · **[S10 §S10.4] ESSENTIAL** · `clc67e.h:139,177,181`, `:204-212` (COMPOSITION_CONTROL);
    `SET_STORAGE` `clc67e.h:131-138` (verified only `BLOCK_HEIGHT`). · **Success:** window
    fetches the surface (no IMP/bandwidth fault). **Measured bind [EVIDENCE]:**
    `SET_CONTEXT_DMA_ISO[0]=0x00010087`, `SET_OFFSET[0]=0`, `SET_PLANAR_STORAGE[0]=0x100`,
    `SET_PARAMS=0xE6`, `SET_COMPOSITION_CONTROL=0x00010000` (`AN/…115116…-evo-full.txt`, evo-data:4823/4824/4839/4844/4866) **[C3,C12]**.
30. **Paint a square into the bound VRAM surface via the BAR1 mapping** — `X8R8G8B8` red =
    **`0x00FF0000`** (NOT S10 pseudocode's `0x00FF00FF`, which is magenta) **[C9]**. ·
    **[S10 §S10.5 step 11] ESSENTIAL** · format `clc67e.h:147`. · **Success:** a visible
    red square on the panel = **first pixel**. Minimal method budget ≈ **16 core + 11
    window (the 2 `UPDATE`s included)** vs the production driver's 4930 (F09 §F09.9; S10 §S10.5).

---

### F10.2 — (b) Explicit SKIP-FOR-V1 list (do NOT build these for first pixel)

Each item cites the section that proves it skippable on **this** GA102 + DP panel.

**Whole subsystems / engines**
- **UVM / unified memory** — not on the display path. [INFERENCE: S01 §S01.5 scope rule]
- **`nvidia-peermem` / P2P / NVLink RPCs** — only fn `72,73,1,65,76,103,10` appear; most
  of `rpc_global_enums.h` is unused. [EVIDENCE: S01 §S01.5; S06 §10]
- **Accel / 3D / compute / copy engines** — the `NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_*`
  graphics/CE static-info storm + probe-clients `0xc1e00002…6` are NOT display.
  [EVIDENCE: S07 §minimal-path]
- **NVENC/NVFBC, MIG, ConfidentialCompute** — CC is off on consumer GA102; take the plain
  msgq branches (no `authTag/aad`, whole-element checksum). [EVIDENCE: S06 §13]

**Interrupts**
- **MSI / MSI-X / any IRQ handler — POLL instead.** The GSP handshake is `rpcRecvPoll` +
  `osSpinLoop`; the "must have an IRQ" checks are Linux policy, not GSP need. [EVIDENCE:
  S02 §3] (Becomes useful later for the 144 Hz vblank/flip loop.)

**Display objects/channels (allocate the minimum)**
- **7 of 8 window channels (windows 1–7), all `NVC67B` window-immediate channels, all
  `NVC67A` cursor PIO channels, heads 1–3.** Use **head 0 + window 0**. [EVIDENCE: S08
  §minimal; S10 §minimal] *(Caveat: the SOR you bind is **fixed by default routing = SOR 1
  / DP_B** for `0x800`; you may bind it to head 0 via `OWNER_MASK` — do not hardcode head,
  R7. The capture happened to use head 3.)*
- **`NVC372_DISPLAY_SW` + `IS_MODE_POSSIBLE`** *may* be skipped IF you hardcode a known-
  good 2560×1440 timing — but then you forfeit the blessed `dispClkKHz` and the
  feasibility check. **Recommended: keep IMP (step 26).** [EVIDENCE: S08 §minimal vs S09
  §S09.5 — R5]
- **`NV40_I2C` (`0x402c`)** — DDC/I2C EDID; DP EDID comes over AUX. [EVIDENCE: S07
  §minimal-path]

**Detection controls (informational only)**
- `SYSTEM_GET_CAPS_V2`, `SPECIFIC_GET_ALL_HEAD_MASK`, `SPECIFIC_GET_CONNECTOR_DATA`,
  `SPECIFIC_GET_TYPE`, `SYSTEM_GET_BOOT_DISPLAYS`, `SPECIFIC_GET_PCLK_LIMIT` (IMP is the
  authority), `SPECIFIC_SET_EDID_V2` (RM-side cache), dongle/internal-display/mux/
  backlight/adaptivesync queries. [EVIDENCE: S09 §minimal-path]
- **`DFP_ASSIGN_SOR` (`0x731152`)** — absent from the capture (0 matches); default
  VBIOS/DCB SOR routing is used. Needed only for crossbar / 2-head-1-OR. [EVIDENCE: S09
  §S09.4]

**Modeset/scanout method groups (skip; default 0)**
- Output-scaler coefficient tables (decoded lines 3–803), OLUT/OCSC/OCSC1/CSC, DITHER,
  OUTPUT_RESOURCE, DSC + HDMI-FRL + HDMI_CTRL, MIN_FRAME_IDLE, RASTER_HBLANK_DELAY (=0),
  TILE_POSITION, multi-plane YUV ISO[1..5]/PLANAR[1..2], input scaler, composition
  `DEPTH`/`COLOR_KEY` *fields* (=0) — **but the `SET_COMPOSITION_CONTROL` method itself must
  be emitted with `BYPASS=ENABLE` (`0x00010000`), see step 29/[C12]** —, semaphores/notifiers,
  present timestamps, **interlock flags** (`interlockMask=0`). [EVIDENCE: S10 §minimal (F09 §F09.9); S08 §S08.6]

**Memory / clocking / boot extras**
- **Reclocking beyond boot** — boot devinit clocks suffice; 144 Hz is a
  `PIXEL_CLOCK_FREQUENCY` + raster-totals delta, not a reclock; GSP-RM programs the VPLL.
  [EVIDENCE: S01 §S01.5; S10 §S10.3; C8]
- **Block-linear surfaces, ReBAR/BAR resize, giant contiguous allocs** — use pitch-linear
  in the 256 MB BAR1 aperture; radix3 removes the need for a big contig buffer. [EVIDENCE:
  S10 §minimal; S02 §5]
- **Booter Unload, FWSEC-SB (`0x19`), Scrubber ucode, `.fwlogging`/libos log decode,
  TaskIsr queue (idx 1), CrashCat, `_issueRpcLarge` until a payload first exceeds
  `maxRpcSize`.** [EVIDENCE: S03 §minimal; S05 §minimal; S06 §13]
- **Hopper FSP / GSP-FMC path** — wrong chip; GA102 uses SEC2 Booter. [EVIDENCE: S05 §S05.7]
- **Page-flip / double-buffer / vblank sync** — write the surface once and leave it.
  [EVIDENCE: S10 §S10.5 step 11]

---

### F10.3 — (c) StelluxOS source-file / module layout

One Falcon routine, one RPC transport, one EVO pusher — reused across engines/channels.
Modules ordered bottom-up. **"Blobs"** = the NVIDIA firmware images a module loads/
consumes (all reusable verbatim; StelluxOS authors none — the BROM does the crypto).

| Module (dir) | Owns (sections) | Responsibility | Firmware blobs |
|---|---|---|---|
| `pci/` | S02 §1–§3 | enumerate `10de:2216`; PCI_COMMAND Mem+BME; BAR0 ioremap; BAR1/BAR3 phys; (MSI stubbed — poll) | — |
| `mem/` | S02 §4–§5; S03 §5–§7 | coherent/contig sysmem alloc, 47-bit DMA, fences/WC-flush; **FB-layout math**, `GspFwWprMeta`, **radix3** builder, libos/GSP args | — (builds tables over blobs) |
| `falcon/` | S04 §S04.2–§S04.5; S05 §S05.4–§S05.5 | generic Falcon engine: reset/BCR, **DMA load-and-go** (`DMATRF*` 256 B loop, `0x614/0x600`), BROM/PKC kick, StartCpu/wait-halt — pointed at GSP `0x110000` or SEC2 `0x840000`. **Reused 3× per boot** (FWSEC@GSP, Booter@SEC2, GSP-RM reload@GSP via CORE_RESUME); ~1689/1768 writes are the 256 B inner loop, only ~79 control writes / ~22 distinct offsets are real surface [CORRECTIONS #8] | — (executes any HS ucode) |
| `firmware/` (a.k.a. `vbios/`) | S03 §2–§3 | locate/load blobs; ELF parse + `.fwversion` gate; **VBIOS PROM extract + FWSEC parse**; bindata Booter/boot-bin + `RM_RISCV_UCODE_DESC` | `gsp_ga10x.bin` (file); **FWSEC** (VBIOS ROM); **Booter Load** (bindata); **GSP-RM boot bin / SK+BL** (bindata) |
| `gsp_boot/` | S03 §1; S04 §S04.1,§S04.6–§S04.9; S05 §S05.1–§S05.3,§S05.6 | the 10-stage orchestrator: GFW_BOOT poll → FWSEC-FRTS (WPR2) → reset-into-RISC-V → Booter Load (SEC2) → RISC-V active → **CPU-sequencer service (R3)** → INIT_DONE | consumes all four (via `firmware/`+`falcon/`+`mem/`) |
| `rpc/` | S06 (all) | msgq SPSC rings, `rpc_message_header_v` (`header_version=0x03000000`, `sig=0x43505256` [C4]), `rpcWriteCommonHeader`, send + **doorbell `0x110c00`**, recv **drain-and-dispatch**, INIT_DONE/event channel | — |
| `rm_objects/` | S07 (all) | handle scheme; `GSP_RM_ALLOC(103)`/`GSP_RM_CONTROL(76)` marshalling (serialize-down/up, large-payload); the 4 openers; ROOT→DEVICE→SUBDEVICE→DISPLAY_COMMON(parent=DEVICE) tree; read internal handles from fn=65 [C7] | — |
| `disp/` | S08 (all); S10 (all) | display object tree (`NVC670`/`C67D`/`C67E`), channel alloc + ctxdma + control-page, **EVO pushbuffer/`UPDATE`/kickoff**, the modeset+scanout method program (SOR protocol from `OR_GET_INFO`, **not** hardcoded — DP_B here [C1]) | — |
| `disp/dp/` | S09 §S09.3 | DP library: `SET_MANUAL_DISPLAYPORT`, DPCD over `DP_AUXCH_CTRL`, link training, `GET_LINK_CONFIG` (60 Hz=2-lane HBR2; 144 Hz=4-lane HBR2) | — |
| `disp/detect/` | S09 §S09.1–§S09.2,§S09.4–§S09.5 | connect-state/EDID read, SOR (`OR_GET_INFO`→SOR1/DP_B), `IS_MODE_POSSIBLE`, EDID→EVO raster math | — |
| `fb/` | S08 §S08.8; S10 §S10.4–§S10.5 | VRAM surface alloc + ctxdma bind + PB-phys registration (`0x20800a58` on `0xc2000005/0xabcd2080`); BAR1 paint of the red square (`0x00FF0000`) | — |

Dependency edges: `pci→mem→{falcon, firmware}→gsp_boot→rpc→rm_objects→disp→{disp/dp,
disp/detect, fb}`. `falcon/` is shared by `gsp_boot/` for GSP and SEC2; `mem/` is shared
by `gsp_boot/` (WprMeta/radix3), `rpc/` (ring sysmem), and `fb/` (surface).
[Mapping basis: each section's own Scope/§S0x.0 + Evidence-cited lists.]

---

### F10.4 — (d) Hardest risks and how to de-risk (ranked)

| # | Risk | Why it bites | De-risk (cited) |
|---|---|---|---|
| **R1** | **GSP boot timeouts** | Boot ≈ 2.06 s, dominated by *on-GPU* compute (FWSEC ~212 ms, Booter ~156 ms, GSP-RM init ~1.36 s), not MMIO. Too-tight host timeouts — or slowing MMIO (mmiotrace single-CPUs Ampere) — trip `RmInitAdapter failed`. [S04 §S04.9; CONTEXT_BRIEF §2] | Real monotonic-ns clock; size timeouts to source (GFW 2.05 s, RPC 1.5× default, TX-space 1 s). **Never trap/serialize MMIO**; poll. Validate against stage timestamps in `CAP/boot-trace.txt`. [S02 §6; S04 §S04.7] |
| **R2** | **WPR2 / FB-layout math** | Whole top-down layout (`frtsOffset`, `gspFwWprStart/End`, heap) is *computed*, not captured; GA102 has **no scrubber** so WPR must fit the pre-scrubbed top ~256 MB; a wrong `frtsOffset` → Booter authenticates into the wrong place. `frtsOffset` is delivered inside a DMEM cmd buffer ⇒ not in the MMIO trace. [S03 §5; S05 §S05.2 TODO-1] | Replicate `kgspCalculateFbLayout_TU102` alignments exactly (S03 §5.2); after stage 05 assert `WPR2_ADDR_LO==frtsOffset>>align`; enable the `#if 0` `GspFwWprMeta` dump (`kernel_gsp_tu102.c:641-663`) for ground truth; honor the no-scrubber clamp. [S03 §5.2–§5.4] |
| **R3** | **CPU-sequencer servicing** | GSP-RM will **never post INIT_DONE** unless the host executes the `run_cpu_sequencer` opcodes mid-boot; `CORE_RESUME` must re-reset GSP RISC-V and restart **SEC2** (via `CPUCTL_ALIAS`) and poll `BSI_SECURE_SCRATCH_14`. The exact opcode list (`boot-trace.txt:1449-1777`) is [INFERENCE] — no `RVGSEQ` trace yet. [S04 §S04.8; S05 §S05.6] | Implement every opcode from `kgspExecuteSequencerBuffer_IMPL` (`kernel_gsp.c:3819-3949`) + the GA102 `CORE_RESUME` (`kernel_gsp_ga102.c:340-389`); only `CORE_RESUME` is GA102-specific (others return `INVALID_ARGUMENT`). Add an `RVGSEQ` trace point and re-capture. [S04 §S04.8 TODO] |
| **R4** | **Queue PA delivery + doorbell semantics** | If `sharedMemPA` (page-table base) + per-queue offsets aren't in the exact GSP-args field, the status-ring link (step 16) hangs; the doorbell writes value `0` every time (pure kick) — misreading it breaks send. [S06 §2,§7,§14] | Confirm `messageQueueInitArguments` is the carrier (`gsp_init_args.h`; S03 §7.2); byte-replicate `msgqTxCreate` header fields (`msgSize/msgCount/rxHdrOff/entryOff`) from `msgq.c:200-270`; treat `0x110c00` as kick-only (GSP reads `writePtr` from shared mem). [S06 §14] |
| **R5** | **IMP vs hardcoded timing** | S09 marks `IS_MODE_POSSIBLE` ESSENTIAL (returns `dispClkKHz`, gates bandwidth/mempool); S08 says it is bypassable with a hardcoded timing. Skipping risks a mode the HW rejects at the window `UPDATE`. [S09 §S09.5 vs S08 §minimal] | Keep IMP for v1 (one RPC). If bypassing, use the **exact EDID raster** nvkms feeds *both* IMP and `HEAD_SET_RASTER_*` (must be byte-identical) and set dispclk conservatively. [S09 §minimal "Hard requirement"] |
| **R6** | **DP link can't carry 144 Hz** | 2560×1440@144 ≈ 14.2 Gb/s payload ⇒ **HBR2 ×4**. Measured trained config for **60 Hz** is `laneCount=2, linkBW=0x14` (HBR2 ×2); the 144 Hz training was **not** captured on-wire, so "HBR2 ×4" is [INFERENCE] (sink caps DP1.2/HBR2/4-lane). [S09 §S09.3; D08; CORRECTIONS #1] | Bring up **60 Hz first** (241.5 MHz, 2-lane HBR2), confirm a lit pixel, then raise `PIXEL_CLOCK_FREQUENCY.HERTZ` to 592 MHz and re-train to 4-lane HBR2; verify via `GET_LINK_CONFIG` (or DPCD 0x100/0x101). [S09 §S09.3; S10 §S10.3] |
| **R7** | **EVO data words + head/SOR binding** | The `112551` EVO capture has method **headers only**; `115116` adds measured **60 Hz** dwords (incl. `SOR_SET_CONTROL(1)=0x908` = head3/SOR1/DP_B, displayId 0x800) but **no 144 Hz** dwords. Physical head is a *driver choice* (capture used head 3). [S10 §S10.0,§S10.1; D06; CORRECTIONS #1] | **Partly resolved:** SOR index/protocol = **SOR 1 / DP_B** (read from `OR_GET_INFO` for `0x800`, do not hardcode DP_A). Bind that SOR to your chosen head via `OWNER_MASK=1<<head` (head 0 is fine). Use `115116` 60 Hz dwords + nvkms formula (self-check active=blankStart−blankEnd, refresh=pclk/(hT·vT)). Add a `nvDmaSetEvoMethodData` trace point to capture real 144 Hz dwords. [S10 §S10.0 TODO; D06] |

---

### Minimal-path notes (synthesis level)

- **Essential = exactly F10.1 (steps 1–30)** — the de-duplicated union of every section's
  own "essential" set, with BAR/DMA, the Falcon DMA mechanic, RPC framing, and
  UPDATE/kickoff each collapsed to one occurrence, ordered by the RVGBOOT 01–10 spine +
  the post-INIT_DONE control plane.
- **Skippable = exactly F10.2.** Anything not in steps 1–30 is not required for one static
  red square on this GA102 + this DP panel.
- **144 Hz is a 2-field delta on first pixel**, not new architecture: raise
  `HEAD_SET_PIXEL_CLOCK_FREQUENCY.HERTZ` to 592 MHz, use raster totals 2666×1543, and have
  the DP link at 4-lane HBR2; everything else is identical. GSP-RM programs the VPLL. [S10
  §S10.3; C6/C8]
- **Reuse, don't reimplement:** the four firmware blobs (`gsp_ga10x.bin`, FWSEC, Booter
  Load, GSP-RM boot-bin) are carried verbatim; the BROM does the crypto. [S03 §minimal;
  S05 §minimal]

---

### Evidence cited

Synthesis substrate (briefs + the ten **draft** sections in `spec-drafts/` — F10 cites these by
their draft `S0x` ids; the **finalized** merged versions are `spec-final/F01–F09` per the F10.0 map):
1. `spec-drafts/CONTEXT_BRIEF.md` §1–§8 (mission, HW §3, bring-up spine §4a–§4e, rules §6/§7/§8).
2. `spec-drafts/CONTEXT_ADDENDUM.md` (measured 60 Hz EVO dwords; 144 Hz EDID timing; corrections #1–#4).
3. `spec-drafts/CORRECTIONS.md` #1 (two monitors; DP `0x800`/head3/SOR1/**DP_B**), #2 (RPC 663 vs 1145), #3 (`header_version=0x03000000`), #4 (DISPLAY_COMMON parent=DEVICE), #5 (window bind dwords), #6 (144 Hz EDID-derived), #7 (internal handles), #8 (one DMA routine ×3), #9 (no window MEMORY_LAYOUT), #10 (host HERTZ, GSP VPLL).
4. `spec-drafts/S01-context-glossary.md` §S01.3 (two-plane arch), §S01.5 (scope/skips).
5. `spec-drafts/S02-os-prerequisites.md` §1 (PCI/BME), §2 (BAR0/1/3), §3 (poll vs MSI), §4 (DMA/47-bit/fences), §5 (contig/radix3/IOMMU), §6 (timers).
6. `spec-drafts/S03-firmware-memory.md` §1–§3 (blobs/VBIOS/FWSEC), §5 (FB layout/WprMeta), §6 (radix3), §7 (libos/GSP args).
7. `spec-drafts/S04-gsp-boot-falcon.md` §S04.1 (10-stage table), §S04.3 (DMA load-and-go), §S04.4–§S04.6 (reset/RISC-V/Booter), §S04.7 (GFW_BOOT 2.05 s), §S04.8 (sequencer), §S04.9 (timings).
8. `spec-drafts/S05-sec2-booter-frts.md` §S05.1–§S05.3 (FRTS/Booter), §S05.4 (710 SEC2 writes), §S05.6 (CORE_RESUME), §S05.7 (Hopper-FSP contrast).
9. `spec-drafts/S06-rpc-infrastructure.md` §2–§3 (rings), §5–§7 (header/build/doorbell), §8–§9 (recv/drain), §10–§12 (ids/INIT_DONE/trace), §13–§14 (minimal/TODO).
10. `spec-drafts/S07-rpc-object-model.md` §S07.1 (4 openers), §S07.2 (handles), §S07.3–§S07.4 (alloc/control), §S07.5 (tree order, DISPLAY_COMMON=DEVICE child).
11. `spec-drafts/S08-display-objects-channels.md` §S08.1–§S08.2 (objects/params), §S08.3–§S08.6 (pushbuffer/UPDATE), §S08.7 (head/window map), §S08.8 (PB-phys ctrl).
12. `spec-drafts/S09-detection-edid-validation.md` §S09.1 (enum), §S09.2 (EDID), §S09.3 (DP), §S09.4 (SOR), §S09.5 (IMP).
13. `spec-drafts/S10-modeset-scanout-pixel.md` §S10.0 (data-word caveat), §S10.1 (structure), §S10.2 (core), §S10.3 (60↔144 math), §S10.4 (window), §S10.5 (critical path), §S10.6 (pseudocode).
14. Deep-dives consulted for the corrections: `spec-drafts/D02` (RPC header), `D03` (static-info handles), `D06` (measured core modeset / SOR1-DP_B), `D07` (measured window scanout), `D08` (DP link), `D10` (pclk/raster math).

Load-bearing primary cites carried into F10 (an implementer types these):
15. `SRC/kernel-open/nvidia/nv-pci.c:653` — `pci_set_master` (BME). **[re-verified exact]**
16. `SRC/src/nvidia/arch/nvalloc/unix/src/osapi.c:3301-3304` — BAR0 uncached ioremap.
17. `SRC/kernel-open/nvidia/nv.c:4649` — `readl(regs->map)==0xFFFFFFFF` GPU-lost probe.
18. `SRC/src/nvidia/arch/nvalloc/unix/src/osinit.c:158` — DMA width 47.
19. `SRC/src/nvidia/src/kernel/gpu/gsp/message_queue_cpu.c:338,348-424,596-606` — `sharedMemPA`, status-ring link, store fence.
20. `SRC/.../gsp/arch/turing/kernel_gsp_frts_tu102.c:101,115-131,454-482,497-503` — FRTS cmd `0x15`, region desc, WPR2 verify, GFW_BOOT field.
21. `SRC/.../gsp/arch/turing/kernel_gsp_tu102.c:509-666` (`:636-639` magic; `:641-663` `#if 0` WprMeta dump) — FB layout.
22. `SRC/.../gsp/kernel_gsp.c:3306-3354` (.fwversion gate), `:3456,3513` (radix3), `:3787-3796` (INIT_DONE poll), `:3819-3949` (sequencer opcodes).
23. `SRC/.../gsp/arch/ampere/kernel_gsp_ga102.c:244-256` (Booter Load + FALCON_OS), `:340-389` (`CORE_RESUME`).
24. `SRC/.../gsp/kernel_gsp_booter.c:434` (Booter Load alloc); `SRC/.../kernel_gsp.c:3145` (boot-bin prep).
25. `SRC/src/common/inc/swref/published/ampere/ga102/dev_gsp.h:38-40` — doorbell `NV_PGSP_QUEUE_HEAD(i)=0x110c00+i*8`. **[re-verified exact]**
26. `SRC/src/nvidia/kernel/vgpu/nv/rpc.c:1368,1374-1378` (async openers), `:1666,1876` (GSP_RM_CONTROL/ALLOC).
27. `SRC/src/nvidia/generated/g_rpc-message-header.h:41-54` + `kernel/inc/vgpu/rpc_headers.h:58-61` — `rpc_message_header_v`; corrected `header_version=0x03000000`, `signature=0x43505256`.
28. `SRC/src/nvidia/generated/g_allclasses.h:192,224,228,498` — ROOT/DEVICE/SUBDEVICE/DISPLAY_COMMON ids.
29. `SRC/src/nvidia/inc/kernel/gpu/gsp/gsp_static_config.h:136-142` — internal handles (`0xc2000005`/`0xabcd2080`).
30. `SRC/src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080internal.h:1361` — PB-phys ctrl `0x20800a58`; `SRC/.../disp/disp_channel.c:727-788` producer.
31. `SRC/src/common/sdk/nvidia/inc/ctrl/ctrl0073/ctrl0073system.h:392` (CONNECT_STATE), `ctrl0073dfp.h:130-135` (DFP SIGNAL), `ctrl0073specific.h:151` (GET_EDID_V2), `ctrl0073specific.h:1082` (OR_GET_INFO), `ctrl0073dp.h:1671,156` (SET_MANUAL_DP, AUXCH_CTRL).
32. `SRC/src/common/sdk/nvidia/inc/ctrl/ctrlc372/ctrlc372chnc.h:39` (`IS_MODE_POSSIBLE=0xc3720101`), `:472-513` (params). **[re-verified exact]**
33. `SRC/src/nvidia-modeset/src/nvkms-evo3.c:1338-1342` (pclk HERTZ=kHz×1000), `:2188-2193` (SOR_SET_CONTROL value).
34. `SRC/src/common/sdk/nvidia/inc/class/clc67d.h:82` (UPDATE), `:301,304,317-320` (SOR_SET_CONTROL OWNER/PROTOCOL **DP_A=8/DP_B=9** — **re-verified exact**), `:828` (RASTER_SIZE), `:73-75` (window→head).
35. `SRC/src/common/sdk/nvidia/inc/class/clc67e.h:131-138` (**SET_STORAGE = BLOCK_HEIGHT only, no MEMORY_LAYOUT — re-verified exact**), `:139,147` (SET_PARAMS, X8R8G8B8=0xE6 — re-verified exact), `:177` (SET_PLANAR_STORAGE PITCH), `:181` (SET_CONTEXT_DMA_ISO).
36. `SRC/src/nvidia-modeset/include/nvkms-utils.h:145-151` (`nvCtxDmaOffsetFromBytes`=>>8); `nvkms-types.h:100` (`NV_SURFACE_OFFSET_ALIGNMENT_SHIFT=10`).
37. `SRC/src/nvidia/src/kernel/gpu/arch/turing/kern_gpu_tu102.c:372-375` — GFW 2.05 s timeout.

Trace / measured artifacts referenced (re-verified):
38. `CAP/boot-trace.txt:2,3,727,1437-1448,1778` — stage markers 02/03/05/07–09/10 **[re-verified `:727/1445/1778`]**.
39. `CAP/rpc-trace.txt:1-6` — async openers (between stages 06–07) + first sync RPCs; **1143 send sync / 1143 recv sync / 2 async = 1145 (re-verified)**.
40. `AN/20260530-110235-open-capture-rpc-decoded.txt` — **663 RPC minimal bring-up (re-verified, 1324 send+recv lines)**, all `result=0x0`.
41. `CAP/payload-trace.txt:196` (CONNECT_STATE `0xff00`), `:251` (EDID `0x800`), `:256` (LINK_CONFIG `0x800`).
42. `CAP/modetest-list.txt:71` (DP-2/93 connected), `:74-75` (2560×1440 @60/@144), `:112-127` (EDID → AOC AG241QG4).
43. `CAP/nvidia-smi-q.txt:56-71` — Bus/Device/SubSystem id.
44. `AN/20260530-110540-open-capture-decoded-full.txt:322-343` — minimal object tree + first display ctrl `0x00730120`.
45. `AN/20260530-112551-open-capture-evo-decoded.txt:804,819,838,863` — `SOR_SET_CONTROL[1]`/`[0]`, `HEAD_SET_RASTER_SIZE` (method headers).
46. `AN/20260530-115116-open-capture-evo-full.txt:4674,4710` — **measured 60 Hz `RASTER_SIZE=0x05c90aa0` and `SOR_SET_CONTROL(1)=0x00000908` (head3/SOR1/DP_B) — re-verified**; evo-data:4823/4824/4839/4844 (window bind `0x00010087`/`0`/`0x100`/`0xE6`).
47. `CAP2/rpc-resp-trace.txt:175` — `DFP_GET_INFO` reply `flags=0x0208001b` (SIGNAL=DP) for `0x800` [D08].

---

### Open questions / TODO

- **[TODO] §3 connector-id reconciliation** — CONTEXT_BRIEF §3 says "connector id 88
  (DisplayPort)", but the capture shows DRM **88=DP-1=disconnected** and the live panel is
  **93=DP-2** ⇒ RM **`displayId=0x800`** (different namespaces: DRM connector vs RM
  displayId vs DRM index). Flagged, not overwritten. [ADDENDUM #1; S09 TODO-1; S10 §S10.1]
- **[TODO] EVO 144 Hz data words** — only `112551` method headers + `115116` *60 Hz*
  dwords exist; add a `nvDmaSetEvoMethodData` trace point to capture the literal 592 MHz
  / `RASTER_SIZE=0x06070a6a` program on-wire (currently EDID-derived [INFERENCE]). [S10
  §S10.0; C6]
- **[TODO] CPU-sequencer opcode list** — add `RVGSEQ` to prove `boot-trace.txt:1449-1777`
  (currently [INFERENCE] from source + timing). [S04 §S04.8; R3]
- **[TODO] frtsOffset / exact WPR heap size** — read `WPR2_ADDR_LO` post-stage-05 or enable
  the `#if 0` WprMeta dump (`kernel_gsp_tu102.c:641-663`). [S03 TODO; S05 TODO-1; R2]
- **[TODO] Queue PA carrier + doorbell value** — pin the GSP-args field carrying
  `sharedMemPA`; confirm doorbell `0x110c00` is kick-only (GSP reads `writePtr` from shared
  mem). [S06 §14; R4]
- **[TODO] DP 144 Hz training** — `GET_LINK_CONFIG` shows **2-lane HBR2 @ 60 Hz**; the
  4-lane-HBR2 claim for 144 Hz is [INFERENCE] (sink caps DP1.2/HBR2/4-lane). Capture a
  144 Hz training to confirm. [S09 TODO-3; D08; R6]
- **[TODO] IMP outputs** — `bIsPossible`/`dispClkKHz` and fn=65 `fb_length` are not in the
  capture (RVGRESP truncates to 32 dwords); read them at runtime. [CORRECTIONS known-gaps]
- **[TODO] Alloc-param VALUES** — the RVGTRACE alloc hook logs only `hClass/hParent/hObject`;
  `NVC670` NULL-param defaults (numHeads/SORs), `NV04_DISPLAY_COMMON` NULL alloc params, and
  FWSEC desc V2-vs-V3 are [INFERENCE] until a param-dump or VBIOS-desc read confirms. [S03
  TODO; S07 TODO; S08 TODO]
- **[TODO] Head choice for first pixel** — the capture drove the DP panel on **head 3**;
  head is a free choice via `OWNER_MASK` (head 0 recommended for minimal), but SOR index
  (1) + protocol (DP_B) are fixed by default routing from `OR_GET_INFO`. Do not hardcode
  the head. [S10 §S10.1; R7]


## F11 — De-Risk Validation: First Pixel Proven on Hardware

> This section records the on-hardware validation that closes the spec's largest
> open gaps **before** StelluxOS implementation begins. Evidence capture:
> `traces/20260530-122852-open-capture/` (CAP3). All results below are
> **[EVIDENCE]** (measured / observed), and where noted they **upgrade** earlier
> `[INFERENCE]` labels. Per §0.6 precedence, these supersede any earlier hedge.

### F11.1 What was proven

Using `red_pixel.c` (a ~120-line `libdrm` program — see F11.5) loaded under our
instrumented open build, we drew **our own framebuffer** on the live RTX 3080 and
scanned it out:

- Solid **RED** (`0x00FF0000`) @ **2560×1440 60 Hz** → `MODESET OK`, visible.
- Solid **GREEN** (`0x0000FF00`) @ **2560×1440 144 Hz** → `MODESET OK`, visible.
- Path: `nvidia-drm → NVKMS → EVO core (NVC67D) + window (NVC67E) → SOR 1 → DP`,
  connector 93 (DP-2), crtc 42 — the exact pipeline StelluxOS reimplements.
- [EVIDENCE] `redpixel-60.txt`, `redpixel-144.txt` ("MODESET OK crtc=42 …").
- The operator visually confirmed both fills on the physical monitor (this is the
  first deliberate "draw our own pixel" — earlier captures were the driver's
  load-time modeset; `modetest`'s patterns never rendered, per CORRECTIONS #1).

### F11.2 144 Hz now MEASURED on the wire (upgrades F09 §144 [INFERENCE]→[EVIDENCE])

The 144 Hz GREEN modeset put the computed dwords on the wire, byte-identical to
F09's prediction:

- `NVC67D_HEAD_SET_RASTER_SIZE[3] = 0x06070a6a` → hTotal **2666** × vTotal **1543**. [EVIDENCE: CAP3 evo-full]
- `NVC67D_HEAD_SET_PIXEL_CLOCK_FREQUENCY[3]` (off `0x2c0c`) and `_MAX` (off `0x2c28`)
  `= 0x23493400` = **592,000,000 Hz**. [EVIDENCE: CAP3 evo-full]
- Distinct `HEAD_SET_RASTER_SIZE` values captured across the run:
  `0x05c90aa0` (60 Hz, 2720×1481) **and** `0x06070a6a` (144 Hz, 2666×1543).
- Refresh check: 592e6 / (2666×1543) = 143.9 Hz ✓.
- ⇒ The 60↔144 delta is confirmed as exactly **two knobs**: `RASTER_SIZE` + pixel
  clock. F09's 144 Hz column is now evidence, not EDID-derived inference.

### F11.3 Surface "kind" risk CLOSED — pitch-linear scans out (resolves §0.6.8 / §0.9 top risk)

The single biggest implementation risk (does GA102 require block-linear/GOB tiling
for scanout?) is resolved **negative**:

- `red_pixel` allocated a **plain pitch-linear** DRM dumb buffer: **`pitch=10240`**
  (= 2560 × 4 bytes/pixel, a *tight* pitch — no 16 KB block-linear padding),
  `size=14745600` (= 2560×1440×4), `bpp=32`, format XRGB8888. [EVIDENCE: redpixel-*.txt]
- It scanned out cleanly at **both** 60 and 144 Hz.
- ⇒ **StelluxOS can start with the simplest pitch-linear scanout surface.** The
  16384-byte pitch (`SET_PLANAR_STORAGE=0x100`) observed earlier was NVKMS's own
  framebuffer choice, **not** a hardware requirement. For a tight 2560-wide XRGB8888
  surface the window-channel `SET_PLANAR_STORAGE` value is `10240>>6 = 160 (0xA0)`.
- Caveat retained: this used DRM dumb buffers (RM picks the memory "kind"); when
  StelluxOS allocates VRAM directly it must tag the surface as **pitch/linear**
  (not block-linear) — but the *mode* (pitch-linear works) is now proven.

### F11.4 modetest mode-spec quirk (documented; not a blocker)

`modetest -s <conn>:2560x1440-144` fails with
`failed to find mode "2560x1440-144.00Hz"` even on the correct connector — its
name-matcher is finicky about the refresh suffix. **Select modes by
`drmModeModeInfo.vrefresh`** (as `red_pixel` does), not by modetest's string form.
The harness `awk '/connected/'` bug (matched "dis**connected**" → picked the dead
port 88) is also fixed → it now resolves `connector=93 (DP-2)`.

### F11.5 `red_pixel.c` — the StelluxOS reference prototype

`/home/flare/dev/gpu-repro/red_pixel.c` is the minimal, working recipe and the
clearest template for the StelluxOS display module:
1. open the `nvidia-drm` card; 2. find the **connected** connector with modes;
3. pick the mode by target `vrefresh` (or preferred); 4. create a **pitch-linear**
buffer, fill it; 5. `drmModeSetCrtc(crtc, fb, connector, mode)`.
StelluxOS performs the same five steps, but issues the underlying
**`GSP_RM_ALLOC`/`GSP_RM_CONTROL` RPCs (F06) + EVO core/window methods (F09)**
directly instead of via DRM ioctls. The captured EVO trace for *this* program is
the ground-truth method/value sequence to match.

### Evidence cited
- `traces/20260530-122852-open-capture/redpixel-60.txt`, `redpixel-144.txt`
  (mode, `pitch=10240`, `MODESET OK`).
- `analysis/20260530-122852-open-capture-evo-full.txt` — `HEAD_SET_RASTER_SIZE`
  `0x05c90aa0` & `0x06070a6a`; pixel clock `0x23493400` at off `0x2c0c`/`0x2c28`.
- `traces/20260530-122852-open-capture/{VERDICT.txt,modetest-set-144.txt}`
  (VERDICT=OK; modetest 144 mode-not-found).
- `/home/flare/dev/gpu-repro/red_pixel.c` (reference prototype).

### F11.6 GSP CPU-sequencer opcode stream — resolves F04 sequencer ([INFERENCE]→[EVIDENCE])

The 3rd Falcon invocation (GSP-RM bootloader load) is driven by the GSP via a
**417-opcode CPU sequencer** the host interprets in `kgspExecuteSequencerBuffer_IMPL`
(`kernel_gsp.c:3819`). Opcode enum confirmed at `ctrlc372...`/`kernel_gsp.c:3851` switch +
`GSP_SEQ_BUF_OPCODE_REG_WRITE = 0`: `op0=REG_WRITE`(2dw), `op2=REG_POLL`(5dw),
`op5=CORE_RESET`, `op6=CORE_START`, `op7=CORE_WAIT_FOR_HALT`, `op8=CORE_RESUME`.
Decoded sequence [EVIDENCE: `CAP3/seq-trace.txt`]:
1. `REG_POLL 0x110040` (mask 0x80000000) → `CORE_RESET`(op5).
2. `REG_WRITE DMATRFBASE(0x110110)=0x02758410` (GSP-RM bootloader FB phys), then the
   **256-byte DMA loop**: `REG_WRITE DMATRFMOFFS(0x110114)/DMATRFFBOFFS(0x11011c)/
   DMATRFCMD(0x110118)=0x614(IMEM)` then `REG_POLL DMATRFCMD` for IDLE — repeated for the
   IMEM blocks (offsets 0x100,0x200,…), then `DMATRFBASE=0x02758451` + `DMATRFCMD=0x600(DMEM)`
   loop for DMEM.
3. BROM/PKC: `0x111210=0x1f10`(PARAADDR), `0x11119c=0x400`(ENGIDMASK), `0x111198=1`(UCODE_ID),
   `0x111180=1`(MOD_SEL=RSA3K); `MAILBOX0(0x110040)=0xfe` (**resolves the prior `[UNCERTAIN]`
   sentinel** in F04/D01); `BOOTVEC(0x110104)=0x100`.
4. `CORE_START`(op6) → `CORE_WAIT_FOR_HALT`(op7) → `CORE_RESUME`(op8).
⇒ **StelluxOS must implement a 9-opcode sequencer interpreter** (REG_WRITE/REG_MODIFY/REG_POLL/
DELAY_US/REG_STORE/CORE_RESET/CORE_START/CORE_WAIT_FOR_HALT/CORE_RESUME); the GSP hands it this
buffer and the boot will not reach INIT_DONE if it is not serviced. Same DMA-load-and-go as F04,
here expressed as sequencer opcodes.

### F11.7 IS_MODE_POSSIBLE reply decoded — resolves F08 IMP ([TODO]→[EVIDENCE])

Captured tail of the `NVC372_CTRL_IS_MODE_POSSIBLE_PARAMS` reply (params start at message word
13; struct fields confirmed `ctrlc372chnc.h:472-512`) [EVIDENCE: `CAP3/rpc-resp-trace.txt` RVGRESP_IMP]:
- `dispClkKHz` = w[491] = `0x00149970` = **1,350,000 kHz** (GSP-picked display pipe clock).
- `worstCaseDomain` = w[492..493] = `"DHUBCLK"`.
- `bUseCachedPerfState` = w[494] = 0.
- `bIsPossible` = **TRUE** (region ~w[460]; the modeset proceeded; exact dword [INFERENCE] on
  the head[8]/window[32] sub-struct sizes).
⇒ The host does **not** compute dispclk — it reads `dispClkKHz` back from the IMP reply and
GSP-RM programs the clock. For first pixel, treat IMP as a gate: send the timing, require
`bIsPossible==TRUE`.

### Open questions / TODO (remaining, all non-blocking for first pixel)
- When StelluxOS allocates VRAM directly, confirm the surface "kind"/PTE flags that mark it
  pitch-linear to the display engine (the scanout *mode* is proven via DRM dumb buffers;
  the direct-VRAM allocation tagging is the one Phase-2 detail to confirm in-context).


