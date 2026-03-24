# XHCI USB Stack — Comprehensive Audit Findings

This document catalogs all issues identified through deep audit of the Stellux
xHCI driver, hub driver, HID driver, and comparison with the Linux kernel
implementation. Issues are categorized by severity and ordered by likely impact
on the reported symptom (keyboard stops working when mouse is also plugged in
via a USB 2.0 hub on Raspberry Pi 4).

## Critical Issues

### C1. Cross-Task `_send_command` Race (Data Race)

**Location:** `_send_command()`, `_process_event_ring()`, `usb_control_transfer()`

**Problem:** When a class driver task (e.g., the hub driver task) performs a
control transfer that STALLs, `_recover_stalled_control_endpoint` calls
`_send_command` from that class driver's task context. Simultaneously, the HCD
task may be in its `run()` loop calling `_process_event_ring`. Both tasks then
race on:
- `m_cmd_state` (pending/completed/result)
- The event ring dequeue pointer (both call `_process_event_ring`)
- `m_event_ring->finish_processing()` (concurrent ERDP writes)

There is no lock protecting any of these shared resources.

**Impact:** Command completion misattribution, corrupted event ring state,
missed or duplicated events. Can cause any subsequent xHCI operation to fail
unpredictably.

**Linux comparison:** Linux uses `xhci->lock` spinlock for command ring access
and event ring processing.

**Fix:** Route stall-recovery commands through the HCD task (post a work item
and wait), or add a global HCD lock around `_send_command` and
`_process_event_ring`.

---

### C2. Command Reentrancy via Port Status Change

**Location:** `_process_event_ring()` → `_setup_device()` → `_send_command()`

**Problem:** While `_send_command` waits for a command completion by calling
`_process_event_ring`, a `PORT_STATUS_CHANGE_EVENT` can trigger `_setup_device`,
which calls `_send_command` again (for Enable Slot). The inner `_send_command`
overwrites `m_cmd_state.pending`/`completed`, causing the outer waiter to
either receive the wrong completion result or time out.

**Impact:** Silent slot ID misattribution (wrong device gets configured),
5-second command timeouts during enumeration, cascading failures on subsequent
devices.

**Preconditions:** Any hot-plug event during an active command wait, or
multiple ports with devices during `_scan_ports()`.

**Linux comparison:** Linux processes events asynchronously via interrupt
handler and defers device setup to a separate work queue, avoiding reentrancy.

**Fix:** Defer `PORT_STATUS_CHANGE_EVENT` handling during `_send_command` (queue
PSC events and replay after the command completes), or validate
`cce->command_trb_pointer` against the submitted TRB before accepting a
completion.

---

### C3. Transient USB Errors Permanently Kill Interrupt Streams

**Location:** `_complete_interrupt_in_stream()` (xhci.cpp ~line 2033)

**Problem:** Any non-SUCCESS/SHORT_PACKET completion code on an interrupt
endpoint permanently deactivates the stream (`active=false`). This includes
transient errors like `USB_TRANSACTION_ERROR` (CRC errors from electrical
noise) and `BABBLE_DETECTED_ERROR`. Once deactivated, the HID driver's `run()`
loop exits and the device stops processing input.

**Impact:** A single transient USB error (common on real hardware, especially
with long cables or noisy environments) permanently kills keyboard/mouse input
with no recovery path.

**Linux comparison:** Linux has retry logic for transient errors on interrupt
endpoints and does not permanently disable the endpoint on a single error.

**Fix:** Implement error classification — retry transient errors (transaction
error, babble, missed service) with a counter and only deactivate after N
consecutive failures. Log all deactivations.

---

### C4. No Command Completion Matching

**Location:** `_process_event_ring()` CMD_COMPLETION_EVENT handler (line 612)

**Problem:** The handler stores any `CMD_COMPLETION_EVENT` into `m_cmd_state`
when `pending` is true, without verifying that `cce->command_trb_pointer`
matches the physical address of the command TRB that was submitted.

**Impact:** Combined with C1 or C2, this allows wrong completions to be
attributed to the wrong command. Even without reentrancy, a stale completion
from a timed-out command can be consumed by the next `_send_command` call.

