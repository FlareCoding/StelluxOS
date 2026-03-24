# xHCI Interrupt-In Stream Audit Report

## Executive Summary

The interrupt-in stream implementation is generally well-structured with correct
synchronization for the close and disconnect races. However, there are several
issues that can cause keyboard events to stop being delivered, the most impactful
being that **transient USB errors permanently kill the stream with no recovery
or retry logic**.

---

## 1. Stream Deactivation on Error

### Finding: Transient errors permanently kill the stream (CRITICAL)

**Location:** `_complete_interrupt_in_stream`, `xhci.cpp:2033-2042`

```
if (event->completion_code != XHCI_TRB_COMPLETION_CODE_SUCCESS &&
    event->completion_code != XHCI_TRB_COMPLETION_CODE_SHORT_PACKET) {
    ...
    state.interrupt_in_stream.active = false;
    ...
}
```

Any completion code other than `SUCCESS` (1) or `SHORT_PACKET` (13) permanently
deactivates the stream. Once `active` is set to `false`, `usb_read_interrupt_in_stream`
(line 2375) exits its wait loop and returns `-1`, causing the HID driver's `run()`
loop (hid_driver.cpp:202-207) to break and exit. **The stream is never restarted.**

#### Completion codes that can trigger this on an interrupt endpoint:

| Code | Name | Cause | Recoverable? |
|------|------|-------|--------------|
| 4 | `USB_TRANSACTION_ERROR` | CRC error, bit-stuff error, timeout on wire | **Yes** — transient bus glitch |
| 6 | `STALL_ERROR` | Device protocol error or NAK overflow | **Yes** — clear halt + retry |
| 3 | `BABBLE_DETECTED_ERROR` | Device sends more data than MPS | Possibly |
| 2 | `DATA_BUFFER_ERROR` | TD buffer too small for data | Possibly |
| 26 | `STOPPED` | Stop Endpoint command issued | Expected during close |
| 27 | `STOPPED_LENGTH_INVALID` | Stop Endpoint variant | Expected during close |
| 28 | `STOPPED_SHORT_PACKET` | Stop Endpoint variant | Expected during close |

**Impact:** A single CRC error on the USB bus (e.g., from electrical noise, a
loose connector, or EMI) causes `USB_TRANSACTION_ERROR` after the xHC exhausts
its internal retries (typically 3). This permanently kills the keyboard stream.
The user must physically unplug and replug the keyboard.

#### Recommendation:
- For `USB_TRANSACTION_ERROR`: re-queue the TD and continue. Log a warning
  but only deactivate after N consecutive errors.
- For `STALL_ERROR`: issue `CLEAR_FEATURE(ENDPOINT_HALT)` via EP0, then
  `_reset_endpoint` + `_set_tr_dequeue_ptr`, re-queue the TD, and continue.
  Control endpoint stall recovery already exists at `xhci.cpp:955-993` and
  should be adapted for interrupt endpoints.
- For `STOPPED`/`STOPPED_*`: These are expected during `usb_close_interrupt_in_stream`
  and are already handled by the `closing` flag (line 2019). However, a STOPPED
  event arriving when `closing` is NOT set still kills the stream. This could
  happen if the OS issues a stop command for other reasons (e.g., bandwidth
  reconfiguration).

### Finding: No error logging on stream deactivation (MODERATE)

**Location:** `_complete_interrupt_in_stream`, `xhci.cpp:2033-2042`

When a non-success completion code kills the stream, there is **no log message**
at all. The only visible output is "hid: interrupt stream read failed (-1)"
from hid_driver.cpp:206, which gives no indication of the root cause.

**Recommendation:** Add a `log::warn` with the completion code before deactivating:
```
log::warn("xhci: interrupt stream killed by completion code %u (%s) on slot %u EP%u",
          event->completion_code,
          trb_completion_code_to_string(event->completion_code),
          device->slot_id(), ep->endpoint_num());
```

### Finding: EP0 stall does NOT affect the interrupt endpoint — CONFIRMED SAFE

Each endpoint has its own DCI (Device Context Index). Transfer events reference
the specific endpoint via `e->endpoint_id` (line 625). EP0 (DCI=1) is dispatched
at line 637-640, while interrupt endpoints (DCI >= 2) are dispatched at line
642-654. A stall on EP0 only generates a Transfer Event with `endpoint_id=1`,
which cannot be confused with the interrupt endpoint.

