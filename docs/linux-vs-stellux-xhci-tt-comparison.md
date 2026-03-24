# Linux vs Stellux: xHCI TT Management, Hub Enumeration, and Stall Recovery

## Overview

This document compares how the Linux kernel's USB/xHCI stack handles Transaction
Translator (TT) management, hub port enumeration, split transactions, and stall
recovery versus Stellux's implementation. The comparison identifies gaps in
Stellux and areas where Linux's more mature approach could inform improvements.

---

## 1. TT Buffer Clearing (`xhci_clear_hub_tt_buffer`)

### Linux Implementation

Linux clears TT buffers through a **two-layer** architecture:

**Layer 1 — xHCI HCD (`xhci-ring.c:xhci_clear_hub_tt_buffer`):**
Called from the transfer event handler (`finish_td`) whenever a halted endpoint
is detected. The function checks three conditions before clearing:
- The device has a TT (`td->urb->dev->tt` is set)
- The transfer is **not** an interrupt pipe (`!usb_pipeint(td->urb->pipe)`)
- The TT hub is not the root hub itself
- The endpoint is not already in `EP_CLEARING_TT` state (dedup flag)

**Critical detail — EP0 stall exception (line 2265):**
```c
if (!(ep->ep_index == 0 && trb_comp_code == COMP_STALL_ERROR))
    xhci_clear_hub_tt_buffer(xhci, td, ep);
```
Linux **skips** TT clearing for EP0 protocol stalls (STALL_ERROR on ep_index 0).
The rationale: EP0 protocol stalls are cleared by the next SETUP token per USB
spec §8.5.3.4, and the TT should handle this transparently. TT clearing is still
done for EP0 on other halt-inducing errors (babble, transaction error, etc.) and
for all non-EP0 functional stalls.

**Layer 2 — USB core hub driver (`hub.c:usb_hub_clear_tt_buffer`):**
This is the generic API called by HCDs. It queues a work item
(`hub_tt_work`) that sends the actual `CLEAR_TT_BUFFER` control request to
the hub. The work function also calls the HCD's `clear_tt_buffer_complete()`
callback so the HCD knows when it's safe to resume URBs to that endpoint. This
is an **asynchronous** mechanism — the HCD marks the endpoint with
`EP_CLEARING_TT` and holds off new URBs until the callback fires.

**When Linux clears TT buffers (comprehensive list):**
1. **Any non-EP0 bulk/control stall** on an FS/LS device behind an HS hub
2. **EP0 babble, transaction error, or other halt** (but NOT protocol stall)
3. **URB cancellation** (unlink) of a split transaction that failed
4. **Endpoint halt processing** in general for FS/LS behind HS hubs

### Stellux Implementation

Stellux clears TT buffers via `_clear_tt_buffer()` in `xhci.cpp`:

**When Stellux clears TT buffers:**
1. **After EP0 stall recovery** — `_recover_stalled_control_endpoint()` calls
   `_clear_tt_buffer()` after Reset Endpoint + Set TR Dequeue. This happens for
   ALL EP0 stalls including protocol stalls.
2. **During enumeration retries** — after a failed `GET_DESCRIPTOR` (first 8
   bytes or full descriptor), `_clear_tt_buffer()` is called before retrying
   (up to 3 attempts with 10ms delay between).
3. **ENUM_FAIL macro** — issues `RESET_TT` (request code 9, distinct from
   `CLEAR_TT_BUFFER` request code 8) as a last resort before slot teardown.

**Stellux's approach is synchronous** — `_clear_tt_buffer()` sends two control
transfers (IN and OUT direction) directly to the parent hub and blocks until they
complete. No dedup tracking, no callback mechanism.

### Key Differences

| Aspect | Linux | Stellux |
|--------|-------|---------|
| EP0 protocol stall | **Skips** TT clear (next SETUP clears it) | **Always** clears TT |
| Non-EP0 functional stall | Clears TT via `usb_hub_clear_tt_buffer` | Not applicable (only EP0 stalls currently handled) |
| Babble/transaction error | Clears TT | No specific handling |
| Mechanism | Async work queue + callback | Synchronous blocking control transfers |
| Dedup tracking | `EP_CLEARING_TT` flag prevents redundant clears | None |
| Interrupt endpoints | Excluded (`!usb_pipeint`) | Excluded (only EP0 stall path exists) |
| URB unlink/cancel | Clears TT | Not applicable (no URB abstraction) |

