# VL805 (VIA Labs VL805/VL806) xHCI Controller — Known Issues Research

Compiled from extensive web research. All sources cited inline.

---

## Table of Contents

1. [Issue 1: TRB Overfetch Beyond Segment Boundaries](#issue-1-trb-overfetch-beyond-segment-boundaries)
2. [Issue 2: Broken Endpoint DCS (Dequeue Cycle State)](#issue-2-broken-endpoint-dcs-dequeue-cycle-state)
3. [Issue 3: Link TRB Dequeue Pointer Bug](#issue-3-link-trb-dequeue-pointer-bug)
4. [Issue 4: Hub TT Babble Error (Split Transaction Timing)](#issue-4-hub-tt-babble-error-split-transaction-timing)
5. [Issue 5: SuperSpeed Bulk OUT Data Corruption](#issue-5-superspeed-bulk-out-data-corruption)
6. [Issue 6: Split Transaction Handling at Frame Boundaries](#issue-6-split-transaction-handling-at-frame-boundaries)
7. [Issue 7: 64-bit MMIO Register Access (AC64 Bug)](#issue-7-64-bit-mmio-register-access-ac64-bug)
8. [Issue 8: Catastrophic USB Subsystem Crash](#issue-8-catastrophic-usb-subsystem-crash)
9. [Issue 9: USB Device Enumeration Failures Through Hubs](#issue-9-usb-device-enumeration-failures-through-hubs)
10. [Issue 10: Firmware Not Reloaded After PCI Reset](#issue-10-firmware-not-reloaded-after-pci-reset)
11. [Issue 11: xHCI Halt Timeout / Non-Compliance](#issue-11-xhci-halt-timeout--non-compliance)
12. [Issue 12: IOMMU / IO_PAGE_FAULT on x86](#issue-12-iommu--io_page_fault-on-x86)
13. [VL805 Firmware Version History](#vl805-firmware-version-history)
14. [VL805 vs VL817 Comparison](#vl805-vs-vl817-comparison)
15. [Stellux Relevance Summary](#stellux-relevance-summary)

---

## Issue 1: TRB Overfetch Beyond Segment Boundaries

**Source:** [Patchew: usb: xhci: Enable the TRB overfetch quirk on VIA VL805](https://patchew.org/linux/20250107153509.727b981e@foxbook/),
[raspberrypi/linux#4685](https://github.com/raspberrypi/linux/issues/4685),
[Linux stable 6.13](https://www.spinics.net/lists/stable-commits/msg400705.html)

**Symptom:** Endpoints go idle and stop processing TRBs. On x86 systems with
IOMMU, IO_PAGE_FAULT errors appear. On RPi4 without IOMMU, endpoints silently
stall — transfers simply stop completing.

**Root cause:** The VL805 prefetches up to **four TRBs beyond the end of a
transfer ring segment**, even when the segment terminates with a Link TRB at a
page boundary. The chip claims to support standard 4KB pages but reads past
them. If the prefetched memory happens to contain TRBs belonging to a
**different** endpoint's ring, and that ring's doorbell is later rung, the
controller uses the stale prefetched data without refreshing from system RAM.
If the cycle bit is stale (doesn't match the expected producer cycle state),
the endpoint stays idle indefinitely.

**Fix (Linux):** The `XHCI_TRB_OVERFETCH` quirk (renamed from
`XHCI_ZHAOXIN_TRB_FETCH`) allocates a **dummy page** after each transfer ring
segment. This ensures the overfetch reads harmless zeroed memory instead of
another ring's TRBs. Merged into Linux 6.13-stable (January 2025). The
Raspberry Pi team originally tried shortening each segment by 4 TRBs but
found the quirk approach simpler.

**Stellux relevance:** **HIGH.** Stellux allocates transfer ring segments from
a general-purpose allocator. If two different endpoint rings end up on
adjacent pages, the VL805 can prefetch one ring's TRBs into another ring's
processing context. This could explain endpoints going idle after a doorbell
ring — the controller processes stale TRBs with wrong cycle bits and stops.
The fix is to allocate an extra guard page after each transfer ring segment,
or ensure segments are at least 2 pages apart.

---

## Issue 2: Broken Endpoint DCS (Dequeue Cycle State)

**Source:** [raspberrypi/linux PR#3066](https://github.com/raspberrypi/linux/pull/3066)

**Symptom:** RTL2832U USB DVB-T dongles (and other devices) hang or produce
errors after an endpoint error/stall recovery. The device stops receiving data
and `dmesg` shows errors.

**Root cause:** When the VL805 halts on a TRB due to an error (e.g., short
packet, stall), the **DCS (Data toggle / Cycle State) field** in the Output
Endpoint Context maintained by hardware is **not updated** to reflect the
current cycle state. Software that reads the endpoint context after a halt
gets a stale DCS value, leading to incorrect ring processing after recovery.

**Fix (Linux):** The `XHCI_EP_CTX_BROKEN_DCS` quirk. Instead of reading the
DCS from the endpoint context, the driver reads the cycle bit directly from
the TRB that the xHC stopped on. Merged into RPi kernel July 2019 by Jonathan
Bell (P33M).

**Stellux relevance:** **HIGH.** After any endpoint stall or error recovery,
Stellux reads the endpoint context to determine the dequeue pointer and cycle
state. If the VL805 doesn't update DCS correctly, Stellux will use the wrong
cycle bit when constructing new TRBs after recovery, causing the ring to
appear empty to the controller. This directly affects stall recovery on EP0
(control endpoint), which is critical for the keyboard enumeration scenario.
The fix is to read the cycle bit from the TRB pointed to by the TR Dequeue
Pointer in the endpoint context, not from the DCS field in the context itself.

---

## Issue 3: Link TRB Dequeue Pointer Bug

**Source:** [raspberrypi/linux PR#3929](https://github.com/raspberrypi/linux/pull/3929),
[raspberrypi/linux#3919](https://github.com/raspberrypi/linux/issues/3919)

**Symptom:** Endpoint becomes stuck, ring expansion events are triggered
erroneously, bulk transfers hang.

**Root cause:** The VL805 **cannot handle the TR Dequeue Pointer being set to
a Link TRB**. When the Set TR Dequeue Pointer command points to a Link TRB,
the hardware-maintained endpoint context becomes stuck at the Link TRB
address. Whenever the enqueue pointer wraps around to the dequeue position,
erroneous ring expansion events are generated.

**Fix (Linux):** When searching for the end of the current TD and the ring
cycle state during stall/error recovery, if the search lands on a Link TRB,
**move to the next segment** instead of using the Link TRB as the dequeue
pointer. This must account for cases where the Link TRB also toggles the ring
cycle state. Merged October 2020.

**Stellux relevance:** **HIGH.** Stellux's `_recover_stalled_control_endpoint()`
issues Set TR Dequeue Pointer after Reset Endpoint. If the dequeue pointer
happens to land on a Link TRB, the VL805 will reject or mishandle the command.
This is a likely contributor to the TRB_ERROR responses seen during EP0 stall
recovery. The fix: when computing the new dequeue pointer for Set TR Dequeue
Pointer, skip past any Link TRBs to the first normal TRB of the next segment.

---

## Issue 4: Hub TT Babble Error (Split Transaction Timing)

**Source:** [raspberrypi/linux PR#5262](https://github.com/raspberrypi/linux/pull/5262)

**Symptom:** USB devices behind hubs produce "babble" errors. Devices like
RTL2832U DVB-T dongles fail intermittently. Full-speed and low-speed devices
behind high-speed hubs are affected.

**Root cause:** The VL805's Transaction Translator (TT) scheduling hardware
has a timing bug. When submitting URBs to endpoints on devices behind a hub
TT, the hardware can generate **babble** frames on its own — meaning the
VL805 sends data that extends past the allocated time slot in the USB frame,
violating timing constraints. The patch author (Jonathan Bell / P33M)
described it as: "spinning is unavoidable but not sufficient (the hardware can
still cause babble by itself)."

This is **architecture-agnostic** — it affects VL805 on any platform, not just
RPi4. P33M stated: "I have no reason to believe that the TT bug is fixed in
any other version of non-Pi4 VL805 firmware."

**Fix (Linux):** The `XHCI_VLI_HUB_TT_QUIRK` adds a maximum **125µs delay**
when submitting URBs to hub endpoints. Multiple URBs are typically pipelined
to maintain throughput, so the delay mainly affects single-URB submissions.
Described by P33M as "pretty horrible, but in the absence of a hub firmware
update it'll have to do for now." Merged December 2022. A firmware fix
(0138c0) was later released in January 2023, and the kernel quirk was
constrained to only apply to older firmware versions.

**Stellux relevance:** **VERY HIGH.** This is the most directly relevant issue.
In the Stellux scenario, keyboard and mouse are both full-speed devices behind
a high-speed hub with a single TT. The VL805's TT timing bug can cause
babble errors during split transactions, which corrupt data on the shared TT
and cause downstream devices to fail. Even without the Linux quirk's 125µs
delay, Stellux should be aware that rapid back-to-back TT split transactions
can trigger hardware-generated babble. This may contribute to the SET_IDLE
STALL cascading into keyboard enumeration failure — the TT may be in a bad
state not just from the STALL but also from babble-induced corruption.

---

## Issue 5: SuperSpeed Bulk OUT Data Corruption

**Source:** [raspberrypi/linux PR#5173](https://github.com/raspberrypi/linux/pull/5173),
[raspberrypi/linux#4844](https://github.com/raspberrypi/linux/issues/4844)

**Symptom:** USB 3.0 SSD drives connected through USB 3.0 hubs experience
data corruption, causing boot file corruption after reboots.

**Root cause:** After months of back-and-forth with VIA, a root cause was
identified involving bulk OUT burst handling. The exact details are not public,
but the fix involves mitigations for the `VLI_SS_BULK_OUT_BUG` quirk related
to how the VL805 handles SuperSpeed bulk OUT transfer bursting.

**Fix (Linux):** The `VLI_SS_BULK_OUT_BUG` quirk was expanded with additional
mitigations for burst scheduling. Merged September 2022.

**Stellux relevance:** **LOW.** This affects USB 3.0 SuperSpeed transfers.
Stellux's keyboard/mouse scenario uses USB 2.0 full-speed devices. However,
it shows a pattern of VL805 firmware bugs in transfer scheduling.

---

## Issue 6: Split Transaction Handling at Frame Boundaries

**Source:** [RPi Forums: VL805 firmware release 0138c0](https://forums.raspberrypi.com/viewtopic.php?t=345334),
[raspberrypi/linux#5586](https://github.com/raspberrypi/linux/issues/5586)

**Symptom:** Packet loss when using full-speed USB CDC devices through
single-TT hubs (e.g., Infineon CY7C65642). Prevents full USB bandwidth
utilization on USB 2.0 CDC devices running at 12 Mbit/s.

**Root cause:** The VL805 firmware incorrectly handles **non-periodic TT
transactions at the end of a USB 1ms frame** boundary. Periodic traffic
(interrupt, isochronous) should be scheduled for the start of a frame, and
non-periodic traffic (control, bulk) fills the remainder. The VL805 gets the
boundary timing wrong, causing split transactions to be issued at the wrong
time in the frame, resulting in the hub TT rejecting or dropping them.

**Fix:** VL805 firmware version **0138c0** (January 2023, beta) includes
"a fix for handling of split transactions" — specifically for non-periodic TT
handling at frame boundaries. The kernel `XHCI_VLI_HUB_TT_QUIRK` was also
updated to only apply to firmware versions older than 0138c0.

**Stellux relevance:** **VERY HIGH.** This is directly relevant. Control
transfers (like GET_DESCRIPTOR, SET_ADDRESS, SET_IDLE) are non-periodic and
go through the hub's TT via split transactions. If the VL805 firmware
schedules these split transactions at the wrong point in the USB frame, the
hub TT may reject them. This could explain why EP0 control transfers fail
with STALL_ERROR on the second device — the TT is getting confused by
incorrectly timed split transactions near frame boundaries, especially when
periodic interrupt transfers (from the already-configured mouse) are also
competing for TT bandwidth in the same frame.

**Note:** The firmware fix (0138c0) is the latest VL805 firmware as of the
most recent information. If the RPi4 running Stellux has older firmware, this
bug is present. There is no software workaround other than the 125µs delay
quirk, which only partially mitigates it.

---

## Issue 7: 64-bit MMIO Register Access (AC64 Bug)

**Source:** [raspberrypi/firmware#1424](https://github.com/raspberrypi/firmware/issues/1424)

**Symptom:** 64-bit MMIO reads return incorrect data (high and low 32-bit
halves contain the same value). Windows 10 stock xHCI driver fails entirely.
U-Boot USB support fails.

**Root cause:** The RPi4's PCIe Root Complex cannot properly translate 64-bit
(QWORD) MMIO accesses into PCIe transactions. 64-bit reads return the same
32-bit value in both halves (e.g., `0xabcd1234abcd1234` instead of
`0x00000000abcd1234`). 64-bit writes are similarly broken. The VL805 firmware
reports `HCCPARAMS1.AC64 = 1` (claiming 64-bit address support), which causes
some drivers (notably Windows' `usbxhci.sys`) to use 64-bit MMIO accesses.

**Fix:** All register accesses must use two 32-bit transfers instead of one
64-bit transfer. Linux already does this by default. The RPi Foundation
refused to change the VL805 firmware's AC64 bit because later Pi4 models
(4GB/8GB) use memory above the 4GB boundary, and clearing AC64 would prevent
the controller from addressing that memory.

**Stellux relevance:** **HIGH.** If Stellux performs any 64-bit register
writes to xHCI operational or runtime registers (e.g., writing DCBAAP,
ERSTBA, or CRCR as a single 64-bit store), the values will be corrupted.
All xHCI register accesses must be split into two 32-bit writes: low 32 bits
first, then high 32 bits. This is a fundamental correctness requirement.

---

## Issue 8: Catastrophic USB Subsystem Crash

**Source:** [raspberrypi/linux#5926](https://github.com/raspberrypi/linux/issues/5926),
[raspberrypi/linux#5980](https://github.com/raspberrypi/linux/issues/5980)

**Symptom:** All USB devices simultaneously disconnect during normal operation
(web browsing, high-speed transfers). Mouse and keyboard stop responding.
SSH still works (network is on a different bus). Reconnecting USB devices
fails — the hub shows "device descriptor read/64, error -71" in a loop.
Only a full reboot recovers.

**Root cause:** Appears to be a VL805 firmware crash triggered by specific
traffic patterns (possibly related to the TRB overfetch bug, TT babble, or
DMA errors). The hub (VIA Labs VL3431, internal to the RPi4) disconnects
from the root port, taking all downstream devices with it. The VL805 then
fails to re-enumerate the hub, producing continuous error -71 messages.

**Fix:** No definitive fix. The TRB overfetch quirk (Issue 1) and firmware
updates may reduce frequency.

**Stellux relevance:** **MEDIUM.** This represents the worst-case failure mode
— a complete USB subsystem crash requiring reboot. While the immediate Stellux
scenario is about enumeration stalls (not total crashes), the same underlying
VL805 bugs that cause catastrophic crashes may contribute to the less severe
enumeration failures.

---

## Issue 9: USB Device Enumeration Failures Through Hubs

**Source:** [raspberrypi/linux#3543](https://github.com/raspberrypi/linux/issues/3543),
[raspberrypi/linux#4443](https://github.com/raspberrypi/linux/issues/4443),
[raspberrypi/linux#4743](https://github.com/raspberrypi/linux/issues/4743),
[RPi Forums: various threads](https://forums.raspberrypi.com/viewtopic.php?t=243893)

**Symptom:** "device descriptor read/64, error -71", "device not responding to
setup address", "device not accepting address X, error -71". Particularly
affects HID devices (keyboards, mice, gamepads). First device may enumerate
fine; second and subsequent devices fail. Intermittent — sometimes works after
hot-plug, sometimes never works.

**Root cause:** Multiple contributing factors:

1. **Single-TT hub contention:** On single-TT hubs, all FS/LS devices share
   one Transaction Translator. If any device's transaction leaves the TT in a
   bad state (stall, babble, timeout), all other devices' split transactions
   through that TT are affected.

2. **VL805 TT timing bug (Issue 4/6):** The controller's split transaction
   scheduling is broken, causing frame boundary violations that the hub TT
   rejects.

3. **Power delivery issues:** Some hubs don't supply enough current for
   multiple devices during simultaneous enumeration.

4. **Error -71 (`EPROTO`):** Protocol error during split transaction. This
   typically means the hub TT detected a framing error, timeout, or babble
   in the full-speed/low-speed transaction.

**Fix:** Combination of firmware update (0138c0), kernel TT quirk, and
using multi-TT hubs where possible.

**Stellux relevance:** **CRITICAL.** This is exactly the Stellux scenario —
second device enumeration through a single-TT hub fails after the first
device has been configured. The error -71 (EPROTO) maps to the STALL_ERROR
that Stellux sees. The root cause chain is: mouse enumerates → SET_IDLE stalls
→ TT state corrupted → keyboard enumeration's split transactions fail through
the contaminated TT.

---

## Issue 10: Firmware Not Reloaded After PCI Reset

**Source:** [RPi Forums: Reloading VL805 firmware after boot](https://forums.raspberrypi.com/viewtopic.php?t=365719),
[raspberrypi/linux#6260](https://github.com/raspberrypi/linux/issues/6260)

**Symptom:** After a PCI bus reset (triggered by EMI, power glitch, or
software reset), the VL805 becomes non-functional. All USB devices disconnect
and cannot re-enumerate.

**Root cause:** On RPi4, the VL805's firmware is loaded from the VideoCore
GPU during initial boot, not from an onboard SPI EEPROM. When the PCI bus
resets, the firmware is lost and not automatically reloaded. A kernel firmware
loader exists but only runs during initial PCI enumeration.

**Fix (Linux):** A firmware loader patch allows reloading VL805 firmware after
PCI reset via the RPi firmware interface.

**Stellux relevance:** **LOW** for the immediate enumeration issue, but
**HIGH** for overall system robustness. If Stellux performs an xHCI reset
that triggers a PCI-level reset, the VL805 firmware may be lost. Stellux
should avoid full PCI bus resets of the VL805.

---

## Issue 11: xHCI Halt Timeout / Non-Compliance

**Source:** [billauer.co.il: VIA VL805 USB 3.0 PCIe adapter](https://billauer.co.il/blog/2019/07/via-vl805-superspeed-pcie-linux/),
[askubuntu.com: VL805 PCIe USB card fails](https://askubuntu.com/questions/1319061/vl805-pcie-usb-card-fails)

**Symptom:** "xHCI HW did not halt within 16000 usec", "Host not halted after
16000 microseconds", "can't setup: -110". Controller declared dead.

**Root cause:** The VL805 is not fully xHCI compliant in its halt behavior.
When the USBCMD.HCHalt bit is set, the controller may not transition to the
Halted state within the xHCI-specified timeout. This appears to primarily
affect x86 systems with certain IOMMU configurations, and may be related
to the VL805 performing DMA to unmapped addresses during shutdown (causing
IOMMU faults that prevent the halt from completing).

**Fix:** Disable IOMMU (`intel_iommu=off` or `iommu=soft`) on x86. On RPi4,
this is less of an issue because there is no IOMMU.

**Stellux relevance:** **MEDIUM.** If Stellux ever needs to halt and restart
the xHCI controller (e.g., during error recovery or reconfiguration), it
should be prepared for the halt to take longer than the spec allows, or to
fail entirely. Use generous timeouts and have a fallback path.

---

## Issue 12: IOMMU / IO_PAGE_FAULT on x86

**Source:** [askubuntu.com: VL805 PCIe USB card fails](https://askubuntu.com/questions/1319061/vl805-pcie-usb-card-fails),
[Patchew: TRB overfetch](https://patchew.org/linux/20250107153509.727b981e@foxbook/)

**Symptom:** AMD-Vi or Intel VT-d IO_PAGE_FAULT errors, "Host System Error"
messages, controller marked dead.

**Root cause:** The TRB overfetch bug (Issue 1) causes the VL805 to DMA-read
memory outside the allocated transfer ring segments. On systems with IOMMU,
these out-of-bounds reads hit unmapped pages, generating IO_PAGE_FAULT
interrupts. The VL805 then receives a PCI Completion Abort, which it treats
as a Host System Error.

**Fix:** The TRB overfetch quirk (Issue 1) resolves this by ensuring the
overfetch reads a valid dummy page.

**Stellux relevance:** **LOW.** RPi4 does not have an IOMMU, so the DMA reads
succeed (returning whatever happens to be in physical memory at those
addresses). However, the underlying bug (reading wrong memory) still affects
Stellux through stale cycle bits (Issue 1).

---

## VL805 Firmware Version History

| Version   | Date       | Status  | Changes |
|-----------|------------|---------|---------|
| 0137ab    | ~2019      | Default | Initial shipping firmware |
| 0137ac    | ~2020      | Stable  | Improved USB downstream power switching (uhubctl support) |
| 0137ad    | ~2020      | Stable  | Superseded 0137ac |
| 0138a1    | May 2020   | Stable  | Maintains ASPM bits; improved full-speed isochronous (removed 792-byte cap, up to 1023 bytes) |
| **0138c0** | **Jan 2023** | **Beta** | **Fix for handling of split transactions** — non-periodic TT handling at frame boundaries |
| 0138c1    | ~2023      | Beta    | Minor update to 0138c0 |

**Critical note:** Version 0138c0 is the most relevant firmware for the Stellux
scenario. It fixes the split transaction timing bug (Issue 6) that is likely
a major contributor to the keyboard enumeration failure. To check the installed
firmware version: `vcgencmd bootloader_config | grep VL805` or `rpi-eeprom-update`.

**Source:** RPi Foundation has no access to VL805 firmware source code and must
rely on VIA Labs to develop and provide fixes. Bug reports are sent to VIA with
test cases when reproducible issues are found.

---

## VL805 vs VL817 Comparison

| Feature | VL805 | VL817 |
|---------|-------|-------|
| **Type** | USB 3.0 **Host Controller** (PCIe → USB) | USB 3.1 Gen1 **Hub Controller** (upstream USB → downstream USB) |
| **USB Spec** | USB 3.0 / xHCI 1.0 | USB 3.1 Gen1 |
| **Ports** | 4 downstream | 2 or 4 downstream |
| **Role** | Connects PCIe bus to USB devices | Connects one USB port to multiple USB ports |
| **Use in RPi4** | Main USB host controller | Not used (hub is VIA Labs VL3431) |
| **Integrated VReg** | No | Yes |
| **USB Charging** | No | Yes |

**Stellux relevance:** The USB 2.0 hub inside the RPi4 has PID `0x3431` (VIA
Labs Hub). This is the internal hub between the VL805 host controller and the
external USB 2.0 ports. It is a **single-TT hub** — all full-speed/low-speed
traffic shares one Transaction Translator. This is important because TT bugs
in the VL805 interact with the single-TT nature of this hub.

---

## Stellux Relevance Summary

### Directly Applicable Issues (in order of likely impact)

| # | Issue | Relevance | Why |
|---|-------|-----------|-----|
| 4 | Hub TT Babble Error | **CRITICAL** | VL805 generates babble during TT split transactions; corrupts shared TT state; directly causes downstream device failures on single-TT hubs |
| 6 | Split Transaction Frame Boundary | **CRITICAL** | Non-periodic split transactions (control transfers) scheduled at wrong frame time; hub TT rejects them; directly causes enumeration failures for second device |
| 9 | Multi-Device Hub Enumeration | **CRITICAL** | Exact symptom match — second device fails with error -71/STALL through shared TT |
| 2 | Broken Endpoint DCS | **HIGH** | After EP0 stall recovery, VL805 gives wrong cycle state; causes ring processing to break |
| 3 | Link TRB Dequeue Pointer | **HIGH** | Set TR Dequeue Pointer fails if it lands on Link TRB; directly explains TRB_ERROR during stall recovery |
| 1 | TRB Overfetch | **HIGH** | Endpoints can go idle if adjacent ring segments have stale cycle bits |
| 7 | 64-bit MMIO | **HIGH** | Must verify all register writes are 32-bit; 64-bit writes silently corrupt |

### Recommended Stellux Mitigations

1. **TT Buffer Clear after EP0 STALL** (addresses Issues 4, 6, 9):
   Already identified in `xhci-usb-enumeration-stalls-analysis.md`. After any
   EP0 STALL on FS/LS device behind HS hub, issue CLEAR_TT_BUFFER.

2. **Link TRB avoidance in Set TR Dequeue Pointer** (addresses Issue 3):
   When computing the new dequeue pointer after stall recovery, if it lands on
   a Link TRB, advance to the first TRB of the next segment. Account for
   cycle state toggle on Link TRBs with the Toggle Cycle bit set.

3. **Read DCS from TRB, not endpoint context** (addresses Issue 2):
   After Reset Endpoint, read the cycle bit from the TRB at the dequeue pointer
   rather than from the endpoint context DCS field.

4. **Transfer ring segment guard pages** (addresses Issue 1):
   Allocate transfer ring segments with an extra guard page after them (or
   ensure at least 4 TRB slots of padding). Alternatively, ensure ring
   segments are allocated on 8KB boundaries (2 pages).

5. **Verify 32-bit register access** (addresses Issue 7):
   Audit all xHCI register writes to ensure 64-bit registers (DCBAAP, ERSTBA,
   CRCR, etc.) are written as two separate 32-bit stores, low word first.

6. **TT scheduling delay** (addresses Issues 4, 6):
   Consider adding a small delay (up to 125µs) between submitting split
   transaction TRBs to the same TT. This is the approach Linux uses
   (`XHCI_VLI_HUB_TT_QUIRK`) and reduces babble occurrence, though it cannot
   eliminate it entirely.

7. **Generous halt/command timeouts** (addresses Issue 11):
   Use timeouts significantly longer than xHCI spec minimums for commands like
   HC Halt, Reset, Stop Endpoint. The VL805 may be slow to respond.

---

## Complete Linux Kernel Quirk Summary for VL805

All quirks applied by Linux to VIA VL805 (PCI vendor `0x1106`, device `0x3483`):

| Quirk Flag | PR/Commit | Description |
|------------|-----------|-------------|
| `XHCI_LPM_SUPPORT` | Upstream | Enable USB Link Power Management |
| `XHCI_EP_CTX_BROKEN_DCS` | [PR#3066](https://github.com/raspberrypi/linux/pull/3066) | Don't trust DCS in endpoint context after halt |
| `XHCI_AVOID_DQ_ON_LINK` | [PR#3929](https://github.com/raspberrypi/linux/pull/3929) | Skip Link TRBs when setting dequeue pointer |
| `XHCI_VLI_HUB_TT_QUIRK` | [PR#5262](https://github.com/raspberrypi/linux/pull/5262) | Add 125µs delay for TT split transaction scheduling |
| `XHCI_VLI_SS_BULK_OUT_BUG` | [PR#5173](https://github.com/raspberrypi/linux/pull/5173) | Mitigate SS bulk OUT burst corruption |
| `XHCI_TRB_OVERFETCH` | [Mainline](https://patchew.org/linux/20250107153509.727b981e@foxbook/) | Allocate dummy page after ring segments for overfetch |

All authored/discovered by Jonathan Bell (P33M) of Raspberry Pi.
