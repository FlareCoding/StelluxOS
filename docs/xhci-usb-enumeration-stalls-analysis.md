# XHCI USB Enumeration Stalls — Root Cause Analysis

## Summary

When a Logitech mouse (VID=0x046d, PID=0xc08b) is plugged into a Raspberry Pi 4
(VL805 xHCI controller) alongside a keyboard through a USB 2.0 hub, the keyboard
fails to enumerate and stops responding to events. The root cause is a **missing
TT (Transaction Translator) buffer clear** after control endpoint stalls on
full-speed devices behind a high-speed hub, compounded by a fragile stall
recovery path that fails on VL805.

## Device Topology

```
xHCI Root Port 1 (USB3 port range)
 └─ USB 2.0 Hub (VID=0x2109, PID=0x3431) — HIGH-SPEED, slot 1
     ├─ Port 1: Logitech Mouse (VID=0x046d, PID=0xc08b) — FULL-SPEED, slot 2
     │   ├─ Interface 0: HID Mouse (class=0x03, subclass=0x01, proto=0x02)
     │   │   └─ EP1 IN (DCI 3, interrupt, 8 bytes)
     │   └─ Interface 1: HID Keyboard (class=0x03, subclass=0x00, proto=0x00)
     │       └─ EP2 IN (DCI 5, interrupt, 20 bytes)
     ├─ Port 2: (empty)
     ├─ Port 3: Keyboard or other device — FULL-SPEED, slot 3 (fails)
     └─ Port 4: Unknown device — FULL-SPEED, slot 3 reused (fails)
```

The hub is a high-speed hub with a **single TT** (`mtt=false` hardcoded in
Stellux). All full-speed downstream devices share one Transaction Translator
for split transactions on the 480 Mb/s link.

## Failure Sequence

### Phase 1: Mouse Enumerates Successfully
The mouse on hub port 1 enumerates as slot 2 without issues. Both interfaces
are discovered, endpoints configured, and HID drivers matched.

### Phase 2: SET_IDLE Stalls (Expected, Benign)
The HID driver issues `SET_IDLE` to interface 1 (the keyboard interface on the
composite mouse device). The mouse responds with **STALL** — this is **normal
and expected** behavior. Many HID devices do not support `SET_IDLE` on
non-boot-protocol interfaces, and the USB HID spec treats it as optional.
Linux's `usbhid` driver ignores `SET_IDLE` failures in its probe path.

### Phase 3: Stall Recovery Fails (The First Bug)
After the STALL, the xHCI driver attempts to recover EP0:

1. **Reset Endpoint** command for DCI 1 (EP0) — this may fail
2. **Set TR Dequeue Pointer** command — this fails with TRB_ERROR

The recovery failure leaves EP0 of slot 2 in **Halted** state (or an
inconsistent state where the dequeue pointer was not advanced past the failed
TD).

**Why recovery fails:** Two contributing factors:

- **VL805 controller quirk:** The VL805 may reject Reset Endpoint or Set TR
  Dequeue Pointer with TRB_ERROR under certain conditions involving devices
  behind hubs (TT split transactions). The existing Link TRB dequeue quirk
  addresses one known issue but may not cover all VL805 edge cases.

- **No TT buffer clear before recovery commands:** After the STALL on EP0 of
  a full-speed device behind a high-speed hub, the hub's TT may still hold
  state from the split transaction. The xHCI Reset Endpoint and Set TR Dequeue
  commands operate on the **host controller's** internal state, but if the
  **hub's TT** is confused about the endpoint state, split transaction
  scheduling for that address/endpoint can remain broken.

### Phase 4: Keyboard Enumeration Fails (The Cascade)
The hub driver continues its initial port scan and tries to enumerate the
keyboard on hub port 3 (and later port 4). Both fail:

```
[INFO]  xhci: hub slot 1 port 3 -> slot 3 (route=0x00003, speed=1)
[WARN]  xhci: EP0 event slot 3 port 3 ... code=STALL_ERROR transferLen=8
[ERROR] xhci: failed to read device descriptor for slot 3
```