### Implications for Stellux

Stellux's **aggressive** TT clearing on EP0 protocol stalls is actually more
conservative than Linux (which skips it). For VL805, this extra clearing may
be **beneficial** — it's a belt-and-suspenders approach that compensates for
VL805 TT bugs. However, Stellux is missing TT clearing for:
- Non-EP0 endpoint stalls (will matter when class drivers use bulk/interrupt EPs)
- Babble and transaction errors
- Any form of dedup to prevent redundant TT clears

---

## 2. VL805 TT Quirks

### Linux Implementation

Linux has a specific **`XHCI_VLI_HUB_TT_QUIRK`** (Raspberry Pi kernel, PR #5262)
that addresses a hardware bug in the VL805's integrated hub TT. The quirk:

- Is set in the xHCI PCI quirks table for VL805 (VID/PID match)
- Adds a **spin-wait of up to 125 µs** in `xhci_urb_enqueue()` before ringing
  the doorbell for FS/LS devices behind the VL805's hub
- Waits until the current microframe position is "safe" (away from SOF boundary)
- The author (Jonathan Bell / P33M) noted: *"Spinning is unavoidable but not
  sufficient (the hardware can still cause babble by itself)"*
- Architecture-agnostic (not Pi-specific), affects all VL805 implementations

Additional VL805 quirks in Linux:
- **`XHCI_TRB_OVERFETCH`**: VL805 prefetches up to 4 TRBs beyond segment
  boundaries, causing stale cycle bits and IOMMU faults. Fix: allocate dummy
  pages after ring segments.
- **Link TRB quirk** (PR #3929): VL805 cannot handle TR Dequeue Pointer set to
  a Link TRB. Fix: advance past Link TRBs to the next segment start.

### Stellux Implementation

Stellux has **two** VL805-related workarounds:

1. **SOF boundary avoidance** in `_send_control_transfer()`:
   ```cpp
   // VL805 quirk: avoid ringing the doorbell near the SOF boundary
   if (device->route_string() != 0 &&
       device->speed() == XHCI_USB_SPEED_FULL_SPEED) {
       for (uint32_t tries = 0; tries < 20; tries++) {
           if ((_read_mfindex() & 0x7) != 0) break;
           delay::us(10);
       }
   }
   ```
   This spins up to 200 µs waiting for `MFINDEX & 0x7 != 0` (not at microframe 0).

2. **Link TRB dequeue pointer quirk** in `_recover_stalled_control_endpoint()`:
   If the dequeue pointer would land on a Link TRB, advance to the next segment
   start and toggle the cycle bit.

### Key Differences

| Aspect | Linux | Stellux |
|--------|-------|---------|
| SOF boundary avoidance | 125 µs max, in `xhci_urb_enqueue` for ALL transfer types | 200 µs max, only in `_send_control_transfer` |
| Scope | All URBs to FS/LS endpoints behind VL805 hub | Only control transfers |
| TRB overfetch | Allocates dummy pages after segments | Not addressed |
| Link TRB dequeue | Advances past Link TRB | Same approach |
| Quirk detection | PCI VID/PID match → quirk flag | Hardcoded behavior |

### Implications for Stellux

- Stellux's SOF boundary quirk only covers **control transfers**. Bulk and
  interrupt transfers also go through the TT and could trigger the same VL805
  babble bug. When Stellux adds bulk transfer support, this quirk should be
  extended.
- The TRB overfetch quirk is missing in Stellux. This could cause issues if
  transfer ring segments are adjacent to differently-owned memory.

---

## 3. Split Transaction Scheduling

### Linux Implementation

Linux's xHCI driver **does not** perform explicit start-split/complete-split
scheduling in software. The xHCI specification (unlike EHCI) delegates split
transaction management entirely to the xHC hardware and the TT. The xHCI driver
only needs to:
- Set the TT fields in the slot context (TT Hub Slot ID, TT Port Number)
- Set the `Max ESIT Payload` and `Average TRB Length` for periodic endpoints

For **EHCI** (older stack), Linux has explicit TT bandwidth tracking in
`ehci-sched.c`, but xHCI abstracts this away.

The VL805 TT quirk (`XHCI_VLI_HUB_TT_QUIRK`) is the closest Linux has to
software-managed split timing under xHCI — spinning to avoid SOF-boundary URB
submissions.

### Stellux Implementation

Stellux similarly relies on xHCI hardware for split transactions. The only
software intervention is the MFINDEX-based SOF boundary avoidance described
in §2, which avoids ringing the doorbell when `MFINDEX & 0x7 == 0` (microframe
0 of the current frame).

### Key Differences

Both Linux and Stellux delegate split transaction scheduling to the xHC. The
main difference is the scope of the SOF boundary workaround (all URBs in Linux
vs only control transfers in Stellux). Neither stack has a software-level TT
bandwidth scheduler under xHCI — this is by design per the xHCI spec.

---

## 4. Hub Port Power Management and Enumeration Ordering

### Linux Implementation

Linux's hub driver (`hub.c`) has sophisticated port enumeration with multiple
retry and recovery layers:

**Constants:**
- `PORT_RESET_TRIES = 5` — attempts per port reset
- `SET_ADDRESS_TRIES = 2` — SET_ADDRESS retry count
- `GET_DESCRIPTOR_TRIES = 2` — GET_DESCRIPTOR retry count
- `SET_CONFIG_TRIES = 2 * (use_both_schemes + 1)` — total enumeration attempts
  (typically 4, covering both "new" and "old" enumeration schemes)

**Enumeration flow (`hub_port_connect`):**
1. Debounce the connection (`hub_port_debounce_be_stable`)
2. Loop up to `SET_CONFIG_TRIES` times:
   a. Allocate a new `usb_device`
   b. Call `hub_port_init()` (port reset, GET_DESCRIPTOR, SET_ADDRESS)
   c. On failure, call `usb_ep0_reinit()` to reset EP0 state
   d. **Halfway through retries**: power-cycle the port (`set_port_power off →
      wait → set_port_power on`)
3. `hub_port_init` alternates between "new scheme" (GET_DESCRIPTOR before
   SET_ADDRESS) and "old scheme" (SET_ADDRESS first) across retries

**Power-on-good delay:**
Linux enforces `bPwrOn2PwrGood * 2ms` after powering on ports. For VL805, the
root hub's `bPwrOn2PwrGood` was increased from 10 (20ms) to 50 (100ms) to fix
USB 3.1 enumeration failures.

**Port ordering:**
Hub events are processed via `list_add_tail()` (FIFO order). Events from a hub's
interrupt endpoint are processed sequentially in `hub_event()` work function.
There is **no explicit inter-port delay** — ports are enumerated as their change
events arrive. Multiple hubs' events can interleave.

**Port status locking:**
`usb_lock_port()` prevents concurrent modifications during reset/enumeration.
`hub_port_init` holds the HCD lock (not just the bus lock) to prevent xHCI
spec violations with concurrent Device Slot state transitions.

### Stellux Implementation

Stellux's hub driver (`hub_driver.cpp`) scans ports sequentially:

```cpp
for (uint8_t port = 1; port <= m_hub_desc.bNbrPorts; port++) {
    // GET_STATUS, reset_port, queue_hub_enumerate...
}
```

- **Power-on-good delay**: `bPwrOn2PwrGood * 2000 + 50000 µs` (adds 50ms extra)
- **No debounce**: Reads port status once, no debounce loop
- **No inter-port delay**: Ports are scanned and queued back-to-back
- **No enumeration retries at port level**: Enumeration failures go to
  `ENUM_FAIL` (RESET_TT + teardown). The retry logic exists only inside
  `_enumerate_device()` for GET_DESCRIPTOR (3 attempts each for 8-byte and
  full descriptor reads)
- **No power cycling fallback**: No equivalent of Linux's halfway-through
  power-cycle recovery
- **No old/new scheme alternation**: Always uses "new scheme" (GET_DESCRIPTOR
  before SET_ADDRESS with Enable Slot → Address Device BSR=1 → GET_DESCRIPTOR
  → Address Device BSR=0)

### Key Differences

| Aspect | Linux | Stellux |
|--------|-------|---------|
| Total enumeration retries | 4 (SET_CONFIG_TRIES) | 1 (no port-level retry) |
| SET_ADDRESS retries | 2 per attempt | 1 (implicit in Address Device) |
| GET_DESCRIPTOR retries | 2 per attempt | 3 (with TT clear between) |
| Port debounce | Yes (100ms+ stable check) | No |
| Power cycle on failure | Yes (halfway through retries) | No |
| Scheme alternation | Old/new scheme toggle | New scheme only |
| Port locking | `usb_lock_port` + HCD lock | Per-device mutex (ctrl_transfer_mutex) |
| `usb_ep0_reinit` on retry | Yes (resets EP0 max_packet to 8/64) | No equivalent |

### Implications for Stellux

Stellux's single-attempt enumeration is fragile. Linux's approach of retrying
with increasingly aggressive recovery (scheme alternation → power cycling) is
significantly more robust. Key missing elements:
- **Connection debouncing** — devices can bounce during insertion
- **Port power cycling** — recovers devices in bad state
- **EP0 reinitialization** — Linux calls `usb_ep0_reinit()` which resets
  `ep0.desc.wMaxPacketSize` to 8 (LS) or 64 (other speeds), ensuring the
  next attempt starts fresh

---

## 5. EP0 Stall Recovery

### Linux Implementation

Linux handles EP0 stalls at two levels:

**xHCI HCD level (`xhci-ring.c`):**
In `process_ctrl_td()`, when `COMP_STALL_ERROR` is detected:
1. Update `urb->actual_length` based on which TRB stalled (SETUP/DATA/STATUS)
2. Call `xhci_handle_halted_endpoint()` with `EP_HARD_RESET`:
   a. Issue **Reset Endpoint** command
   b. Issue **Set TR Dequeue Pointer** to advance past the failed TD
3. The EP0 protocol stall is considered "cleared by next SETUP" per USB spec
4. TT buffer is **not** cleared for EP0 protocol stalls (see §1)

**USB core level (`hub.c`):**
- `usb_ep0_reinit()` resets the EP0 descriptor's `wMaxPacketSize` back to the
  default for the device speed (8 for LS, 64 for FS/HS/SS)
- Called during `usb_reset_and_verify_device()` and before each `hub_port_init`
  retry in `hub_port_connect`

**Critical: Linux does NOT:**
- Re-read `bMaxPacketSize0` during stall recovery
- Issue `Evaluate Context` during stall recovery
- Send `ClearFeature(ENDPOINT_HALT)` for EP0 — the next SETUP token clears it

**Linux DOES additionally:**
- Track `EP_HALTED` and `EP_CLEARING_TT` state flags to coordinate halted
  endpoint handling
- Ensure dequeue pointer is not restarted until Set TR Dequeue completes
  (fixes a race where the same stalled TD would replay)
- Handle the case where Reset Endpoint fails but Set TR Dequeue is still needed
  (the endpoint may not have been halted)

### Stellux Implementation

`_recover_stalled_control_endpoint()`:
1. Compute dequeue address from the ring's current enqueue pointer
2. **VL805 Link TRB quirk**: Skip past Link TRBs
3. **Reset Endpoint** — failure is logged but ignored (matches Linux)
4. **Set TR Dequeue Pointer** — failure returns -1
5. **`_clear_tt_buffer()`** — always done for FS/LS behind HS hub (differs from
   Linux, which skips this for EP0 protocol stalls)

**Not done:**
- No EP0 max packet size reset/reinit
- No `Evaluate Context` during recovery
- No state tracking (no `EP_HALTED` / `EP_CLEARING_TT` flags)
- No coordination to prevent doorbell ring before Set TR Dequeue completes

### Key Differences

| Aspect | Linux | Stellux |
|--------|-------|---------|
| Reset EP → Set Dequeue ordering | Enforced (ring not restarted until dequeue set) | Sequential but no interlock |
| TT clear on EP0 stall | Skipped (next SETUP clears it) | Always done |
| EP state tracking | `EP_HALTED`, `EP_CLEARING_TT` flags | None |
| EP0 reinit on retry | Yes (`usb_ep0_reinit`) | No |
| Max packet size re-eval | At enumeration retry, not stall recovery | At enumeration only |
| Error escalation | Eventually reaches `usb_reset_device` | `ENUM_FAIL` → teardown |

---

## 6. Concurrent Hub and Device Control Transfers

### Linux Implementation

Linux uses multiple locking layers:

**Per-device serialization:**
- `usb_lock_device()` (actually `device_lock` / `dev->mutex`) — held by the
  USB core around configuration changes, probing, etc. NOT held for individual
  control messages from class drivers.
- Class drivers' `usb_control_msg()` / `usb_control_msg_send()` are
  **synchronous** and can be called concurrently from different contexts for
  **different devices** without conflict.

**HCD submission:**
- `usb_submit_urb()` can be called from interrupt context
- `usb_hcd_submit_urb()` calls into the HCD's `urb_enqueue` method
- xHCI uses `xhci->lock` (spinlock) for command ring and event ring access
- URBs to different endpoints on different devices are **fully concurrent**
- URBs to the same endpoint are queued in FIFO order

**Bandwidth mutex:**
- `hcd->bandwidth_mutex` serializes bandwidth-affecting operations (config
  changes, alt-setting changes) but NOT ordinary control/bulk/interrupt URBs

**Key point:** Linux's hub driver and a device's class driver can submit control
URBs simultaneously without explicit coordination beyond the HCD's internal
spinlock. The hub driver talks to the hub device's EP0, and the class driver
talks to the downstream device's EP0 — these are different devices and different
endpoints, so they proceed independently through the xHC.

### Stellux Implementation

- **Per-device mutex:** `ctrl_transfer_mutex` on each `xhci_device` serializes
  EP0 access to that specific device
- **Two code paths:**
  - HCD task: uses `_send_control_transfer()` with direct event ring polling
  - Class driver tasks: uses `usb_control_transfer()` with wait queue
    (`ctrl_completion_wq`)
- Hub EP0 and downstream device EP0 use **different** `ctrl_transfer_mutex`
  instances, so they can proceed concurrently
- **Global `g_usb_core_lock`** protects device/driver tables, not EP0 traffic

### Key Differences

| Aspect | Linux | Stellux |
|--------|-------|---------|
| EP0 serialization | Per-device (implicit via URB queuing) | Per-device (explicit mutex) |
| HCD command ring lock | `xhci->lock` spinlock | Sequential command ring access in HCD task |
| Cross-device concurrency | Full (URBs are independent) | Hub and device can overlap |
| Async support | Yes (URB callback model) | Only for HCD task (class drivers block) |

The architectures are similar for cross-device concurrency but differ in
the async model. Linux's URB+callback approach is more flexible, while Stellux's
synchronous model is simpler but can block the class driver task.

---

## 7. USB Device Reset as Stall Recovery Fallback

### Linux Implementation

Linux has a **multi-level escalation** path when things go wrong:

**Level 1 — Endpoint stall recovery:**
`xhci_handle_halted_endpoint()` → Reset Endpoint + Set TR Dequeue

**Level 2 — Class driver `ClearFeature(ENDPOINT_HALT)`:**
Class driver calls `usb_clear_halt()` which sends a CLEAR_FEATURE request and
calls `usb_reset_ep()` to reset the host-side toggle.

**Level 3 — Device reset (`usb_reset_device`):**
If the class driver determines the device is non-responsive (e.g., too many
consecutive transfer errors), it calls `usb_reset_device()` which:
1. Issues a hub port reset (or warm reset for USB3)
2. Calls `hub_port_init()` to re-enumerate (GET_DESCRIPTOR, SET_ADDRESS)
3. Calls `usb_reset_and_verify_device()` to restore configuration
4. Notifies the class driver via `pre_reset()` / `post_reset()` callbacks

**Level 4 — Port power cycle:**
During `hub_port_connect()` retries, halfway through the retry count, Linux
power-cycles the port (power off → wait → power on) as a last resort.

**Where in the code:**
- `usb_reset_device()` in `hub.c` (~line 5000+)
- Called by class drivers (e.g., `usb-storage`, `usbhid`) when they detect
  persistent errors
- `usb_queue_reset_device()` for deferred reset from interrupt context

**Important:** The escalation is **class-driver-initiated**, not automatic from
the HCD. The xHCI HCD never autonomously triggers a port reset — it only reports
errors to the USB core, and the class driver decides whether to reset.

### Stellux Implementation

Stellux has **no escalation path** beyond EP0 stall recovery:

1. `_recover_stalled_control_endpoint()` — Reset EP + Set Dequeue + Clear TT
2. On failure: the control transfer returns error code to the caller
3. If this happens during enumeration: `ENUM_FAIL` → `RESET_TT` → teardown
4. If this happens during class driver operation: error is returned to the class
   driver, which currently has no reset/recovery logic

**No `usb_reset_device` equivalent exists.** There is no mechanism to:
- Issue a hub port reset for a downstream device
- Re-enumerate a device without tearing down its slot
- Power-cycle a port as recovery
- Notify class drivers about an impending reset

### Key Differences

| Aspect | Linux | Stellux |
|--------|-------|---------|
| Escalation levels | 4 (EP reset → clear halt → device reset → power cycle) | 1 (EP reset only) |
| Hub port reset for recovery | Yes (`usb_reset_device`) | No |
| Power cycling | Yes (during connect retries) | No |
| Class driver notification | `pre_reset()` / `post_reset()` callbacks | Not applicable |
| Who initiates reset | Class driver (based on error accumulation) | Nobody |
| Deferred reset | `usb_queue_reset_device()` from IRQ context | Not applicable |

### Implications for Stellux

This is the largest architectural gap. When a device enters a persistently bad
state (e.g., EP0 stall recovery fails, or a device stops responding), Stellux
has no way to recover short of tearing down the slot entirely. Adding a
`usb_reset_device` equivalent with hub port reset + re-enumeration would
significantly improve robustness.

---

## Summary of Recommended Stellux Improvements

### High Priority

1. **Add port-level enumeration retries** — at least 2-3 attempts with port
   reset between them, matching Linux's `SET_CONFIG_TRIES` loop
2. **Add connection debouncing** — verify stable connection before enumeration
3. **Add `usb_reset_device` equivalent** — hub port reset + re-enumerate as
   recovery path when class drivers hit persistent errors
4. **Extend SOF boundary quirk** — apply to bulk/interrupt transfers, not just
   control transfers

### Medium Priority

5. **Add EP0 reinit on retry** — reset max packet size to default before
   retrying enumeration
6. **Add power cycle recovery** — toggle port power as last-resort recovery
7. **Add EP state tracking** — `EP_HALTED` / `EP_CLEARING_TT` flags to
   prevent doorbell rings during recovery and avoid redundant TT clears

### Low Priority (Architectural)

8. **Add TT clear for non-EP0 stalls** — needed when bulk/interrupt class
   drivers are added
9. **Add TRB overfetch quirk** — allocate guard pages after ring segments
10. **Add async control transfer model** — URB+callback for class drivers
    (current synchronous model is adequate for now)

---

## References

- Linux `xhci-ring.c`: `xhci_clear_hub_tt_buffer()`, `finish_td()`,
  `process_ctrl_td()`, `xhci_handle_halted_endpoint()`
- Linux `hub.c`: `hub_port_connect()`, `hub_port_init()`,
  `usb_hub_clear_tt_buffer()`, `usb_reset_and_verify_device()`
- Linux `xhci.c`: `xhci_endpoint_reset()`, `XHCI_VLI_HUB_TT_QUIRK`
- Raspberry Pi kernel PR #5262: VL805 TT quirk
- Raspberry Pi kernel PR #3929: VL805 Link TRB quirk
- Stellux `kernel/drivers/usb/xhci/xhci.cpp`: `_clear_tt_buffer()`,
  `_recover_stalled_control_endpoint()`, `_enumerate_device()`,
  `_send_control_transfer()`
- Stellux `kernel/drivers/usb/hub/hub_driver.cpp`: `run()`, `reset_port()`
- USB 2.0 Specification §8.5.3.4 (EP0 stall clearing), §11.17.5 (TT halt),
  §11.24.2.3 (CLEAR_TT_BUFFER)
