# HID Driver Probe Path Audit — Composite USB Devices

## Scope

This audit examines the HID driver probe path (`hid_driver::probe()` in
`kernel/drivers/usb/hid/hid_driver.cpp`) and the interface enumeration loop
(`device_configured()` in `kernel/drivers/usb/core/usb_core.cpp`) for
correctness when binding a composite USB device with multiple HID interfaces.

**Reference device:** Logitech mouse (VID=0x046d, PID=0xc08b)
- Interface 0: HID Mouse — class=0x03, subclass=0x01 (boot), protocol=0x02
  - EP1 IN (interrupt, 8 bytes)
- Interface 1: HID Keyboard — class=0x03, subclass=0x00 (non-boot), protocol=0x00
  - EP2 IN (interrupt, 20 bytes)

---

## 1. Probe Ordering for Composite Devices

### How it works

`device_configured()` (usb_core.cpp:246–288) iterates interfaces 0..N
sequentially. For each interface it finds a matching class driver, calls
`probe()` **synchronously**, and only then spawns the `run()` task. The entire
loop runs on the HCD task inside `RUN_ELEVATED`.

Because `probe()` is called directly (not in the spawned task), both probes
execute **serially on the same thread** before either `run()` task begins.

### Execution trace for the Logitech mouse

```
── Interface 0 probe (mouse, subclass=0x01, proto=0x02) ──
  1. find_interrupt_in_endpoint()    → EP 0x81
  2. GET_REPORT_DESCRIPTOR via EP0   → succeeds
  3. parse_report_descriptor          → succeeds
  4. create_handlers()                → mouse binding (X/Y axes detected)
  5. SET_PROTOCOL(REPORT) on iface 0  → succeeds (boot protocol interface)
  6. apply_idle_policy()              → no keyboard bindings → no SET_IDLE sent
  7. open_interrupt_in_stream(EP 0x81)→ succeeds
  8. probe returns 0
  9. task spawned, run() queued

── Interface 1 probe (keyboard, subclass=0x00, proto=0x00) ──
  1. find_interrupt_in_endpoint()     → EP 0x82
  2. GET_REPORT_DESCRIPTOR via EP0    → succeeds (EP0 is still healthy)
  3. parse_report_descriptor           → succeeds
  4. create_handlers()                 → keyboard binding (keyboard usage page)
  5. subclass=0x00 → SET_PROTOCOL NOT called (correct)
  6. apply_idle_policy()               → keyboard binding exists →
       SET_IDLE(Duration=0, ReportID=N) → device STALLs
       _recover_stalled_control_endpoint() → may fail (VL805)
       SET_IDLE failure logged as warning → non-fatal
  7. max_input_report_bytes()          → succeeds
  8. open_interrupt_in_stream(EP 0x82) → succeeds (does not use EP0)
  9. probe returns 0
 10. task spawned, run() queued
```

### Assessment

**No correctness issue in probe ordering.** The sequential execution ensures
interface 0's probe is fully complete before interface 1's probe begins. EP0
serialization via `ctrl_transfer_mutex` prevents interleaving, though it is
moot here since both probes run on the same thread.

**Risk:** If a future change runs probes concurrently (e.g., from separate
tasks), EP0 serialization via the mutex is already in place, but both probes
would share the same `ctrl_transfer_buffer` and `ctrl_result` state. The
current code is safe only because probes are serial.

---

## 2. SET_PROTOCOL Handling

### Current behavior

```cpp
// hid_driver.cpp:135-157
if (iface->interface_subclass == 0x01) {
    // Boot-protocol interface → send SET_PROTOCOL(REPORT)
    // Fail probe if SET_PROTOCOL fails
} else if (iface->interface_subclass != 0x00) {
    // Unknown subclass → reject
}
// subclass=0x00 → no SET_PROTOCOL, continue
```

### HID spec analysis

Per HID 1.11 §7.2.6:
- SET_PROTOCOL is **only** supported by boot-protocol interfaces (subclass=0x01)
- Non-boot interfaces (subclass=0x00) do not implement the boot protocol and
  MUST NOT receive SET_PROTOCOL

**Verdict: Correct.** The driver correctly gates SET_PROTOCOL on
`subclass==0x01` and skips it for `subclass==0x00`. Sending SET_PROTOCOL to a
non-boot interface would likely produce a STALL (the device doesn't recognize
the request), so not sending it is the right behavior.