**Why the keyboard fails:** The most likely cause is **shared TT state
corruption**. On a single-TT hub:

1. The mouse's STALL during `SET_IDLE` leaves TT buffer state associated with
   the mouse's device address and EP0
2. Stellux does **not** issue `CLEAR_TT_BUFFER` after the stall — it only
   does so during enumeration descriptor read retries
3. The TT may remain in a confused state for **all** FS/LS traffic through
   that hub, because single-TT hubs serialize all split transactions through
   one TT engine
4. When the keyboard's `GET_DESCRIPTOR` is attempted, the split transaction
   through the same TT fails or produces a STALL-like response

### Phase 5: No Keyboard Driver Running
Since the keyboard device on hub port 3/4 never successfully enumerates, no
HID driver is ever started for it. The keyboard simply does not exist from the
OS's perspective, so key events are never delivered.

## Root Causes (Ranked)

### 1. Missing TT Buffer Clear After Control Endpoint Stall (Primary)

**Location:** `_recover_stalled_control_endpoint()` and `_send_control_transfer()`
/ `usb_control_transfer()` in `kernel/drivers/usb/xhci/xhci.cpp`

**Problem:** After a control transfer STALL on a FS/LS device behind a HS hub,
the driver recovers the xHCI endpoint state (Reset Endpoint + Set TR Dequeue)
but does **not** clear the hub's TT buffer. The `_clear_tt_buffer()` function
exists and is correctly implemented (clearing both IN and OUT directions per
USB 2.0 §11.24.2.3), but is only called from enumeration descriptor read
retry loops.

**Evidence:**
- `_clear_tt_buffer` call sites: only in `_enumerate_device` retry loops
- No `_clear_tt_buffer` call in `_recover_stalled_control_endpoint`
- No `_clear_tt_buffer` call after STALL_ERROR in `_send_control_transfer`
  or `usb_control_transfer`
- The `ENUM_FAIL` macro issues `RESET_TT` but only **after** enumeration
  has already failed — too late to prevent the cascade

**Linux comparison:** Linux's `finish_td()` in `xhci-ring.c` calls
`xhci_clear_hub_tt_buffer()` for halted endpoints behind hubs, with an
explicit exception for **EP0 + STALL_ERROR** (protocol stall). However, Linux
has broader TT recovery infrastructure that Stellux lacks, and the overall
hub/TT error handling is more mature.

**Fix direction:** After any control endpoint STALL on a FS/LS device behind a
HS hub, issue `_clear_tt_buffer()` for that device's EP0. This should be done
in `_recover_stalled_control_endpoint()` after the Reset Endpoint / Set TR
Dequeue sequence, using the device's current USB address from the output slot
context. For the strongest recovery, consider also issuing `RESET_TT` on the
hub if `_clear_tt_buffer` + retry still fails.

### 2. Fragile Stall Recovery on VL805 (Contributing)

**Location:** `_recover_stalled_control_endpoint()` in `xhci.cpp`

**Problem:** The Reset Endpoint + Set TR Dequeue Pointer sequence fails on VL805
with TRB_ERROR. According to the xHCI spec:
- Reset Endpoint requires the endpoint to be in **Halted** state; if not,
  it returns Context State Error (19)
- Set TR Dequeue Pointer requires the endpoint to be in **Stopped** or
  **Error** state; Reset Endpoint transitions Halted → Stopped
- TRB_ERROR (5) indicates a malformed command TRB or invalid parameters

The fact that TRB_ERROR (not Context State Error) is returned suggests either
a VL805-specific behavior or a parameter issue in the command TRB construction.