### Finding: Ring Underrun cannot occur on interrupt endpoints — CONFIRMED SAFE

Per xHCI Specification Section 4.10.3.1, Ring Underrun (`RING_UNDERRUN`, code 14)
is only generated for **Isochronous IN** endpoints. For interrupt endpoints, if
no TD is queued, the xHC simply does not schedule an IN transaction. No error
event is generated. When a new TD is later queued and the doorbell rung, the
xHC resumes scheduling normally.

---

## 2. Queue Depth and Payload Drops

### Finding: Queue overflow silently drops oldest payload (LOW)

**Location:** `_queue_interrupt_in_stream_payload`, `xhci.cpp:1989-1993`

```
if (stream.count >= stream.queue_depth) {
    stream.head = static_cast<uint8_t>((stream.head + 1) % stream.queue_depth);
    stream.count--;
    stream.dropped++;
}
```

With `STREAM_QUEUE_DEPTH = 2` (line 2298), when both slots are full, the oldest
payload is overwritten and `dropped` is incremented. The `dropped` counter is
**never read or logged** anywhere in the codebase.

**Impact:** For HID keyboards, this is partially mitigated because each report
contains the full set of currently-pressed keys (not deltas). A dropped
intermediate state is mostly harmless. However, if a key-release report is
dropped, the keyboard handler may see a key as "stuck" until the next report
arrives.

For HID mice, dropped reports mean lost position deltas, causing cursor jumps
or missed button clicks.

**Recommendation:**
- Periodically log or expose the `dropped` counter for diagnostics.
- Consider increasing `STREAM_QUEUE_DEPTH` to 4 or 8 for more headroom.

---

## 3. DMA Buffer Sharing

### Finding: DMA buffer race does NOT exist — CONFIRMED SAFE

**Location:** `_complete_interrupt_in_stream`, `xhci.cpp:2052-2067`

The sequence of operations is:

1. **Line 2052:** `barrier::dma_read()` — ensures hardware DMA write is visible to CPU
2. **Lines 2053-2064:** Under spinlock, `memcpy` from `ep->dma_buffer()` into the
   payload queue via `_queue_interrupt_in_stream_payload`
3. **Line 2067:** `_queue_interrupt_in_stream_td` enqueues the next TD pointing to
   the same DMA buffer, with `defer_doorbell=true`
4. The doorbell is rung later in the `run()` loop (lines 191-194 or 203-207)

The hardware cannot start writing to the DMA buffer until: (a) the doorbell is
rung, (b) the xHC processes the TD, and (c) the device sends data. By that time,
the `memcpy` in step 2 has long completed.

Even in the fallback case where `_queue_deferred_doorbell` finds the array full
(line 823-824) and rings the doorbell immediately, the `memcpy` has already
completed in step 2 before `_queue_interrupt_in_stream_td` is called in step 3.

**No fix needed.**

---

## 4. Deferred Doorbells

### Finding: Keyboard input stalls during hub event processing (MODERATE)

**Location:** `run()` loop, `xhci.cpp:184-210`

The `run()` loop structure is:

```
while (true) {
    wait_for_event();                    // line 185
    m_pending_doorbell_count = 0;        // line 187
    _process_event_ring();               // line 188 — completions queue deferred doorbells
    flush deferred doorbells;            // lines 191-194
    _process_hub_events();               // line 196 — can take SECONDS
    flush remaining deferred doorbells;  // lines 203-207
}
```

During `_process_hub_events()` (line 196), device enumeration occurs. Each
`_setup_hub_device` → `_enumerate_device` call involves multiple synchronous
control transfers via `_send_command` (each with a 5-second timeout):

- Enable Slot
- Address Device (BSR=1)
- GET_DESCRIPTOR (8 bytes)
- Address Device (BSR=0)
- GET_DESCRIPTOR (full)
- SET_CONFIGURATION
- Class driver probing (GET_REPORT_DESCRIPTOR, SET_PROTOCOL, SET_IDLE, open_interrupt_in_stream)

`_send_command` internally calls `_process_event_ring()`, which processes any
keyboard completion events and calls `_complete_interrupt_in_stream`. This
re-queues a TD with `defer_doorbell=true`. But the deferred doorbell is NOT
flushed until either:

1. The `m_pending_doorbells[32]` array fills up (line 823), causing the next
   doorbell to be rung immediately, OR