---

## 3. Report Descriptor Fetch and EP0 Stall Ordering

### The concern

If a prior interface's probe leaves EP0 halted, can subsequent
GET_REPORT_DESCRIPTOR calls succeed?

### Analysis

The probe sequence is:

1. GET_REPORT_DESCRIPTOR (control IN on EP0)
2. parse + create_handlers
3. SET_PROTOCOL (control OUT on EP0, only if subclass=0x01)
4. apply_idle_policy → SET_IDLE (control OUT on EP0, only if keyboard bindings)
5. open_interrupt_in_stream (no EP0 involvement)

For the Logitech mouse:
- **Interface 0 probe:** SET_PROTOCOL succeeds, no SET_IDLE sent → EP0 is fine
  when interface 0's probe ends.
- **Interface 1 probe:** GET_REPORT_DESCRIPTOR runs *before* SET_IDLE →
  EP0 is still healthy → descriptor fetch succeeds. SET_IDLE comes later and
  may STALL, but the report descriptor is already parsed.

**Verdict: No issue.** The GET_REPORT_DESCRIPTOR for each interface executes
while EP0 is in a good state. The dangerous SET_IDLE call happens *after* all
EP0 reads for that interface's probe are complete.

### Edge case: What if SET_PROTOCOL on interface 0 STALLed?

If interface 0's SET_PROTOCOL STALLed and recovery failed, EP0 would be halted
when interface 1's probe starts. The GET_REPORT_DESCRIPTOR for interface 1
would then fail, and interface 1's probe would return -1.

Per USB 2.0 §8.5.3.4, a SETUP transaction clears a protocol stall at the USB
level. However, if `_recover_stalled_control_endpoint()` also failed at the
xHCI level (leaving the endpoint state as Halted in the xHC), subsequent
control transfers would fail because the xHC would not accept new TRBs on a
Halted endpoint without a successful Reset Endpoint + Set TR Dequeue sequence.

**Impact:** If SET_PROTOCOL STALLs and xHCI recovery fails, the second
interface will not bind. The probe returns -1, the driver is deleted, and
only interface 0 works. For the Logitech mouse this would mean only the mouse
interface binds, not the keyboard interface. However, SET_PROTOCOL on a boot
mouse interface should not normally STALL.

---

## 4. Shared EP0 Across Multiple HID Interfaces

### Current state

Each interface gets its own `hid_driver` instance, its own task, its own
interrupt endpoint, and its own interrupt stream. But all interfaces on the
same USB device share EP0 for control transfers.

### Post-probe control transfers

The `run()` method (hid_driver.cpp:181–238) **only reads from the interrupt
stream**. It does not issue any control transfers. Therefore:

- If interface 1's SET_IDLE STALL leaves EP0 halted after recovery failure,
  this does **not** affect the running HID drivers because they never touch
  EP0 again.
- Interface 0's interrupt stream on EP 0x81 is unaffected.
- Interface 1's interrupt stream on EP 0x82 is unaffected.

**Verdict: Currently safe, but fragile.**

### Future risk

If the HID driver is extended to support:
- **Keyboard LED updates** via SET_REPORT (requires EP0 OUT control transfer)
- **Feature report reads** via GET_REPORT (requires EP0 IN control transfer)
- **Runtime SET_IDLE changes** for power management

...then a halted EP0 from a prior probe failure would cause these control
transfers to fail silently. The `ctrl_transfer_mutex` serializes access, but
a Halted xHCI endpoint state would reject all subsequent TRBs.

**Recommendation:** If keyboard LED support is planned, the driver should
either:
1. Verify EP0 is functional before attempting post-probe control transfers
2. Implement EP0 recovery (Reset Endpoint + Set TR Dequeue) before retrying
3. Track EP0 health state on the `usb::device` and refuse control transfers
   when EP0 is known-halted

---

## 5. Report Classification and Handler Binding

### Classification logic

```cpp
// hid_driver.cpp:25-61
classify_report() checks for:
  mouse:    has X-axis AND Y-axis fields (generic_desktop usage page)
  keyboard: any non-constant field on keyboard usage page (0x07),
            UNLESS it is a variable field with modifier usage (0xE0–0xE7)
```

### Analysis for composite devices