**Current mitigation:** The code already ignores Reset Endpoint failure and
proceeds with Set TR Dequeue Pointer (matching Linux's approach). It also has
a VL805 Link TRB quirk. However, when Set TR Dequeue also fails, EP0 is left
in an unrecoverable state.

**Fix direction:**
- After failed stall recovery, clear TT buffers (ties to root cause #1)
- Consider issuing Stop Endpoint before Reset Endpoint as some controllers
  handle this better
- Consider a fallback to Configure Endpoint with Drop+Add for EP0 if both
  Reset Endpoint and Set TR Dequeue fail
- As a last resort, issue a USB device reset (port reset via the hub) to
  fully recover the device

### 3. Single Global Command State (Latent Risk)

**Location:** `m_cmd_state` in `xhci.h`, `_send_command()` in `xhci.cpp`

**Problem:** The driver uses a single `xhci_cmd_state` for all command
completions. The completion handler accepts **any** Command Completion Event
while `m_cmd_state.pending` is true, without verifying that the completion's
`command_trb_pointer` matches the command that was actually submitted.

This creates several risks:
- If a command times out and its completion arrives later, the stale
  completion can be attributed to the **next** command
- Nested `_send_command` calls (possible via `_process_event_ring` →
  Port Status Change → `_setup_device` → `_send_command`) can corrupt
  the outer command's state
- Multiple Command Completion Events in a single event ring drain can
  overwrite each other

**Current mitigation:** The command ring is sequential (one outstanding command
at a time by design), and most event processing happens on a single HCD task
thread. However, the lack of completion matching is still fragile.

**Fix direction:** Verify `cce->command_trb_pointer` matches the physical
address of the enqueued command TRB before accepting the completion. This is a
defensive fix that prevents misattribution even under unusual event ordering.

## Log Message Version Note

The log trace provided contains messages (e.g., `EP0 event slot %u port %u`,
`submit/hcd`, `complete/hcd`, `command XHCI_TRB_TYPE_TRANSFER_EVENT failed`)
that do not appear in the current source tree at commit `71634cb`. The binary
running on the RPi4 appears to be from a different revision or contains
uncommitted changes. The analysis above is based on the current source tree's
logic, which implements the same fundamental recovery path; the log format
differences do not affect the root cause conclusions.

## Recommended Fix (Minimal, Targeted)

The smallest change that addresses the primary failure mode:

1. **In `_recover_stalled_control_endpoint()`:** After the existing Reset
   Endpoint + Set TR Dequeue sequence (regardless of success/failure), call
   `_clear_tt_buffer(device)` if the device is FS/LS behind a HS hub. This
   ensures the hub's TT is cleaned up after any EP0 stall.

2. **In `_send_control_transfer()` and `usb_control_transfer()` STALL
   handling:** After calling `_recover_stalled_control_endpoint()`, also call
   `_clear_tt_buffer(device)` with the correct device address from the output
   slot context. This catches stalls that happen after enumeration (like
   SET_IDLE) where the device already has a USB address.

3. **SET_IDLE failure tolerance (already correct):** The HID driver's
   `apply_idle_policy()` already treats SET_IDLE failure as non-fatal. No
   change needed in the HID layer.

These three changes should resolve the keyboard enumeration failure because:
- The TT buffer clear unblocks the single-TT hub for other device traffic
- Even if the mouse's EP0 remains halted (recovery failed), the TT is clean
- Subsequent keyboard enumeration through the same hub can proceed normally

## References

- USB 2.0 Specification §8.5.3.4 — Control endpoint STALL semantics
- USB 2.0 Specification §11.24.2.3 — CLEAR_TT_BUFFER hub class request
- xHCI Specification §4.6.8 — Reset Endpoint command
- xHCI Specification §4.6.10 — Set TR Dequeue Pointer command
- xHCI Specification §6.4.5 Table 6-90 — TRB Completion Code definitions
- Linux kernel `drivers/usb/host/xhci-ring.c` — `finish_td()`, `xhci_handle_halted_endpoint()`, `xhci_clear_hub_tt_buffer()`
- Linux kernel `drivers/hid/usbhid/hid-core.c` — `hid_set_idle()` (ignores STALL)