2. `_process_hub_events()` returns and we reach lines 203-207.

**Impact:** During device enumeration (which can take 100ms+), the keyboard
endpoint stops receiving data because the hardware doesn't know about the
newly-queued TD. With a 32-entry doorbell array, and keyboard polling at 125Hz
(8ms intervals), the array fills after 32 × 8ms = 256ms. After that, doorbells
spill to immediate ringing (line 824). So the stall is bounded to ~256ms.

If multiple hub ports enumerate simultaneously (e.g., hub with multiple devices),
the stall could be longer. But the 32-entry overflow safety valve prevents
indefinite stalls.

**Recommendation:**
- Consider flushing deferred doorbells after each `_send_command` returns within
  `_process_hub_events`, or reduce the doorbell array to a smaller size to trigger
  the immediate-ring fallback sooner.

---

## 5. Disconnection During Stream Read

### Finding: Disconnect wake-up path is correct — CONFIRMED SAFE

**Disconnect sequence** (for a hub-attached device via `_teardown_hub_device`):

1. **Line 1322:** `_mark_async_endpoints_for_device_disconnecting(device)` — sets
   `state->disconnecting = true` under spinlock (lines 2134-2137)

2. **Line 1324:** `usb::core::device_disconnected(this, device)` — calls
   `hid_driver::disconnect()` which sets `m_disconnected = true` (hid_driver.cpp:241)

3. **Line 1326:** `_clear_async_endpoints_for_device(device, device_gone)` —
   calls `_clear_async_endpoint_state` which:
   - Under spinlock: sets `active = false`, `closing = false` (lines 2100-2106)
   - After spinlock: `wake_all(state.interrupt_in_stream.available_wq)` (line 2120)

The HID task blocked in `usb_read_interrupt_in_stream` (line 2392,
`sync::wait(available_wq, ...)`) is woken by step 3. Upon waking, it re-acquires
the lock, sees `active == false` at line 2375, breaks, and returns `-1`.

The HID driver's `run()` loop sees `rc != 0` at line 202, checks `m_disconnected`
at line 203, and breaks.

**The lifetime management is also correct:** `_clear_async_endpoint_state` does NOT
free `payloads`/`payload_storage` (it only nulls `head`/`count` and sets
`active = false`). The HID driver's cleanup at hid_driver.cpp:233-236 calls
`usb::close_interrupt_in_stream(m_stream)`, which properly frees the storage
via `usb_close_interrupt_in_stream` (lines 2458-2463).

The device memory is protected by the `finalize_disconnected_device` lifetime
system (usb_core.cpp:323-346), which ensures `xhci_device` is not freed until
both HCD teardown and the driver task complete.

### Finding: `_stop_endpoint` is skipped during disconnect cleanup — CONFIRMED SAFE

In `usb_close_interrupt_in_stream` (line 2427): `need_stop = state->interrupt_in_stream.active`.
Since `_clear_async_endpoint_state` already set `active = false`, `need_stop` is
`false` and `_stop_endpoint` is not called. This is correct because:

1. The slot was already disabled by `_disable_slot` (line 1348 for hub devices,
   line 904 for root devices).
2. Calling `_stop_endpoint` on a disabled slot would fail.
3. Any in-flight TD is implicitly cancelled by slot disablement.

---

## 6. Stream Close Race

### Finding: Close race is properly guarded — CONFIRMED SAFE

The `closing` flag and `active` checks provide correct mutual exclusion between
`usb_close_interrupt_in_stream` (HID task) and `_complete_interrupt_in_stream`
(HCD task).

**Detailed trace of the worst-case interleaving:**

```
T1: HCD task enters _complete_interrupt_in_stream
    - Acquires lock, checks closing=false, releases lock
    - Checks active=true (line 2029, without lock — see note below)

T2: HID task enters usb_close_interrupt_in_stream
    - Acquires lock: active=false, closing=true, nulls payloads/storage
    - Releases lock
    - Calls wake_all

T3: HCD task continues (past line 2029)
    - Acquires lock at line 2054
    - Checks active=false → skips payload copy (line 2055)
    - Releases lock

T4: HCD task calls _queue_interrupt_in_stream_td (line 2067)
    - Checks active=false at line 1936 → returns -1

T5: HCD task handles rc != 0 (line 2067-2073)
    - Sets active=false under lock (already false, harmless)
```