For the Logitech mouse, each interface has **its own report descriptor**
fetched independently via GET_REPORT_DESCRIPTOR with the interface number as
wIndex. The descriptors describe different functionality:

- Interface 0 descriptor: mouse pointer, X/Y axes, buttons → classified as
  **mouse**
- Interface 1 descriptor: keyboard keys, maybe consumer keys → classified as
  **keyboard**

There is no cross-contamination because each HID driver instance parses only
its own interface's report descriptor.

### Potential issue: Reports with both mouse and keyboard fields

If a single report descriptor contains reports with BOTH X/Y axes and keyboard
usage page fields, `classify_report()` would set both `caps.mouse` and
`caps.keyboard`, and `create_handlers()` (line 319–341) would create **both**
a mouse handler and a keyboard handler bound to the same report ID.

When that report arrives, `dispatch_report()` (line 382–388) iterates all
bindings and calls `on_report()` on every handler with a matching report ID.
Both handlers would process the same raw data.

**This is correct behavior** for reports that genuinely contain both mouse and
keyboard data (e.g., a composite report with axes and keys). Each handler
extracts only the fields it cares about from the report body using the parsed
field offsets.

### Modifier key exclusion

The check at line 53-56:
```cpp
if (field.usage_page == kb &&
    (!field.is_variable() || !is_keyboard_modifier_usage(field.usage))) {
    caps.keyboard = true;
}
```

This means:
- Array fields on keyboard page → keyboard (standard key array)
- Variable fields on keyboard page with non-modifier usage → keyboard
- Variable fields on keyboard page with modifier usage (Shift/Ctrl/Alt/GUI)
  → **not counted** as keyboard

**Potential issue:** A report containing **only** modifier key fields (no key
array) would not be classified as a keyboard. This could happen with unusual
HID descriptors. In practice, standard keyboard reports always have a key
array alongside the modifier bitmap, so this is unlikely to be a problem.

However, some gaming keyboards or macro pads might have reports with only
modifier fields. Consider changing the logic to:

```cpp
// Count modifier variables as keyboard too
if (field.usage_page == kb && !field.is_constant()) {
    caps.keyboard = true;
}
```

**Severity: Low.** Standard keyboards are correctly handled.

---

## 6. SET_IDLE Behavior and the STALL Cascade

### What happens

`apply_idle_policy()` (hid_driver.cpp:348–379) sends SET_IDLE to every keyboard
binding's report ID. For the Logitech mouse, interface 1 has a keyboard binding,
so SET_IDLE is sent.

The SET_IDLE request:
```
bmRequestType: 0x21 (OUT, Class, Interface)
bRequest: 0x0A (SET_IDLE)
wValue: 0x00RR (Duration=0, ReportID=RR)
wIndex: interface_number
```

Many HID devices — especially non-boot-protocol interfaces on composite
devices — do not support SET_IDLE and respond with STALL. Per HID 1.11 §7.2.4,
SET_IDLE is optional, and a STALL response is valid.

### The cascade (documented in xhci-usb-enumeration-stalls-analysis.md)

1. SET_IDLE STALLs on EP0
2. `_recover_stalled_control_endpoint()` attempts Reset Endpoint + Set TR
   Dequeue → fails on VL805
3. EP0 left in Halted state
4. TT buffer not cleared → single-TT hub blocks all other FS/LS traffic
5. Other devices on the same hub fail to enumerate

**The HID driver's SET_IDLE failure tolerance is correct** (it logs a warning
and continues). The problem is downstream in the xHCI stall recovery and TT
buffer management.

### Improvement opportunity

The driver could avoid the STALL entirely by not sending SET_IDLE to interfaces
that are unlikely to support it. Heuristics:

1. Skip SET_IDLE for non-boot-protocol interfaces (subclass=0x00) unless the
   device is known to support it
2. Skip SET_IDLE for composite devices where the keyboard interface is a
   secondary function

However, this would be a workaround for the real problem (broken xHCI stall
recovery). The fix should be in the xHCI layer, not the HID layer.

---

## 7. Concurrency and Lifetime Concerns

### Probe runs under RUN_ELEVATED

The entire `device_configured()` body, including all probe calls, runs inside
`RUN_ELEVATED`. This means:
- Probes execute with kernel-level privileges
- The `ctrl_transfer_mutex` lock/unlock within `_send_control_transfer()` also
  runs inside `RUN_ELEVATED`