**Fix:** Before setting `m_cmd_state.completed = true`, verify that
`cce->command_trb_pointer` matches the physical address of the enqueued command
TRB (which can be computed from the command ring's state).

---

## High-Severity Issues

### H1. VL805 Hub TT Babble / Split Transaction Timing

**Location:** `_send_control_transfer()` SOF boundary delay (line ~2643)

**Problem:** The VL805 generates babble during TT split transactions. Linux
has `XHCI_VLI_HUB_TT_QUIRK` which adds a 125µs delay before ALL transfer types
to FS/LS devices behind VL805. Stellux only has a shorter SOF-boundary delay
(up to 200µs) and only for **control** transfers, not bulk or interrupt.

**Impact:** Interrupt and bulk transfers to FS/LS devices behind hubs may hit
TT babble, causing transaction errors or the permanent stream deactivation
described in C3.

**Fix:** Apply the SOF-boundary/timing delay to all transfer types (not just
control) when the device is FS/LS behind a hub. Consider a broader 125µs
minimum inter-transfer delay matching Linux's quirk.

---

### H2. No Stall Recovery for Interrupt Endpoints

**Location:** `_complete_async_request()`, `_complete_interrupt_in_stream()`

**Problem:** When an interrupt endpoint STALLs, the async completion path
maps it to `transfer_status::stalled` but performs no endpoint recovery (Reset
Endpoint + Set TR Dequeue). The interrupt stream path just deactivates. The
endpoint is left halted with no way to resume.

**Impact:** If a device STALLs an interrupt endpoint for any reason, that
endpoint is permanently dead.

**Linux comparison:** Linux's `finish_td()` handles stalls on all endpoint
types, issuing Reset Endpoint and dequeue fixup.

**Fix:** Add stall recovery to the interrupt/bulk completion paths, mirroring
`_recover_stalled_control_endpoint` but adapted for non-control endpoints.

---

### H3. No USB Device Reset Escalation

**Location:** (missing entirely)

**Problem:** When EP0 stall recovery fails, or when multiple consecutive
control transfers fail, there is no escalation to a full USB device reset
(hub port reset + re-enumeration). The device is simply left in a broken
state.

**Linux comparison:** Linux has a 4-level escalation:
1. EP reset (Reset Endpoint + Set TR Dequeue)
2. CLEAR_FEATURE(ENDPOINT_HALT) to device
3. `usb_reset_device()` (hub port reset + re-enumerate)
4. Port power cycle

**Impact:** Any persistent device error leaves the device permanently broken
until the OS is rebooted or the device is physically unplugged and replugged.

**Fix:** After N consecutive EP0 failures (e.g., 3), issue a hub port reset
via `SET_PORT_FEATURE(PORT_RESET)` and re-enumerate the device.

---

### H4. Hub Event Queue Overflow

**Location:** `queue_hub_enumerate()`, `queue_hub_disconnect()` (xhci.cpp ~1126)

**Problem:** The hub event queue is 32 entries. If a hub with many ports
(or multiple hubs) queue events faster than the HCD task processes them, events
are silently dropped. No error is returned to the caller.

**Impact:** Devices connected to hub ports whose enumerate events are dropped
will never be enumerated. The system provides no indication that devices were
missed.

**Fix:** Either grow the queue, add a retry mechanism, or block the hub driver
task when the queue is full (requires careful design to avoid deadlock).

---

### H5. VL805 Broken Endpoint DCS (Cycle State)

**Location:** `_recover_stalled_control_endpoint()` (xhci.cpp ~968-970)

**Problem:** VL805 can return the wrong DCS (Dequeue Cycle State) in the
endpoint context after a stall. The current recovery reads the cycle state
from the software ring's producer state (`ring->get_cycle_bit()`), which
may not match what the hardware expects.

**Relevant Linux quirk:** `XHCI_EP_CTX_BROKEN_DCS` (PR#3066) — Linux reads
the DCS from the TRB at the dequeue pointer itself, not from the endpoint
context.

**Impact:** After stall recovery, the endpoint's cycle state is wrong, causing
the controller to see all TRBs as consumed (stale) and never process new ones.
EP0 appears to work but silently drops all subsequent transfers.

**Fix:** When computing the dequeue pointer for Set TR Dequeue, also read the
cycle bit from the TRB at the target address (if it has been written by
software) rather than relying solely on the ring's software state.

---

### H6. Silent Stream Deactivation (No Logging)

**Location:** `_complete_interrupt_in_stream()` (xhci.cpp ~2033-2042)

**Problem:** When an interrupt stream is deactivated due to an error, no log
message is emitted. The `dropped` counter for queue overflows is incremented
but never logged. This makes debugging impossible when keyboards/mice stop
working.

**Fix:** Add `log::warn` when a stream is deactivated, including the slot ID,
endpoint address, and completion code. Log dropped payloads at a throttled
rate.

---

## Medium-Severity Issues

### M1. No Command Ring Abort on Timeout

**Location:** `_send_command()` (xhci.cpp ~704)

**Problem:** After a command timeout, the driver returns -1 but does not issue
a command-abort via `CRCR.CA`. The controller may still be processing the
command, and the stale completion can corrupt the next command's result.

**Linux comparison:** Linux uses `xhci_abort_cmd_ring()` which writes CRCR
with the Command Abort bit.

**Fix:** On timeout, set `CRCR.CA`, wait for the Command Ring Stopped
completion event, then proceed.

---

### M2. Hub Port Enumeration Not Robust

**Location:** `_enumerate_device()` ENUM_FAIL path

**Problem:** Enumeration has only a single attempt (with 3 GET_DESCRIPTOR
retries within that attempt). No debouncing, no EP0 reinitialization between
retries, no port power cycling as a recovery mechanism.

**Linux comparison:** Linux has `SET_CONFIG_TRIES = 4`, alternates between
"new" and "old" enumeration schemes, reinitializes EP0 between attempts, and
power-cycles the port midway through retries.

**Fix:** Add an outer retry loop around the full enumeration sequence (Enable
Slot → Address Device → GET_DESCRIPTOR → Configure), with port reset between
retries.

---

### M3. `_clear_tt_buffer` Only Checks Immediate Parent

**Location:** `_clear_tt_buffer()` (xhci.cpp ~999)

**Problem:** The function checks if the immediate parent is a HS hub. For
multi-tier topologies (FS device → FS hub → HS hub), the immediate parent is
the FS hub, so the check fails and CLEAR_TT_BUFFER is never sent.

**Fix:** Walk up the parent chain to find the nearest HS hub ancestor, as
`_configure_ctrl_ep_input_context` already does for slot context TT fields.

---

### M4. VL805 SOF Boundary Delay Not Applied to LS Devices

**Location:** `_send_control_transfer()` (xhci.cpp ~2643)

**Problem:** The SOF boundary delay (`_read_mfindex() & 0x7 != 0`) is only
applied when `device->speed() == XHCI_USB_SPEED_FULL_SPEED`. Low-speed
devices behind hubs are equally susceptible to TT babble but don't get the
delay.

**Fix:** Apply the delay for both `FULL_SPEED` and `LOW_SPEED` when
`route_string() != 0`.

---

### M5. TOCTOU Race in Hub Port Scan

**Location:** `hub_driver::run()` initial port scan (hub_driver.cpp ~68-97)

**Problem:** Between `reset_port()` completing and the HCD processing the
`queue_hub_enumerate` event, the port state can change (device disconnects).
The `_setup_hub_device` diagnostic port status check is log-only and does not
abort on disconnection.

**Fix:** Check port status in `_setup_hub_device` before committing a slot,
and handle the case where the device is no longer present.

---

## Low-Severity Issues

### L1. `m_disconnected` Lacks Atomic Semantics

**Location:** `hub_driver.cpp` (`m_disconnected`), `hid_driver.cpp`
(`m_disconnected`)

**Problem:** These flags are set from one task and read from another with no
memory ordering guarantees. On weakly-ordered architectures (AArch64), the
store may not be visible to the polling task.

**Fix:** Use `volatile` or an atomic with appropriate ordering.

---

### L2. Interrupt Endpoint Payload Queue Drops Not Logged

**Location:** `_queue_interrupt_in_stream_payload()` dropped counter

**Problem:** The `dropped` counter is incremented when the 2-slot queue is
full, but the value is never read, logged, or exposed.

**Fix:** Log dropped payloads periodically or expose via a diagnostic
interface.

---

### L3. Event Ring Full Not Monitored

**Location:** `_process_event_ring()` / `on_interrupt()`

**Problem:** The driver does not check `USBSTS.ERF` (Event Ring Full) flag.
If the event ring overflows, events are lost with no indication.

**Fix:** Check ERF in the interrupt handler or event processing loop and log
a warning.

---

### L4. Missing Stop Endpoint Before Slot Disable on Teardown

**Location:** `_teardown_device()`, `_teardown_hub_device()`

**Problem:** Disable Slot is issued without first stopping active endpoints.
Per xHCI spec, this should work (Disable Slot implicitly stops everything),
but some controllers behave better with an explicit Stop Endpoint first.

**Fix:** Issue Stop Endpoint for all active endpoints before Disable Slot.

---

## Summary Table

| ID  | Severity | Area      | Summary                                           |
|-----|----------|-----------|----------------------------------------------------|
| C1  | Critical | Command   | Cross-task _send_command data race                  |
| C2  | Critical | Command   | Command reentrancy via Port Status Change           |
| C3  | Critical | Stream    | Transient errors permanently kill interrupt streams |
| C4  | Critical | Command   | No command completion TRB pointer matching           |
| H1  | High     | VL805     | TT babble timing quirk incomplete                   |
| H2  | High     | Recovery  | No stall recovery for interrupt endpoints           |
| H3  | High     | Recovery  | No USB device reset escalation path                 |
| H4  | High     | Hub       | Hub event queue can overflow silently                |
| H5  | High     | VL805     | Broken endpoint DCS after stall                     |
| H6  | High     | Stream    | Silent stream deactivation (no logging)             |
| M1  | Medium   | Command   | No command ring abort on timeout                    |
| M2  | Medium   | Enum      | Enumeration not robust (no outer retry)             |
| M3  | Medium   | TT        | _clear_tt_buffer only checks immediate parent       |
| M4  | Medium   | VL805     | SOF boundary delay not applied to LS devices        |
| M5  | Medium   | Hub       | TOCTOU race in hub port scan                        |
| L1  | Low      | Sync      | m_disconnected lacks atomic semantics               |
| L2  | Low      | Stream    | Payload queue drops not logged                      |
| L3  | Low      | Event     | Event ring full not monitored                       |
| L4  | Low      | Teardown  | Missing Stop Endpoint before Disable Slot           |