No use-after-free occurs because:
- The payload copy (which would access freed `payloads`) is guarded by
  `if (state.interrupt_in_stream.active)` **under the spinlock** at line 2055.
- The TD enqueue is guarded by `!state.interrupt_in_stream.active` at line 1936.

**Note:** The `active` check at line 2029 is **outside the lock**, which is
technically a data race. However, it is benign:
- If the stale read sees `true`, the subsequent locked checks at lines 2055
  and 1936 catch the actual state.
- If the stale read sees `false`, it returns early (correct behavior).
- `bool` reads are atomic on all supported architectures.

### Finding: `closing` flag consumed correctly for Stop Endpoint events

When `usb_close_interrupt_in_stream` calls `_stop_endpoint` (line 2446), the
controller generates a `STOPPED` Transfer Event for any in-flight TD. This event
is processed by `_complete_interrupt_in_stream` (via `_process_event_ring` inside
`_send_command`):
- Line 2019: `closing` is `true` → consumed, returns immediately.

If the in-flight TD completed naturally before the stop command:
- The completion event arrives first; `_complete_interrupt_in_stream` sees
  `closing=true`, consumes it, returns.
- The stop command may not generate a STOPPED event (no TD was in-flight).
- `usb_close_interrupt_in_stream` proceeds normally.

At most one TD is in flight at any time (one is queued per completion), so there
is at most one completion event to handle during close.

---

## Additional Findings

### Finding: `_stop_endpoint` in `usb_close_interrupt_in_stream` can be called from non-HCD task (DESIGN HAZARD)

**Location:** `usb_close_interrupt_in_stream`, `xhci.cpp:2445-2447`

If `need_stop` is true, `_stop_endpoint` → `_send_command` is called. `_send_command`
calls `_process_event_ring()` and `wait_for_event()`. If this is called from the
HID task (not the HCD task), both tasks would concurrently process the event ring,
corrupting shared state.

**Mitigating factor:** In all current code paths, `need_stop` is `false` when
called from a non-HCD task:
- During disconnect: `_clear_async_endpoint_state` already set `active = false`.
- During error exit: `_complete_interrupt_in_stream`'s error path set `active = false`.
- During probe failure: runs on the HCD task (same task as `run()`).

**Recommendation:** Add an assertion that `_stop_endpoint` is only called from
the HCD task, or restructure so `usb_close_interrupt_in_stream` posts a
close-request to the HCD task rather than executing `_stop_endpoint` directly.

### Finding: `_complete_interrupt_in_stream` does not wake reader on TD re-queue failure (LOW)

**Location:** `xhci.cpp:2067-2073`

```
if (_queue_interrupt_in_stream_td(device, ep, state, true) != 0) {
    RUN_ELEVATED({
        ...
        state.interrupt_in_stream.active = false;
        ...
    });
}

RUN_ELEVATED(sync::wake_one(state.interrupt_in_stream.available_wq));
```

When `_queue_interrupt_in_stream_td` fails, `active` is set to `false` but
`wake_one` is still called (line 2075). This is **correct** — the reader needs
to be woken to see that `active` changed. However, it should arguably be
`wake_all` (consistent with the error path at line 2040) in case multiple
readers exist (even though currently there is only one).

---

## Summary Table

| # | Issue | Severity | Can Cause Keyboard Stall? |
|---|-------|----------|--------------------------|
| 1 | Transient USB errors permanently kill stream | **CRITICAL** | **YES** — permanent until replug |
| 2 | No STALL recovery for interrupt endpoints | **HIGH** | **YES** — permanent until replug |
| 3 | No error logging on stream deactivation | **MODERATE** | No, but blocks diagnosis |
| 4 | Keyboard stalls during hub device enumeration | **MODERATE** | **YES** — temporary (~250ms) |
| 5 | Queue depth 2 can drop payloads silently | **LOW** | Possible stuck key, unlikely |
| 6 | `dropped` counter never checked/logged | **LOW** | No, but blocks diagnosis |
| 7 | `_stop_endpoint` concurrency design hazard | **LOW** | No (currently unreachable) |
| 8 | DMA buffer race | None | **SAFE** |
| 9 | Close race | None | **SAFE** |
| 10 | Disconnect wake-up | None | **SAFE** |
| 11 | Ring Underrun on interrupt EP | None | **Cannot occur** |
| 12 | EP0 stall affecting interrupt EP | None | **Cannot occur** |
