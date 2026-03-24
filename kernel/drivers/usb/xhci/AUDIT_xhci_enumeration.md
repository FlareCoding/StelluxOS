# xHCI Device Enumeration Audit: FS/LS Devices Behind USB 2.0 Hubs

**Scope**: `_setup_hub_device`, `_enumerate_device`, `_configure_ctrl_ep_input_context`,
`_address_device`, `_initial_max_packet_size`, `_clear_tt_buffer` in `xhci.cpp`.

---

## 1. Slot Context TT Fields

**Code**: `_configure_ctrl_ep_input_context` (lines 2479–2547)

For FS/LS devices behind a HS hub, the code at lines 2508–2529 walks the parent chain
to find the nearest high-speed hub and sets `parent_hub_slot_id` and `parent_port_number`
on the slot context:

```
auto* hub = m_slot_devices[device->parent_slot_id()];
uint8_t port_on_hub = device->parent_port_num();
while (hub && hub->speed() != XHCI_USB_SPEED_HIGH_SPEED
           && hub->parent_slot_id() != 0) {
    port_on_hub = hub->parent_port_num();
    hub = m_slot_devices[hub->parent_slot_id()];
}
```

**Assessment: Correct for single-tier and multi-tier topologies.**

- For a FS device directly behind a HS hub: the while-loop body is skipped
  (parent is already HS), `parent_hub_slot_id` = HS hub's slot, `parent_port_number` =
  the port on the HS hub where the device (or an intermediate FS hub) connects. Correct.

- For multi-tier (e.g., FS device → FS hub → HS hub): the loop walks up from the FS hub
  to the HS hub, updating `port_on_hub` to the port *on the HS hub* where the FS hub
  is attached. This is the correct TT routing per xHCI spec Section 6.2.2.

- **Edge case — FS hub behind FS hub behind HS hub**: The loop walks until it finds
  the HS ancestor, tracking the port at each step. The final `port_on_hub` is the port
  on the HS hub. Correct.

- **Termination guard**: `hub->parent_slot_id() != 0` prevents walking past the root hub.
  If no HS hub is found, the else-branch at line 2524 logs a warning and leaves the TT
  fields zeroed. This is safe — it means the topology has no TT (e.g., device is FS on a
  root port, which is impossible per xHCI spec for FS but handled gracefully).

- **MTT propagation**: Line 2520 sets `slot_ctx->mtt` from the HS hub's MTT capability.
  Correct — the xHCI spec says MTT in the slot context should reflect the TT hub's MTT.

**No issues found.**

---

## 2. Max Packet Size for FS/LS — `_initial_max_packet_size`

**Code**: Lines 2572–2585

| Speed       | Returned value | USB Spec requirement          | Verdict    |
|-------------|----------------|-------------------------------|------------|
| Low speed   | 8              | Must be 8 (USB 2.0 §5.5.3)   | **Correct** |
| Full speed  | 64             | 8, 16, 32, or 64 (USB 2.0 §5.5.3) | **See note** |
| High speed  | 64             | Must be 64 (USB 2.0 §5.5.3)  | **Correct** |
| Super speed | 512            | Must be 512 (USB 3.x §8.4.4) | **Correct** |

**Note on FS initial value of 64**: The user's query suggests 8 is safer for the initial
8-byte descriptor read. However, 64 is actually the correct choice for the xHCI
controller's EP0 context. The xHCI spec (Section 4.3, "Device Slot Initialization")
states that for an Address Device command the EP0 Max Packet Size should be set to a
value based on the speed, and for FS the initial guess should be 8 bytes. However:

- **Risk**: Some FS devices have bMaxPacketSize0=8. If the controller is told MPS=64 and
  the device only sends 8 bytes in the data phase of the first GET_DESCRIPTOR(8 bytes),
  this is fine — the transfer completes successfully because the host requested only
  8 bytes. The MPS field only limits what the host *accepts* per packet; a short packet
  is always valid.

- **Correctness concern**: The xHCI specification Section 4.6.5 says: *"Software shall set
  the value of Max Packet Size to the maximum value for the default Control Endpoint, as
  function of the [speed]."* For full-speed, this maximum is 64. So the code's value
  of 64 is actually **spec-compliant** — it's the maximum the endpoint *could* have.

- **Practical safety**: Using 64 for FS avoids the need for an Evaluate Context command
  for the majority of FS devices (which use MPS=64). Only legacy devices with MPS=8/16/32
  will trigger the Evaluate Context path. This is a valid optimization used by Linux.

**Verdict: Correct.** The code follows the xHCI spec recommendation. Using 8 would also
work but would force an unnecessary Evaluate Context for most FS devices.

---

## 3. BSR=1 (Block Set Address Request) and TT Handling