- This is correct; `RUN_ELEVATED` is re-entrant

### active_driver_count

After a successful probe, `device_configured()` increments
`dev->active_driver_count` under `dev->lifetime_lock` before enqueueing the
task (line 276–278). When the task exits, `class_driver_task_entry` decrements
the count (line 156–158).

For two interfaces:
1. Interface 0 probe succeeds → active_driver_count = 1
2. Interface 1 probe succeeds → active_driver_count = 2
3. Both tasks run concurrently
4. On disconnect, both tasks exit and decrement the count
5. When count reaches 0 and `hcd_teardown_complete` is set, the device is
   finalized

**This is correct.** There is no race between the increment (single-threaded in
`device_configured`) and the decrement (per-task in `class_driver_task_entry`)
because the increment happens before the task is enqueued.

### Driver binding table

`publish_bound_driver()` uses `slot_id * 16 + interface_index` as the key.
For two interfaces on the same device:
- Interface 0 → drv_idx = slot_id * 16 + 0
- Interface 1 → drv_idx = slot_id * 16 + 1

These are distinct indices, so there is no collision. The factor of 16 limits
the maximum number of interfaces per device to 16, which is well within USB
limits (max 32 interfaces per configuration, but 16 is typical for xHCI
hardware).

**No issue found.**

---

## 8. Summary of Findings

| # | Area | Severity | Status |
|---|------|----------|--------|
| 1 | Probe ordering (sequential, correct) | — | OK |
| 2 | SET_PROTOCOL gated on subclass=0x01 | — | OK |
| 3 | GET_REPORT_DESCRIPTOR before SET_IDLE | — | OK |
| 4 | Halted EP0 doesn't affect interrupt streams | — | OK (currently) |
| 5 | Shared EP0 blocks future control transfers | Medium | Risk if LED/feature support added |
| 6 | classify_report excludes modifier-only reports | Low | Edge case |
| 7 | SET_IDLE STALL → TT corruption cascade | High | Root cause is in xHCI layer |
| 8 | active_driver_count and lifetime management | — | OK |
| 9 | Driver binding table, no collision | — | OK |

### Confirmed non-issues

- **Interrupt stream after SET_IDLE STALL:** Opening the interrupt stream does
  not use EP0. The `open_interrupt_in_stream()` call configures a transfer ring
  on the interrupt endpoint (DCI 3 or 5) and queues Normal TRBs. The halted
  EP0 (DCI 1) has no effect on other endpoints.

- **SET_PROTOCOL on non-boot interface:** Correctly skipped. The HID spec
  does not require SET_PROTOCOL for subclass=0x00 interfaces.

- **Report descriptor parsing per-interface:** Each HID driver instance fetches
  and parses the report descriptor for its own interface only. There is no
  cross-interface contamination of parsed layouts.

### Items requiring attention

1. **EP0 recovery after SET_IDLE STALL (xHCI layer):** Already tracked in
   `xhci-usb-enumeration-stalls-analysis.md`. The fix (TT buffer clear in
   `_recover_stalled_control_endpoint()`) should be implemented in the xHCI
   driver, not the HID driver.

2. **Future-proofing for post-probe control transfers:** If keyboard LED
   support is added, the driver must handle the case where EP0 was left halted
   by a prior SET_IDLE STALL. Consider adding an EP0 health check or recovery
   step before post-probe control transfers.

3. **Modifier-only keyboard reports:** The current classification logic would
   miss a keyboard report that contains only modifier key fields (no key
   array). This is an unlikely edge case with standard keyboards but could
   occur with specialty input devices.

---

## Appendix: Relevant Code Paths

- `usb::core::device_configured()` — usb_core.cpp:229–291
- `usb::hid::hid_driver::probe()` — hid_driver.cpp:79–178
- `usb::hid::hid_driver::run()` — hid_driver.cpp:181–238
- `usb::hid::hid_driver::apply_idle_policy()` — hid_driver.cpp:348–379
- `usb::hid::classify_report()` — hid_driver.cpp:25–61
- `xhci_hcd::usb_control_transfer()` — xhci.cpp:2825–2943
- `xhci_hcd::_send_control_transfer()` — xhci.cpp:2587–2707
- `xhci_hcd::_recover_stalled_control_endpoint()` — xhci.cpp:955–1013