**Code**: `_address_device` (lines 2549–2570), called at line 1430.

The BSR=1 Address Device command tells the xHC to assign an internal slot address and
enable EP0 *without* issuing a SET_ADDRESS on the USB wire. The device remains on
default address 0.

**TT interaction**: After BSR=1, the device responds at address 0 on the FS/LS bus
behind the TT. The TT in the HS hub handles the split transactions transparently.
BSR=1 does not alter any TT state — it's purely an xHC internal operation.

**VL805 concerns**: The VL805 has known issues with TT babble during concurrent periodic
traffic (acknowledged in the comment at line 1434). The code mitigates this with:
1. Retry loop with `_clear_tt_buffer` between attempts (line 1442).
2. SOF boundary avoidance for FS doorbell rings (lines 2659–2668).

**Assessment: Correct.** BSR=1 doesn't interact with the TT directly. The mitigations
for VL805 TT babble are appropriate.

---

## 4. CLEAR_TT_BUFFER with dev_addr=0

**Code**: `_clear_tt_buffer` (lines 1015–1038), called at line 1442 with default `dev_addr=0`.

The first descriptor read (line 1440) happens after BSR=1, when the device is still at
USB address 0 on the wire. The `_clear_tt_buffer` call at line 1442 uses `dev_addr=0`.

**USB 2.0 Spec Section 11.24.2.3 (CLEAR_TT_BUFFER)**:
wValue encodes `Dev_Addr` in bits [10:4]. Address 0 is the default USB address.

**Assessment: Correct.** After BSR=1, the device's wire address is 0. If the TT buffer
is stuck from a failed transaction to address 0, CLEAR_TT_BUFFER with dev_addr=0 is the
right value to clear it. This is consistent with what Linux does (`hub_tt_work` uses
the device's current USB address).

**Second read (post BSR=0)**: At line 1492, the code correctly reads the assigned USB
address from the output slot context (lines 1479–1486) and passes it to
`_clear_tt_buffer(device, dev_addr)`. This is correct — after SET_ADDRESS, the TT buffer
entry is keyed by the new address.

**Potential concern — CLEAR_TT_BUFFER for control endpoint**: The wValue encoding at
line 1031 uses `EP_Num=0` (bits [3:0]=0) and `EP_Type=0` (bits [12:11]=0, meaning
"control"). Per USB 2.0, EP_Type encoding for CLEAR_TT_BUFFER is: 0=Control, 1=Isochronous,
2=Bulk, 3=Interrupt. Control=0 is correct for EP0.

**Minor concern**: The code clears both IN (line 1033, `base | 0x8000`) and OUT (line 1036,
`base`) directions. For control endpoints, the TT buffer could be stuck in either
direction, so clearing both is defensive and correct. Some implementations only clear the
direction that failed, but clearing both is safe.

**No issues found.**

---

## 5. Post-SET_ADDRESS Delay

**Code**: Line 1470 — `delay::us(10000)` (10 ms).

USB 2.0 Spec Section 9.2.6.3 states: *"After successful completion of the Status stage,
the device is allowed a SetAddress recovery interval of 2 ms."*

**Assessment: 10 ms is generous and safe.** The 2 ms minimum is for the *fastest*
compliant device. In practice:
- Most devices are fine with 2–5 ms.
- Some particularly slow devices (certain mass storage devices, legacy keyboards) may
  need up to 10 ms or even 50 ms.
- Linux uses 10 ms in its enumeration path for the same reason.

**Verdict: Correct.** 10 ms provides good margin. If extremely slow devices are
encountered, this could be increased to 20–50 ms, but 10 ms is the standard choice.

---

## 6. Evaluate Context for Max Packet Size Update

**Code**: Lines 1449–1462

When the device reports a different `bMaxPacketSize0` than the initial guess, the code:
1. Calls `_configure_ctrl_ep_input_context` again with the new MPS (line 1452).
2. Sends an Evaluate Context command (lines 1455–1461).

**Assessment — add_flags**: `_configure_ctrl_ep_input_context` sets `add_flags = (1<<0) | (1<<1)`
(slot context + EP0 context). Per xHCI spec Section 6.2.3.3, Evaluate Context only
evaluates fields in contexts whose Add Context flags are set. Setting A0 (slot) and A1
(EP0) is correct — the controller needs to see both.

**Assessment — fields evaluated**: For Evaluate Context, the xHCI spec says only specific
fields are evaluated:
- Slot context: Interrupter Target, Max Exit Latency, MTT (some controllers).
- EP0 context: Max Packet Size.

The code re-fills the entire input context via `_configure_ctrl_ep_input_context`, which
is safe — the controller ignores fields it doesn't evaluate.

**FS/LS specifics**: For FS devices with MPS=8 (initial guess was 64), the Evaluate
Context updates the controller's EP0 max packet size to 8. This is critical for correct
data toggle and packet splitting. Works correctly.

**Controller quirks**: Some controllers (notably older Renesas and ASMedia) have been
reported to fail Evaluate Context for EP0 on FS devices. The VL805 does not have this
known issue, but the error is handled (ENUM_FAIL at line 1461).

**Potential concern**: After Evaluate Context, the code proceeds to `_address_device(BSR=0)`
at line 1465. At this point the xHC's internal EP0 context has the updated MPS. The
BSR=0 Address Device command will re-write the slot and EP0 contexts from the input
context. **This is correct** — the input context was just updated at line 1452 with the
correct MPS, so the Address Device command picks up the right value.

**No issues found.**

---

## 7. Route String Calculation

**Code**: `_setup_hub_device` lines 1270–1278

```
uint32_t parent_rs = hub_device->route_string();
uint32_t rs = parent_rs;
uint8_t port_nibble = (hub_port > 15) ? 15 : hub_port;
for (uint8_t shift = 0; shift < 20; shift += 4) {
    if (((rs >> shift) & 0xF) == 0) {
        rs |= (static_cast<uint32_t>(port_nibble) << shift);
        break;
    }
}
```

**Port numbers > 15**: The xHCI spec Section 8.9 (USB 3.1 spec) states that each nibble
in the route string can hold values 1–15. Port numbers greater than 15 are capped to 15.
The code does this correctly at line 1272: `(hub_port > 15) ? 15 : hub_port`.

**xHCI spec compliance**: The xHCI spec Section 6.2.2 says the route string is 20 bits
(5 nibbles), supporting up to 5 tiers of hubs. The loop iterates over shifts 0, 4, 8,
12, 16 (5 iterations), which covers all 5 nibbles. Correct.

**Finding the lowest available nibble**: The loop finds the first zero nibble and writes
the port number there. This works because:
- Root hub devices have route_string=0, so the first nibble (shift=0) is available.
- Each tier fills the next nibble in sequence.
- A nibble value of 0 indicates "unused" since valid port numbers start at 1.

**Edge case — hub port 0**: The function guards against this at line 1196 (`hub_port == 0`).
Port 0 would be invisible in the route string (nibble = 0 = unused). Correct to reject.

**Edge case — more than 5 tiers**: If all 5 nibbles are occupied, the loop finds no zero
nibble and `rs` remains unchanged. The device would have an incorrect route string.
However, USB 3.x limits topology to 5 tiers max (root hub + 5 external hubs), and USB 2.0
limits to 5 tiers as well. This is a theoretical edge case that the code doesn't explicitly
guard against, but it's unreachable in practice.

**No issues found.**

---

## 8. Speed Mismatch

**Code**: `hub_speed_to_xhci_speed` in `hub_driver.cpp` (lines 308–316)

The hub driver reads `wPortStatus` bits to determine speed:
- Bit 10 (PORT_STATUS_HIGH_SPEED) → HS
- Bit 9 (PORT_STATUS_LOW_SPEED) → LS
- Neither → FS (default)

**Can the hub report the wrong speed?** In practice:
- After a port reset, the hub reports the negotiated speed. This is authoritative — the
  hub's PHY determines the speed during reset signaling.
- A well-functioning hub will not report the wrong speed. If it does, the device is
  unreachable at the wrong speed and all transactions will fail/time out.

**What would happen on mismatch**:
1. If hub says FS but device is LS: The slot context would have `speed=1` (FS). The xHC
   would use FS timing for split transactions through the TT. LS devices use different
   signaling (bit stuffing, PRE/PID packets) that the TT handles differently. The TT
   would send full-speed split transactions but the device wouldn't respond correctly.
   Result: Transaction Errors or timeouts during GET_DESCRIPTOR. The retry loop would
   exhaust attempts and ENUM_FAIL would tear down the device. No data corruption.

2. If hub says LS but device is FS: The slot context would have `speed=2` (LS). EP0 MPS
   would be set to 8 (correct for LS). The TT would use LS split timing. Some FS devices
   might partially respond, but packet sizes would be wrong. Again: Transaction Errors,
   retry exhaustion, ENUM_FAIL teardown.

**Assessment: Graceful degradation.** The code doesn't validate speed independently
(which would require analyzing the port status bits directly from the hub's port
registers, which it already does in the diagnostic block at lines 1207–1220). The
diagnostic logging at line 1213 captures `LS` and `HS` bits alongside the reported
speed, which aids debugging but doesn't prevent the mismatch.

**Potential improvement**: The pre-enumeration diagnostic (lines 1207–1220) could compare
the speed bits in `ps.status` with the `speed` argument and log a warning on mismatch.
This would help diagnose hub firmware bugs.

---

## Additional Observations

### A. `_clear_tt_buffer` Only Walks One Level Up

**Code**: Lines 1022–1024

```
auto* hub = m_slot_devices[device->parent_slot_id()];
if (!hub || hub->speed() != XHCI_USB_SPEED_HIGH_SPEED)
    return;
```

Unlike `_configure_ctrl_ep_input_context` which walks up the chain to find the HS hub,
`_clear_tt_buffer` only checks the *immediate* parent. For a FS device behind a FS hub
behind a HS hub, `device->parent_slot_id()` points to the FS hub, not the HS hub.
The function would see that the parent is not HS and **return without clearing**.

**This is a bug for multi-tier topologies.**

CLEAR_TT_BUFFER must be sent to the HS hub that owns the TT, not the immediate parent.
The same walk-up logic from `_configure_ctrl_ep_input_context` (lines 2510–2516) should
be applied here.

**Similarly**, the `ENUM_FAIL` macro (lines 1408–1412) has the same single-level check
for RESET_TT — it only sends RESET_TT if `device->parent_slot_id()` is HS. This also
fails for multi-tier topologies.

**Severity**: Medium. Multi-tier FS hub topologies are uncommon but do exist (e.g.,
FS hub cascaded off a HS hub). If a TT buffer gets stuck in such a topology, the
CLEAR_TT_BUFFER and RESET_TT mitigation would silently fail, potentially blocking all
FS/LS traffic through that TT.

### B. Evaluate Context Before BSR=0 — Redundant Slot/EP0 Rewrite

When `desc.bMaxPacketSize0 != max_packet_size`, the code sends Evaluate Context (line
1460) and then immediately sends Address Device with BSR=0 (line 1465). The BSR=0
Address Device command will overwrite the slot and EP0 contexts from the input context
again. The Evaluate Context is still necessary because it updates the xHC's *output*
context for EP0, and the BSR=0 command reads the *input* context which already has the
correct MPS. No correctness issue, just noting the interaction.

### C. VL805 SOF Boundary Workaround Scope

**Code**: Lines 2659–2668

The SOF boundary workaround only applies to `XHCI_USB_SPEED_FULL_SPEED` (line 2662).
LS devices behind a hub also go through the TT and could theoretically suffer from TT
babble if the doorbell rings near SOF. Consider extending this workaround to LS:

```
if (device->route_string() != 0 &&
    (device->speed() == XHCI_USB_SPEED_FULL_SPEED ||
     device->speed() == XHCI_USB_SPEED_LOW_SPEED)) {
```

**Severity**: Low. LS control transfers are short and less likely to trigger TT babble
due to their lower bandwidth. However, VL805 errata may still apply.

### D. No Reset Recovery Delay After Hub Port Reset

The hub driver's `reset_port` (hub_driver.cpp line 225) waits for the reset to complete
(PORT_CHANGE_RESET bit) but doesn't add an explicit recovery delay after reset. USB 2.0
Section 7.1.7.5 says the hub should provide a 10 ms reset recovery time (TRSTRCY), and
Section 9.2.6.2 says the device needs time after reset. Some slow devices need up to
100 ms before they respond to the first SETUP packet.

The code does have `delay::us(10000)` between retry attempts (line 1443) if the first
GET_DESCRIPTOR fails, which partially compensates. But there's no explicit post-reset
settling time before the first Address Device command.

**Severity**: Low-Medium. Most devices are ready quickly after reset, but some legacy
keyboards and storage devices may need 50–100 ms.

---

## Summary

| # | Area | Verdict | Severity |
|---|------|---------|----------|
| 1 | Slot context TT fields | **Correct** | — |
| 2 | Initial max packet size | **Correct** (64 for FS is spec-compliant) | — |
| 3 | BSR=1 + TT interaction | **Correct** | — |
| 4 | CLEAR_TT_BUFFER dev_addr=0 | **Correct** | — |
| 5 | Post-SET_ADDRESS delay | **Correct** (10 ms, generous) | — |
| 6 | Evaluate Context for MPS | **Correct** | — |
| 7 | Route string calculation | **Correct** | — |
| 8 | Speed mismatch handling | **Graceful** (fails safely) | — |
| A | `_clear_tt_buffer` single-level walk | **BUG** — multi-tier FS hub topology | Medium |
| B | Evaluate Context + BSR=0 ordering | No issue (redundant but correct) | — |
| C | VL805 SOF workaround LS omission | **Potential gap** | Low |
| D | Missing post-reset recovery delay | **Potential gap** | Low-Medium |
