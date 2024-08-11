#ifndef XCHI_H
#define XCHI_H
#include <acpi/mcfg.h>

namespace drivers {
// Memory Alignment and Boundary Definitions
#define XHCI_DEVICE_CONTEXT_INDEX_MAX_SIZE      2048
#define XHCI_DEVICE_CONTEXT_MAX_SIZE            2048
#define XHCI_INPUT_CONTROL_CONTEXT_MAX_SIZE     64
#define XHCI_SLOT_CONTEXT_MAX_SIZE              64
#define XHCI_ENDPOINT_CONTEXT_MAX_SIZE          64
#define XHCI_STREAM_CONTEXT_MAX_SIZE            16
#define XHCI_STREAM_ARRAY_LINEAR_MAX_SIZE       1024 * 1024 // 1 MB
#define XHCI_STREAM_ARRAY_PRI_SEC_MAX_SIZE      PAGE_SIZE
#define XHCI_TRANSFER_RING_SEGMENTS_MAX_SIZE    1024 * 64   // 64 KB
#define XHCI_COMMAND_RING_SEGMENTS_MAX_SIZE     1024 * 64   // 64 KB
#define XHCI_EVENT_RING_SEGMENTS_MAX_SIZE       1024 * 64   // 64 KB
#define XHCI_EVENT_RING_SEGMENT_TABLE_MAX_SIZE  1024 * 512  // 512 KB
#define XHCI_SCRATCHPAD_BUFFER_ARRAY_MAX_SIZE   248
#define XHCI_SCRATCHPAD_BUFFERS_MAX_SIZE        PAGE_SIZE

#define XHCI_DEVICE_CONTEXT_INDEX_BOUNDARY      PAGE_SIZE
#define XHCI_DEVICE_CONTEXT_BOUNDARY            PAGE_SIZE
#define XHCI_INPUT_CONTROL_CONTEXT_BOUNDARY     PAGE_SIZE
#define XHCI_SLOT_CONTEXT_BOUNDARY              PAGE_SIZE
#define XHCI_ENDPOINT_CONTEXT_BOUNDARY          PAGE_SIZE
#define XHCI_STREAM_CONTEXT_BOUNDARY            PAGE_SIZE
#define XHCI_STREAM_ARRAY_LINEAR_BOUNDARY       PAGE_SIZE
#define XHCI_STREAM_ARRAY_PRI_SEC_BOUNDARY      PAGE_SIZE
#define XHCI_TRANSFER_RING_SEGMENTS_BOUNDARY    1024 * 64   // 64 KB
#define XHCI_COMMAND_RING_SEGMENTS_BOUNDARY     1024 * 64   // 64 KB
#define XHCI_EVENT_RING_SEGMENTS_BOUNDARY       1024 * 64   // 64 KB
#define XHCI_EVENT_RING_SEGMENT_TABLE_BOUNDARY  PAGE_SIZE
#define XHCI_SCRATCHPAD_BUFFER_ARRAY_BOUNDARY   PAGE_SIZE
#define XHCI_SCRATCHPAD_BUFFERS_BOUNDARY        PAGE_SIZE

#define XHCI_DEVICE_CONTEXT_INDEX_ALIGNMENT      64
#define XHCI_DEVICE_CONTEXT_ALIGNMENT            64
#define XHCI_INPUT_CONTROL_CONTEXT_ALIGNMENT     64
#define XHCI_SLOT_CONTEXT_ALIGNMENT              32
#define XHCI_ENDPOINT_CONTEXT_ALIGNMENT          32
#define XHCI_STREAM_CONTEXT_ALIGNMENT            16
#define XHCI_STREAM_ARRAY_LINEAR_ALIGNMENT       16
#define XHCI_STREAM_ARRAY_PRI_SEC_ALIGNMENT      16
#define XHCI_TRANSFER_RING_SEGMENTS_ALIGNMENT    16
#define XHCI_COMMAND_RING_SEGMENTS_ALIGNMENT     64
#define XHCI_EVENT_RING_SEGMENTS_ALIGNMENT       64
#define XHCI_EVENT_RING_SEGMENT_TABLE_ALIGNMENT  64
#define XHCI_SCRATCHPAD_BUFFER_ARRAY_ALIGNMENT   64
#define XHCI_SCRATCHPAD_BUFFERS_ALIGNMENT        PAGE_SIZE

// Configuration Definitions
#define XHCI_COMMAND_RING_TRB_COUNT     256
#define XHCI_EVENT_RING_TRB_COUNT       256
#define XHCI_TRANSFER_RING_TRB_COUNT    256

/*
// xHci Spec Section 5.4.1 USB Table 5-20: USB Command Register Bit Definitions (USBCMD) (page 358)

Run/Stop (R/S) – RW. Default = ‘0’. ‘1’ = Run. ‘0’ = Stop. When set to a ‘1’, the xHC proceeds with
execution of the schedule. The xHC continues execution as long as this bit is set to a ‘1’. When this bit
is cleared to ‘0’, the xHC completes any current or queued commands or TDs, and any USB transactions
associated with them, then halts.
Refer to section 5.4.1.1 for more information on how R/S shall be managed.
The xHC shall halt within 16 ms. after software clears the Run/Stop bit if the above conditions have
been met.
The HCHalted (HCH) bit in the USBSTS register indicates when the xHC has finished its pending
pipelined transactions and has entered the stopped state. Software shall not write a ‘1’ to this flag
unless the xHC is in the Halted state (that is, HCH in the USBSTS register is ‘1’). Doing so may yield
undefined results. Writing a ‘0’ to this flag when the xHC is in the Running state (that is, HCH = ‘0’) and
any Event Rings are in the Event Ring Full state (refer to section 4.9.4) may result in lost events.
When this register is exposed by a Virtual Function (VF), this bit only controls the run state of the xHC
instance presented by the selected VF. Refer to section 8 for more information.
*/
#define XHCI_USBCMD_RUN_STOP                    (1 << 0)

/*
// xHci Spec Section 5.4.1 USB Table 5-20: USB Command Register Bit Definitions (USBCMD) (page 358)

Host Controller Reset (HCRST) – RW. Default = ‘0’. This control bit is used by software to reset the
host controller. The effects of this bit on the xHC and the Root Hub registers are similar to a Chip
Hardware Reset.
When software writes a ‘1’ to this bit, the Host Controller resets its internal pipelines, timers, counters,
state machines, etc. to their initial value. Any transaction currently in progress on the USB is
immediately terminated. A USB reset shall not be driven on USB2 downstream ports, however a Hot or
Warm Reset79 shall be initiated on USB3 Root Hub downstream ports.
PCI Configuration registers are not affected by this reset. All operational registers, including port
registers and port state machines are set to their initial values. Software shall reinitialize the host
controller as described in Section 4.2 in order to return the host controller to an operational state.
This bit is cleared to ‘0’ by the Host Controller when the reset process is complete. Software cannot
terminate the reset process early by writing a ‘0’ to this bit and shall not write any xHC Operational or
Runtime registers until while HCRST is ‘1’. Note, the completion of the xHC reset process is not gated by
the Root Hub port reset process.
Software shall not set this bit to ‘1’ when the HCHalted (HCH) bit in the USBSTS register is a ‘0’.
Attempting to reset an actively running host controller may result in undefined behavior.
When this register is exposed by a Virtual Function (VF), this bit only resets the xHC instance presented
by the selected VF. Refer to section 8 for more information.
*/
#define XHCI_USBCMD_HCRESET                     (1 << 1)

/*
// xHci Spec Section 5.4.1 USB Table 5-20: USB Command Register Bit Definitions (USBCMD) (page 359)

Interrupter Enable (INTE) – RW. Default = ‘0’. This bit provides system software with a means of
enabling or disabling the host system interrupts generated by Interrupters. When this bit is a ‘1’, then
Interrupter host system interrupt generation is allowed, for example, the xHC shall issue an interrupt at
the next interrupt threshold if the host system interrupt mechanism (for example, MSI, MSI-X, etc.) is
enabled. The interrupt is acknowledged by a host system interrupt specific mechanism.
When this register is exposed by a Virtual Function (VF), this bit only enables the set of Interrupters
assigned to the selected VF. Refer to section 7.7.2 for more information.
*/
#define XHCI_USBCMD_INTERRUPTER_ENABLE          (1 << 2)

/*
// xHci Spec Section 5.4.1 USB Table 5-20: USB Command Register Bit Definitions (USBCMD) (page 359)

Host System Error Enable (HSEE) – RW. Default = ‘0’. When this bit is a ‘1’, and the HSE bit in the
USBSTS register is a ‘1’, the xHC shall assert out-of-band error signaling to the host. The signaling is
acknowledged by software clearing the HSE bit. Refer to section 4.10.2.6 for more information.
When this register is exposed by a Virtual Function (VF), the effect of the assertion of this bit on the
Physical Function (PF0) is determined by the VMM. Refer to section 8 for more information.
*/
#define XHCI_USBCMD_HOSTSYS_ERROR_ENABLE        (1 << 3)

/*
// xHci Spec Section 5.4.1 USB Table 5-20: USB Command Register Bit Definitions (USBCMD) (page 359)

Light Host Controller Reset (LHCRST) – RO or RW. Optional normative. Default = ‘0’. If the Light
HC Reset Capability (LHRC) bit in the HCCPARAMS1 register is ‘1’, then this flag allows the driver to
reset the xHC without affecting the state of the ports.
A system software read of this bit as ‘0’ indicates the Light Host Controller Reset has completed and it is
safe for software to re-initialize the xHC. A software read of this bit as a ‘1’ indicates the Light Host
Controller Reset has not yet completed.
If not implemented, a read of this flag shall always return a ‘0’.
All registers in the Aux Power well shall maintain the values that had been asserted prior to the Light
Host Controller Reset. Refer to section 4.23.1 for more information.
When this register is exposed by a Virtual Function (VF), this bit only generates a Light Reset to the
xHC instance presented by the selected VF, for example, Disable the VFs’ device slots and set the
associated VF Run bit to Stopped. Refer to section 8 for more information.
*/
#define XHCI_USBCMD_LIGHT_HCRESET               (1 << 7)

/*
// xHci Spec Section 5.4.1 USB Table 5-20: USB Command Register Bit Definitions (USBCMD) (page 359)

Controller Save State (CSS) - RW. Default = ‘0’. When written by software with ‘1’ and HCHalted
(HCH) = ‘1’, then the xHC shall save any internal state (that may be restored by a subsequent Restore
State operation) and if FSC = '1' any cached Slot, Endpoint, Stream, or other Context information (so
that software may save it). When written by software with ‘1’ and HCHalted (HCH) = ‘0’, or written with
‘0’, no Save State operation shall be performed. This flag always returns ‘0’ when read. Refer to the
Save State Status (SSS) flag in the USBSTS register for information on Save State completion. Refer to
section 4.23.2 for more information on xHC Save/Restore operation. Note that undefined behavior may
occur if a Save State operation is initiated while Restore State Status (RSS) = ‘1’.
When this register is exposed by a Virtual Function (VF), this bit only controls saving the state of the
xHC instance presented by the selected VF. Refer to section 8 for more information.
*/
#define XHCI_USBCMD_CSS                         (1 << 8)

/*
// xHci Spec Section 5.4.1 USB Table 5-20: USB Command Register Bit Definitions (USBCMD) (page 359)

Controller Restore State (CRS) - RW. Default = ‘0’. When set to ‘1’, and HCHalted (HCH) = ‘1’, then
the xHC shall perform a Restore State operation and restore its internal state. When set to ‘1’ and
Run/Stop (R/S) = ‘1’ or HCHalted (HCH) = ‘0’, or when cleared to ‘0’, no Restore State operation shall
be performed. This flag always returns ‘0’ when read. Refer to the Restore State Status (RSS) flag in
the USBSTS register for information on Restore State completion. Refer to section 4.23.2 for more
information. Note that undefined behavior may occur if a Restore State operation is initiated while Save
State Status (SSS) = ‘1’.
When this register is exposed by a Virtual Function (VF), this bit only controls restoring the state of the
xHC instance presented by the selected VF. Refer to section 8 for more information.
*/
#define XHCI_USBCMD_CRS                         (1 << 9)

/*
// xHci Spec Section 5.4.1 USB Table 5-20: USB Command Register Bit Definitions (USBCMD) (page 359)

Enable Wrap Event (EWE) - RW. Default = ‘0’. When set to ‘1’, the xHC shall generate a MFINDEX
Wrap Event every time the MFINDEX register transitions from 03FFFh to 0. When cleared to ‘0’ no
MFINDEX Wrap Events are generated. Refer to section 4.14.2 for more information.
When this register is exposed by a Virtual Function (VF), the generation of MFINDEX Wrap Events to VFs
shall be emulated by the VMM. 
*/
#define XHCI_USBCMD_EWE                         (1 << 10)

/*
// xHci Spec Section 5.4.2 Table 5-21: USB Status Register Bit Definitions (USBSTS) (page 362)

HCHalted (HCH) – RO. Default = ‘1’. This bit is a ‘0’ whenever the Run/Stop (R/S)
bit is a ‘1’. The xHC sets this bit to ‘1’ after it has stopped executing as a result of the
Run/Stop (R/S) bit being cleared to ‘0’, either by software or by the xHC hardware
(for example, internal error).
If this bit is '1', then SOFs, microSOFs, or Isochronous Timestamp Packets (ITP) shall
not be generated by the xHC, and any received Transaction Packet shall be dropped.
When this register is exposed by a Virtual Function (VF), this bit only reflects the
Halted state of the xHC instance presented by the selected VF. Refer to section 8 for
more information.
*/
#define XHCI_USBSTS_HCH                         (1 << 0)

/*
// xHci Spec Section 5.4.2 Table 5-21: USB Status Register Bit Definitions (USBSTS) (page 362)

Host System Error (HSE) – RW1C. Default = ‘0’. The xHC sets this bit to ‘1’ when
a serious error is detected, either internal to the xHC or during a host system access
involving the xHC module. (In a PCI system, conditions that set this bit to ‘1’ include
PCI Parity error, PCI Master Abort, and PCI Target Abort.) When this error occurs,
the xHC clears the Run/Stop (R/S) bit in the USBCMD register to prevent further
execution of the scheduled TDs. If the HSEE bit in the USBCMD register is a ‘1’, the
xHC shall also assert out-of-band error signaling to the host. Refer to section
4.10.2.6 for more information.
When this register is exposed by a Virtual Function (VF), the assertion of this bit
affects all VFs and reflects the Host System Error state of the Physical Function
(PF0). Refer to section 8 for more information.
*/
#define XHCI_USBSTS_HSE                         (1 << 2)

/*
// xHci Spec Section 5.4.2 Table 5-21: USB Status Register Bit Definitions (USBSTS) (page 362)

Event Interrupt (EINT) – RW1C. Default = ‘0’. The xHC sets this bit to ‘1’ when
the Interrupt Pending (IP) bit of any Interrupter transitions from ‘0’ to ‘1’. Refer to
section 7.1.2 for use.
Software that uses EINT shall clear it prior to clearing any IP flags. A race condition
may occur if software clears the IP flags then clears the EINT flag, and between the
operations another IP ‘0’ to '1' transition occurs. In this case the new IP transition
shall be lost.
When this register is exposed by a Virtual Function (VF), this bit is the logical 'OR' of
the IP bits for the Interrupters assigned to the selected VF. And it shall be cleared to
‘0’ when all associated interrupter IP bits are cleared, that is, all the VF’s Interrupter
Event Ring(s) are empty. Refer to section 8 for more information.
*/
#define XHCI_USBSTS_EINT                        (1 << 3)

/*
// xHci Spec Section 5.4.2 Table 5-21: USB Status Register Bit Definitions (USBSTS) (page 362)

Port Change Detect (PCD) – RW1C. Default = ‘0’. The xHC sets this bit to a ‘1’
when any port has a change bit transition from a ‘0’ to a ‘1’.
This bit is allowed to be maintained in the Aux Power well. Alternatively, it is also
acceptable that on a D3 to D0 transition of the xHC, this bit is loaded with the OR of
all of the PORTSC change bits. Refer to section 4.19.3.
This bit provides system software an efficient means of determining if there has been
Root Hub port activity. Refer to section 4.15.2.3 for more information.
When this register is exposed by a Virtual Function (VF), the VMM determines the
state of this bit as a function of the Root Hub Ports associated with the Device Slots
assigned to the selected VF. Refer to section 8 for more information.
*/
#define XHCI_USBSTS_PCD                         (1 << 4)

/*
// xHci Spec Section 5.4.2 Table 5-21: USB Status Register Bit Definitions (USBSTS) (page 363)

Save State Status (SSS) - RO. Default = ‘0’. When the Controller Save State
(CSS) flag in the USBCMD register is written with ‘1’ this bit shall be set to ‘1’ and
remain 1 while the xHC saves its internal state. When the Save State operation is
complete, this bit shall be cleared to ‘0’. Refer to section 4.23.2 for more
information.
When this register is exposed by a Virtual Function (VF), the VMM determines the
state of this bit as a function of the saving the state for the selected VF. Refer to
section 8 for more information.
*/
#define XHCI_USBSTS_SSS                         (1 << 8)

/*
// xHci Spec Section 5.4.2 Table 5-21: USB Status Register Bit Definitions (USBSTS) (page 363)

Restore State Status (RSS) - RO. Default = ‘0’. When the Controller Restore State
(CRS) flag in the USBCMD register is written with ‘1’ this bit shall be set to ‘1’ and
remain 1 while the xHC restores its internal state. When the Restore State operation
is complete, this bit shall be cleared to ‘0’. Refer to section 4.23.2 for more
information.
When this register is exposed by a Virtual Function (VF), the VMM determines the
state of this bit as a function of the restoring the state for the selected VF. Refer to
section 8 for more information.
*/
#define XHCI_USBSTS_RSS                         (1 << 9)

/*
// xHci Spec Section 5.4.2 Table 5-21: USB Status Register Bit Definitions (USBSTS) (page 363)

Save/Restore Error (SRE) - RW1C. Default = ‘0’. If an error occurs during a Save
or Restore operation this bit shall be set to ‘1’. This bit shall be cleared to ‘0’ when a
Save or Restore operation is initiated or when written with ‘1’. Refer to section 4.23.2
for more information.
When this register is exposed by a Virtual Function (VF), the VMM determines the
state of this bit as a function of the Save/Restore completion status for the selected
VF. Refer to section 8 for more information.
*/
#define XHCI_USBSTS_SRE                         (1 << 10)

/*
// xHci Spec Section 5.4.2 Table 5-21: USB Status Register Bit Definitions (USBSTS) (page 363)

Controller Not Ready (CNR) – RO. Default = ‘1’. ‘0’ = Ready and ‘1’ = Not Ready.
Software shall not write any Doorbell or Operational register of the xHC, other than
the USBSTS register, until CNR = ‘0’. This flag is set by the xHC after a Chip
Hardware Reset and cleared when the xHC is ready to begin accepting register
writes. This flag shall remain cleared (‘0’) until the next Chip Hardware Reset.
*/
#define XHCI_USBSTS_CNR                         (1 << 11)

/*
// xHci Spec Section 5.4.2 Table 5-21: USB Status Register Bit Definitions (USBSTS) (page 363)

Host Controller Error (HCE) – RO. Default = 0. 0’ = No internal xHC error
conditions exist and ‘1’ = Internal xHC error condition. This flag shall be set to
indicate that an internal error condition has been detected which requires software to
reset and reinitialize the xHC. Refer to section 4.24.1 for more information.
*/
#define XHCI_USBSTS_HCE                         (1 << 12)

/*
// xHci Spec Section 5.3.3 Table 5-10: Host Controller Structural Parameters 1 (HCSPARAMS1) (page 348)

Number of Device Slots (MaxSlots). This field specifies the maximum number of
Device Context Structures and Doorbell Array entries this host controller can support.
Valid values are in the range of 1 to 255. The value of ‘0’ is reserved.
*/
#define XHCI_MAX_DEVICE_SLOTS(regs) ((regs->hcsparams1) & 0xFF)

/*
// xHci Spec Section 5.3.3 Table 5-10: Host Controller Structural Parameters 1 (HCSPARAMS1) (page 348)

Number of Interrupters (MaxIntrs). This field specifies the number of
Interrupters implemented on this host controller. Each Interrupter may be allocated
to a MSI or MSI-X vector and controls its generation and moderation.
The value of this field determines how many Interrupter Register Sets are
addressable in the Runtime Register Space (refer to section 5.5). Valid values are in
the range of 1h to 400h. A ‘0’ in this field is undefined.
*/
#define XHCI_MAX_INTERRUPTERS(regs) ((regs->hcsparams1 >> 8) & 0x7FF)

/*
// xHci Spec Section 5.3.3 Table 5-10: Host Controller Structural Parameters 1 (HCSPARAMS1) (page 348)

Number of Ports (MaxPorts). This field specifies the maximum Port Number value,
that is, the highest numbered Port Register Set that are addressable in the
Operational Register Space (refer to Table 5-18). Valid values are in the range of 1h
to FFh.
The value in this field shall reflect the maximum Port Number value assigned by an
xHCI Supported Protocol Capability, described in section 7.2. Software shall refer to
these capabilities to identify whether a specific Port Number is valid, and the protocol
supported by the associated Port Register Set.
*/
#define XHCI_MAX_PORTS(regs) ((regs->hcsparams1 >> 24) & 0xFF)

/*
// xHci Spec Section 5.3.4 Table 5-11: Host Controller Structural Parameters 2 (HCSPARAMS2) (page 349)

Isochronous Scheduling Threshold (IST). Default = implementation dependent.
The value in this field indicates to system software the minimum distance (in time)
that it is required to stay ahead of the host controller while adding TRBs, in order to
have the host controller process them at the correct time. The value shall be
specified in terms of number of frames/microframes.
If bit [3] of IST is cleared to '0', software can add a TRB no later than IST[2:0]
Microframes before that TRB is scheduled to be executed.
If bit [3] of IST is set to '1', software can add a TRB no later than IST[2:0] Frames
before that TRB is scheduled to be executed.
Refer to Section 4.14.2 for details on how software uses this information for
scheduling isochronous transfers.
*/
#define XHCI_IST(regs) ((regs->hcsparams2) & 0xF)

/*
// xHci Spec Section 5.3.4 Table 5-11: Host Controller Structural Parameters 2 (HCSPARAMS2) (page 349)

Event Ring Segment Table Max (ERST Max). Default = implementation
dependent. Valid values are 0 – 15. This field determines the maximum value
supported the Event Ring Segment Table Base Size registers (5.5.2.3.1), where:
    The maximum number of Event Ring Segment Table entries = 2 ERST Max

For example, if the ERST Max = 7, then the xHC Event Ring Segment Table(s)
supports up to 128 entries, 15 then 32K entries, etc.
*/
#define XHCI_ERST_MAX(regs) ((regs->hcsparams2 >> 4) & 0xF)

/*
// xHci Spec Section 5.3.4 Table 5-11: Host Controller Structural Parameters 2 (HCSPARAMS2) (page 349)

Max Scratchpad Buffers (Max Scratchpad Bufs Hi). Default = implementation
dependent. This field indicates the high order 5 bits of the number of Scratchpad
Buffers system software shall reserve for the xHC. Refer to section 4.20 for more
information.
*/
#define XHCI_MAX_SCRATCHPAD_BUFS_HI(regs) ((regs->hcsparams2 >> 21) & 0x1F)

/*
// xHci Spec Section 5.3.4 Table 5-11: Host Controller Structural Parameters 2 (HCSPARAMS2) (page 349)

Scratchpad Restore (SPR). Default = implementation dependent. If Max Scratchpad Buffers is >
‘0’ then this flag indicates whether the xHC uses the Scratchpad Buffers for saving state when
executing Save and Restore State operations. If Max Scratchpad Buffers is = ‘0’ then this flag
shall be ‘0’. Refer to section 4.23.2 for more information.
A value of ‘1’ indicates that the xHC requires the integrity of the Scratchpad Buffer space to be
maintained across power events.
A value of ‘0’ indicates that the Scratchpad Buffer space may be freed and reallocated between
power events.
*/
#define XHCI_SPR(regs) ((regs->hcsparams2 >> 26) & 0x1)

/*
// xHci Spec Section 5.3.4 Table 5-11: Host Controller Structural Parameters 2 (HCSPARAMS2) (page 349)

Max Scratchpad Buffers (Max Scratchpad Bufs Lo). Default = implementation dependent. Valid
values for Max Scratchpad Buffers (Hi and Lo) are 0-1023. This field indicates the low order 5
bits of the number of Scratchpad Buffers system software shall reserve for the xHC. Refer to
section 4.20 for more information.
*/
#define XHCI_MAX_SCRATCHPAD_BUFS_LO(regs) ((regs->hcsparams2 >> 27) & 0x1F)

// Combination of low and high bit macros for maximum scratchpad buffer count
#define XHCI_MAX_SCRATCHPAD_BUFFERS(regs) ((XHCI_MAX_SCRATCHPAD_BUFS_HI(regs) << 5) | XHCI_MAX_SCRATCHPAD_BUFS_LO(regs))

/*
// xHci Spec Section 5.3.5 Table 5-12: Host Controller Structural Parameters 3 (HCSPARAMS3) (page 350)

U1 Device Exit Latency. Worst case latency to transition a root hub Port Link State
(PLS) from U1 to U0. Applies to all root hub ports.
The following are permissible values:
    Value Description
    00h Zero
    01h Less than 1 µs
    02h Less than 2 µs.
    …
    0Ah Less than 10 µs.
    0B-FFh Reserved
*/
#define XHCI_U1_DEVICE_EXIT_LATENCY(regs) ((regs->hcsparams3) & 0xFF)

/*
// xHci Spec Section 5.3.5 Table 5-12: Host Controller Structural Parameters 3 (HCSPARAMS3) (page 350)

U2 Device Exit Latency. Worst case latency to transition from U2 to U0. Applies to
all root hub ports.
The following are permissible values:
    Value Description
    0000h Zero
    0001h Less than 1 µs.
    0002h Less than 2 µs.
    …
    07FFh Less than 2047 µs.
    0800-FFFFh Reserved
*/
#define XHCI_U2_DEVICE_EXIT_LATENCY(regs) ((regs->hcsparams3 >> 16) & 0xFFFF)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 351)

64-bit Addressing Capability77 (AC64). This flag documents the addressing range capability of this
implementation. The value of this flag determines whether the xHC has implemented the high order 32
bits of 64 bit register and data structure pointer fields. Values for this flag have the following
interpretation:
    Value Description
    0 32-bit address memory pointers implemented
    1 64-bit address memory pointers implemented
If 32-bit address memory pointers are implemented, the xHC shall ignore the high order 32 bits of 64-
bit data structure pointer fields, and system software shall ignore the high order 32 bits of 64-bit xHC
registers.
*/
#define XHCI_AC64(regs) ((regs->hccparams1) & 0x1)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 351)

BW Negotiation Capability (BNC). This flag identifies whether the xHC has implemented the
Bandwidth Negotiation. Values for this flag have the following interpretation:
    Value Description
    0 BW Negotiation not implemented
    1 BW Negotiation implemented
Refer to section 4.16 for more information on Bandwidth Negotiation.
*/
#define XHCI_BNC(regs) ((regs->hccparams1 >> 1) & 0x1)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 351)

Context Size (CSZ). If this bit is set to ‘1’, then the xHC uses 64 byte Context data structures. If this
bit is cleared to ‘0’, then the xHC uses 32 byte Context data structures.
This flag does not apply to Stream Contexts
*/
#define XHCI_CSZ(regs) ((regs->hccparams1 >> 2) & 0x1)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 351)

Port Power Control (PPC). This flag indicates whether the host controller implementation includes
port power control. A ‘1’ in this bit indicates the ports have port power switches. A ‘0’ in this bit
indicates the port do not have port power switches. The value of this flag affects the functionality of
the PP flag in each port status and control register (refer to Section 5.4.8).
*/
#define XHCI_PPC(regs) ((regs->hccparams1 >> 3) & 0x1)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 352)

Port Indicators (PIND). This bit indicates whether the xHC root hub ports support port indicator
control. When this bit is a ‘1’, the port status and control registers include a read/writeable field for
controlling the state of the port indicator. Refer to Section 5.4.8 for definition of the Port Indicator
Control field.
*/
#define XHCI_PIND(regs) ((regs->hccparams1 >> 4) & 0x1)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 352)

Light HC Reset Capability (LHRC). This flag indicates whether the host controller implementation
supports a Light Host Controller Reset. A ‘1’ in this bit indicates that Light Host Controller Reset is
supported. A ‘0’ in this bit indicates that Light Host Controller Reset is not supported. The value of this
flag affects the functionality of the Light Host Controller Reset (LHCRST) flag in the USBCMD register
(refer to Section 5.4.1).
*/
#define XHCI_LHRC(regs) ((regs->hccparams1 >> 5) & 0x1)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 352)

Latency Tolerance Messaging Capability (LTC). This flag indicates whether the host controller
implementation supports Latency Tolerance Messaging (LTM). A ‘1’ in this bit indicates that LTM is
supported. A ‘0’ in this bit indicates that LTM is not supported. Refer to section 4.13.1 for more
information on LTM.
*/
#define XHCI_LTC(regs) ((regs->hccparams1 >> 6) & 0x1)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 352)

No Secondary SID Support (NSS). This flag indicates whether the host controller implementation
supports Secondary Stream IDs. A ‘1’ in this bit indicates that Secondary Stream ID decoding is not
supported. A ‘0’ in this bit indicates that Secondary Stream ID decoding is supported. (refer to
Sections 4.12.2 and 6.2.3).
*/
#define XHCI_NSS(regs) ((regs->hccparams1 >> 7) & 0x1)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 352)

Parse All Event Data (PAE). This flag indicates whether the host controller implementation Parses all
Event Data TRBs while advancing to the next TD after a Short Packet, or it skips all but the first Event
Data TRB. A ‘1’ in this bit indicates that all Event Data TRBs are parsed. A ‘0’ in this bit indicates that
only the first Event Data TRB is parsed (refer to section 4.10.1.1).
*/
#define XHCI_PAE(regs) ((regs->hccparams1 >> 8) & 0x1)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 352)

Stopped - Short Packet Capability (SPC). This flag indicates that the host controller
implementation is capable of generating a Stopped - Short Packet Completion Code. Refer to section
4.6.9 for more information.
*/
#define XHCI_SPC(regs) ((regs->hccparams1 >> 9) & 0x1)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 352)

Stopped EDTLA Capability (SEC). This flag indicates that the host controller implementation Stream
Context support a Stopped EDTLA field. Refer to sections 4.6.9, 4.12, and 6.4.4.1 for more
information.
Stopped EDTLA Capability support (that is, SEC = '1') shall be mandatory for all xHCI 1.1 and xHCI 1.2
compliant xHCs.
*/
#define XHCI_SEC(regs) ((regs->hccparams1 >> 10) & 0x1)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 352)

Contiguous Frame ID Capability (CFC). This flag indicates that the host controller implementation
is capable of matching the Frame ID of consecutive Isoch TDs. Refer to section 4.11.2.5 for more
information.
*/
#define XHCI_CFC(regs) ((regs->hccparams1 >> 11) & 0x1)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 352)

Maximum Primary Stream Array Size (MaxPSASize). This fields identifies the maximum size
Primary Stream Array that the xHC supports. The Primary Stream Array size = 2MaxPSASize+1. Valid
MaxPSASize values are 0 to 15, where ‘0’ indicates that Streams are not supported.
*/
#define XHCI_MAXPSASIZE(regs) ((regs->hccparams1 >> 12) & 0xF)

/*
// xHci Spec Section 5.3.6 Table 5-13: Host Controller Capability 1 Parameters (HCCPARAMS1) (page 352)

xHCI Extended Capabilities Pointer (xECP). This field indicates the existence of a capabilities list.
The value of this field indicates a relative offset, in 32-bit words, from Base to the beginning of the
first extended capability.
For example, using the offset of Base is 1000h and the xECP value of 0068h, we can calculated the
following effective address of the first extended capability:
1000h + (0068h << 2) -> 1000h + 01A0h -> 11A0h
*/
#define XHCI_XECP(regs) ((regs->hccparams1 >> 16) & 0xFFFF)

/*
// xHci Spec Section 5.3.9 Table 5-16: Host Controller Capability Parameters 2 (HCCPARAMS2) (page 354)

U3 Entry Capability (U3C) - RO. This bit indicates whether the xHC Root Hub ports
support port Suspend Complete notification. When this bit is '1', PLC shall be
asserted on any transition of PLS to the U3 State. Refer to section 4.15.1 for more
information.
*/
#define XHCI_U3C(regs) ((regs->hccparams2) & 0x1)

/*
// xHci Spec Section 5.3.9 Table 5-16: Host Controller Capability Parameters 2 (HCCPARAMS2) (page 354)

Configure Endpoint Command Max Exit Latency Too Large Capability (CMC) -
RO. This bit indicates whether a Configure Endpoint Command is capable of
generating a Max Exit Latency Too Large Capability Error. When this bit is '1', a Max
Exit Latency Too Large Capability Error may be returned by a Configure Endpoint
Command. When this bit is '0', a Max Exit Latency Too Large Capability Error shall
not be returned by a Configure Endpoint Command. This capability is enabled by the
CME flag in the USBCMD register. Refer to sections 4.23.5.2 and 5.4.1 for more
information.
*/
#define XHCI_CMC(regs) ((regs->hccparams2 >> 1) & 0x1)

/*
// xHci Spec Section 5.3.9 Table 5-16: Host Controller Capability Parameters 2 (HCCPARAMS2) (page 354)

Force Save Context Capability (FSC) - RO. This bit indicates whether the xHC
supports the Force Save Context Capability. When this bit is '1', the Save State
operation shall save any cached Slot, Endpoint, Stream or other Context information
to memory. Refer to Implementation Note “FSC and Context handling by Save and
Restore”, and sections 4.23.2 and 5.4.1 for more information.
*/
#define XHCI_FSC(regs) ((regs->hccparams2 >> 2) & 0x1)

/*
// xHci Spec Section 5.3.9 Table 5-16: Host Controller Capability Parameters 2 (HCCPARAMS2) (page 354)

Compliance Transition Capability (CTC) - RO. This bit indicates whether the xHC
USB3 Root Hub ports support the Compliance Transition Enabled (CTE) flag. When
this bit is ‘1’, USB3 Root Hub port state machine transitions to the Compliance
substate shall be explicitly enabled software. When this bit is ‘0’, USB3 Root Hub port
state machine transitions to the Compliance substate are automatically enabled.
Refer to section 4.19.1.2.4.1 for more information.
*/
#define XHCI_CTC(regs) ((regs->hccparams2 >> 3) & 0x1)

/*
// xHci Spec Section 5.3.9 Table 5-16: Host Controller Capability Parameters 2 (HCCPARAMS2) (page 355)

Large ESIT Payload Capability (LEC) - RO. This bit indicates whether the xHC
supports ESIT Payloads greater than 48K bytes. When this bit is ‘1’, ESIT Payloads
greater than 48K bytes are supported. When this bit is ‘0’, ESIT Payloads greater
than 48K bytes are not supported. Refer to section 6.2.3.8 for more information
*/
#define XHCI_LEC(regs) ((regs->hccparams2 >> 4) & 0x1)

/*
// xHci Spec Section 5.3.9 Table 5-16: Host Controller Capability Parameters 2 (HCCPARAMS2) (page 355)

Configuration Information Capability (CIC) - RO. This bit indicates if the xHC
supports extended Configuration Information. When this bit is 1, the Configuration
Value, Interface Number, and Alternate Setting fields in the Input Control Context
are supported. When this bit is 0, the extended Input Control Context fields are not
supported. Refer to section 6.2.5.1 for more information.
*/
#define XHCI_CIC(regs) ((regs->hccparams2 >> 5) & 0x1)

/*
// xHci Spec Section 5.3.9 Table 5-16: Host Controller Capability Parameters 2 (HCCPARAMS2) (page 355)

Extended TBC Capability78 (ETC) - RO. This bit indicates if the TBC field in an
Isoch TRB supports the definition of Burst Counts greater than 65535 bytes. When
this bit is ‘1’, the Extended EBC capability is supported by the xHC. When this bit is
‘0’, it is not. Refer to section 4.11.2.3 for more information.
*/
#define XHCI_ETC(regs) ((regs->hccparams2 >> 6) & 0x1)

/*
// xHci Spec Section 5.3.9 Table 5-16: Host Controller Capability Parameters 2 (HCCPARAMS2) (page 355)

Extended TBC TRB Status Capability (ETC_TSC) - RO. This bit indicates if the
TBC/TRBSts field in an Isoch TRB indicates additional information regarding TRB in
the TD. When this bit is ‘1’, the Isoch TRB TD Size/TBC field presents TBC value and
TBC/TRBSts field presents the TRBSts value. When this bit is ‘0’ then the ETC/ETE
values defines the TD Size/TBC field and TBC/RsvdZ field. This capability shall be
enabled only if LEC = ‘1’ and ETC=’1’. Refer to section 4.11.2.3 for more information.
*/
#define XHCI_ETC_TSC(regs) ((regs->hccparams2 >> 7) & 0x1)

/*
// xHci Spec Section 5.3.9 Table 5-16: Host Controller Capability Parameters 2 (HCCPARAMS2) (page 355)

Get/Set Extended Property Capability (GSC) – RO. This bit indicates support for
the Set Extended Property and Get Extended Property commands. When this bit is
‘1’, the xHC supports the Get Extended Property and Set Extended Property
commands defined in section 4.6.17 and section 4.6.18. When this bit is ‘0’, the xHC
does not support the Get Extended Property and Set Extended Property commands
and the xHC does not support any of the associated Extended Capabilities.
This bit shall only be set to ‘1’ if the xHC supports one or more extended capabilities
that require the Get Extended Property and Set Extended Property commands.
*/
#define XHCI_GSC(regs) ((regs->hccparams2 >> 8) & 0x1)

/*
// xHci Spec Section 5.3.9 Table 5-16: Host Controller Capability Parameters 2 (HCCPARAMS2) (page 355)

Virtualization Based Trusted I/O Capability (VTC) – RO. This bit when set to
1, indicates that the xHC supports the Virtualization based Trusted IO (VTIO)
Capability. When this bit is 0, the VTIO Capability is not supported. This capability is
enabled by the VTIOE flag in the USBCMD register.
*/
#define XHCI_VTC(regs) ((regs->hccparams2 >> 9) & 0x1)

/*
// xHci Spec Section 5.4.7 Table 5-26: Configure Register Bit Definitions (CONFIG) (page 369)

Max Device Slots Enabled (MaxSlotsEn) – RW. Default = ‘0’. This field specifies
the maximum number of enabled Device Slots. Valid values are in the range of 0 to
MaxSlots. Enabled Devices Slots are allocated contiguously. For example, a value of
16 specifies that Device Slots 1 to 16 are active. A value of ‘0’ disables all Device
Slots. A disabled Device Slot shall not respond to Doorbell Register references.
This field shall not be modified by software if the xHC is running (Run/Stop (R/S) =
‘1’).
*/
#define XHCI_MAX_SLOTS_EN(config) ((config) & 0xFF)

// Returns the value of config with max device slots set to the given value
#define XHCI_SET_MAX_SLOTS_EN(config, slots) (((config) & ~0xFF) | ((slots) & 0xFF))

/*
// xHci Spec Section 5.4.7 Table 5-26: Configure Register Bit Definitions (CONFIG) (page 369)

U3 Entry Enable (U3E) – RW. Default = '0'. When set to '1', the xHC shall assert
the PLC flag ('1') when a Root Hub port transitions to the U3 State. Refer to section
4.15.1 for more information.
*/
#define XHCI_U3_ENTRY_ENABLE(config) (((config) >> 8) & 0x1)

/*
// xHci Spec Section 5.4.7 Table 5-26: Configure Register Bit Definitions (CONFIG) (page 369)

Configuration Information Enable (CIE) - RW. Default = '0'. When set to '1', the
software shall initialize the Configuration Value, Interface Number, and Alternate
Setting fields in the Input Control Context when it is associated with a Configure
Endpoint Command. When this bit is '0', the extended Input Control Context fields
are not supported. Refer to section 6.2.5.1 for more information.
*/
#define XHCI_CONFIG_INFO_ENABLE(config) (((config) >> 9) & 0x1)

/*
// xHci Spec Section 5.5.2.1 Table 5-38: Interrupter Management Register Bit Definitions (IMAN) (page 425)

Interrupt Pending (IP) - RW1C. Default = ‘0’. This flag represents the current state of the
Interrupter. If IP = ‘1’, an interrupt is pending for this Interrupter. A ‘0’ value indicates that no
interrupt is pending for the Interrupter. Refer to section 4.17.3 for the conditions that modify
the state of this flag.
*/
#define XHCI_IMAN_INTERRUPT_PENDING (1 << 0)

/*
// xHci Spec Section 5.5.2.1 Table 5-38: Interrupter Management Register Bit Definitions (IMAN) (page 425)

Interrupt Enable (IE) – RW. Default = ‘0’. This flag specifies whether the Interrupter is capable of
generating an interrupt. When this bit and the IP bit are set (‘1’), the Interrupter shall generate
an interrupt when the Interrupter Moderation Counter reaches ‘0’. If this bit is ‘0’, then the
Interrupter is prohibited from generating interrupts.
*/
#define XHCI_IMAN_INTERRUPT_ENABLE (1 << 1)

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 469)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : no
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_RESERVED  0

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 469)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : no
Transfer Ring : yes
*/
#define XHCI_TRB_TYPE_NORMAL  1

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 469)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : no
Transfer Ring : yes
*/
#define XHCI_TRB_TYPE_SETUP_STAGE  2

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 469)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : no
Transfer Ring : yes
*/
#define XHCI_TRB_TYPE_DATA_STAGE  3

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 469)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : no
Transfer Ring : yes
*/
#define XHCI_TRB_TYPE_STATUS_STAGE  4

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : no
Transfer Ring : yes
*/
#define XHCI_TRB_TYPE_ISOCH  5

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : yes
*/
#define XHCI_TRB_TYPE_LINK  6

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : no
Transfer Ring : yes
*/
#define XHCI_TRB_TYPE_EVENT_DATA  7

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : no
Transfer Ring : yes
*/
#define XHCI_TRB_TYPE_NOOP  8

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_ENABLE_SLOT_CMD  9

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_DISABLE_SLOT_CMD  10

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD  11

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD  12

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD  13

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_RESET_ENDPOINT_CMD  14

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_STOP_ENDPOINT_CMD  15

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_SET_TR_DEQUEUE_PTR_CMD  16

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_RESET_DEVICE_CMD  17

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no

Note: (Optional, used with virtualization only)
*/
#define XHCI_TRB_TYPE_FORCE_EVENT_CMD  18

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no

Note: (Optional)
*/
#define XHCI_TRB_TYPE_NEGOTIATE_BANDWIDTH_CMD  19

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no

Note: (Optional)
*/
#define XHCI_TRB_TYPE_SET_LATENCY_TOLERANCE_VALUE_CMD  20

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no

Note: (Optional)
*/
#define XHCI_TRB_TYPE_GET_PORT_BANDWIDTH_CMD  21

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_FORCE_HEADER_CMD  22

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_NOOP_CMD  23

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no

Note: (Optional)
*/
#define XHCI_TRB_TYPE_GET_EXTENDED_PROPERTY_CMD  24

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 470)
Allowed TRB Types
-----------------
Command Ring  : yes
Event Ring    : no
Transfer Ring : no

Note: (Optional)
*/
#define XHCI_TRB_TYPE_SET_EXTENDED_PROPERTY_CMD  25

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 471)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : yes
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_TRANSFER_EVENT  32

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 471)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : yes
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_CMD_COMPLETION_EVENT  33

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 471)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : yes
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_PORT_STATUS_CHANGE_EVENT  34

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 471)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : yes
Transfer Ring : no

Note: (Optional)
*/
#define XHCI_TRB_TYPE_BANDWIDTH_REQUEST_EVENT  35

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 471)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : yes
Transfer Ring : no

Note: (Optional, used width virtualization only)
*/
#define XHCI_TRB_TYPE_DOORBELL_EVENT  36

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 471)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : yes
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_HOST_CONTROLLER_EVENT  37

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 471)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : yes
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_DEVICE_NOTIFICATION_EVENT  38

/*
// xHci Spec Section 6.4.6 TRB Types Table 6-91: TRB Type Definitions (page 471)
Allowed TRB Types
-----------------
Command Ring  : no
Event Ring    : yes
Transfer Ring : no
*/
#define XHCI_TRB_TYPE_MFINDEX_WRAP_EVENT  39

// The Cycle Bit for a TRB, typically set to 1 for an initialized TRB
// (TO-DO: find exact spec page with documentation)
#define XHCI_TRB_CYCLE_BIT  0x1

// The Toggle Cycle bit for a Link TRB, used to toggle the Cycle Bit
// (TO-DO: find exact spec page with documentation)
#define XHCI_TRB_TOGGLE_CYCLE 0x2

/*
// xHci Spec Section 5.4.5 Table 5-24: Command Ring Control Register Bit Definitions (CRCR) (page 366)

Ring Cycle State (RCS) - RW. This bit identifies the value of the xHC Consumer
Cycle State (CCS) flag for the TRB referenced by the Command Ring Pointer. Refer to
section 4.9.3 for more information.

Writes to this flag are ignored if Command Ring Running (CRR) is ‘1’.

If the CRCR is written while the Command Ring is stopped (CRR = ‘0’), then the
value of this flag shall be used to fetch the first Command TRB the next time the Host
Controller Doorbell register is written with the DB Reason field set to Host Controller
Command.

If the CRCR is not written while the Command Ring is stopped (CRR = ‘0’), then the
Command Ring shall begin fetching Command TRBs using the current value of the
internal Command Ring CCS flag.

Reading this flag always returns ‘0’
*/
#define XHCI_CRCR_RING_CYCLE_STATE      (1 << 0)

/*
// xHci Spec Section 5.4.5 Table 5-24: Command Ring Control Register Bit Definitions (CRCR) (page 366)

Command Stop (CS) - RW1S. Default = ‘0’. Writing a ‘1’ to this bit shall stop the
operation of the Command Ring after the completion of the currently executing
command and generate a Command Completion Event with the Completion Code set
to Command Ring Stopped and the Command TRB Pointer set to the current value of
the Command Ring Dequeue Pointer. Refer to section 4.6.1.1 for more information on
stopping a command.

The next write to the Host Controller Doorbell with DB Reason field set to Host
Controller Command shall restart the Command Ring operation.

Writes to this flag are ignored by the xHC if Command Ring Running (CRR) = ‘0’.

Reading this bit shall always return ‘0’.
*/
#define XHCI_CRCR_COMMAND_STOP          (1 << 1)

/*
// xHci Spec Section 5.4.5 Table 5-24: Command Ring Control Register Bit Definitions (CRCR) (page 367)

Command Abort (CA) - RW1S. Default = ‘0’. Writing a ‘1’ to this bit shall
immediately terminate the currently executing command, stop the Command Ring,
and generate a Command Completion Event with the Completion Code set to
Command Ring Stopped. Refer to section 4.6.1.2 for more information on aborting a
command.
The next write to the Host Controller Doorbell with DB Reason field set to Host
Controller Command shall restart the Command Ring operation.
Writes to this flag are ignored by the xHC if Command Ring Running (CRR) = ‘0’.
Reading this bit always returns ‘0’.
*/
#define XHCI_CRCR_COMMAND_ABORT          (1 << 2)

/*
// xHci Spec Section 5.4.5 Table 5-24: Command Ring Control Register Bit Definitions (CRCR) (page 367)

Command Ring Running (CRR) - RO. Default = 0. This flag is set to ‘1’ if the
Run/Stop (R/S) bit is ‘1’ and the Host Controller Doorbell register is written with the
DB Reason field set to Host Controller Command. It is cleared to ‘0’ when the
Command Ring is “stopped” after writing a ‘1’ to the Command Stop (CS) or
Command Abort (CA) flags, or if the R/S bit is cleared to ‘0’
*/
#define XHCI_CRCR_COMMAND_RING_RUNNING   (1 << 3)

// This macro defines the bit position in the control field of a Transfer
// Request Block (TRB) where the TRB type field begins. The xHCI specification
// requires that the TRB type be placed in a specific position within the control
// field of a TRB. This macro is used to shift the TRB type value to the correct
// position when preparing a TRB.
// (TO-DO: find spec page)
#define XHCI_TRB_TYPE_SHIFT 10

// TRB Type field mask and shift
// (TO-DO: find spec page)
#define XHCI_TRB_TYPE_MASK  0xFC00 // Mask for the TRB type field

// Slot ID field mask and shift (in the status field of a Command Completion Event TRB)
// (TO-DO: find spec page)
#define XHCI_SLOT_ID_MASK   0x3F000000

// Shift for the Slot ID field
// (TO-DO: find spec page)
#define XHCI_SLOT_ID_SHIFT  24

// Completion Code field mask and shift (in the status field of a Command Completion Event TRB)
// (TO-DO: find spec page)
#define XHCI_COMPLETION_CODE_MASK   0xFF0000

// Shift for the Completion Code field
// (TO-DO: find spec page)
#define XHCI_COMPLETION_CODE_SHIFT  16

/*
// xHci Spec Section 6.4.5 Table 6-90: TRB Completion Code Definitions (page 507)

The following TRB Completion Status codes will be asserted by the Host
Controller during status update if the associated error condition is detected.
*/
#define XHCI_TRB_COMPLETION_CODE_INVALID                0
#define XHCI_TRB_COMPLETION_CODE_SUCCESS                1
#define XHCI_TRB_COMPLETION_CODE_DATA_BUFFER_ERROR      2
#define XHCI_TRB_COMPLETION_CODE_BABBLE_DETECTED_ERROR  3
#define XHCI_TRB_COMPLETION_CODE_USB_TRANSACTION_ERROR  4
#define XHCI_TRB_COMPLETION_CODE_TRB_ERROR              5
#define XHCI_TRB_COMPLETION_CODE_STALL_ERROR            6
#define XHCI_TRB_COMPLETION_CODE_RESOURCE_ERROR         7
#define XHCI_TRB_COMPLETION_CODE_BANDWIDTH_ERROR        8
#define XHCI_TRB_COMPLETION_CODE_NO_SLOTS_AVAILABLE     9
#define XHCI_TRB_COMPLETION_CODE_INVALID_STREAM_TYPE    10
#define XHCI_TRB_COMPLETION_CODE_SLOT_NOT_ENABLED       11
#define XHCI_TRB_COMPLETION_CODE_ENDPOINT_NOT_ENABLED   12
#define XHCI_TRB_COMPLETION_CODE_SHORT_PACKET           13
#define XHCI_TRB_COMPLETION_CODE_RING_UNDERRUN          14
#define XHCI_TRB_COMPLETION_CODE_RING_OVERRUN           15
#define XHCI_TRB_COMPLETION_CODE_VF_EVENT_RING_FULL     16
#define XHCI_TRB_COMPLETION_CODE_PARAMETER_ERROR        17
#define XHCI_TRB_COMPLETION_CODE_BANDWIDTH_OVERRUN      18
#define XHCI_TRB_COMPLETION_CODE_CONTEXT_STATE_ERROR    19
#define XHCI_TRB_COMPLETION_CODE_NO_PING_RESPONSE       20
#define XHCI_TRB_COMPLETION_CODE_EVENT_RING_FULL        21
#define XHCI_TRB_COMPLETION_CODE_INCOMPATIBLE_DEVICE    22
#define XHCI_TRB_COMPLETION_CODE_MISSED_SERVICE         23
#define XHCI_TRB_COMPLETION_CODE_COMMAND_RING_STOPPED   24
#define XHCI_TRB_COMPLETION_CODE_COMMAND_ABORTED        25
#define XHCI_TRB_COMPLETION_CODE_STOPPED                26
#define XHCI_TRB_COMPLETION_CODE_STOPPED_LENGTH_INVALID 27
#define XHCI_TRB_COMPLETION_CODE_STOPPED_SHORT_PACKET   28
#define XHCI_TRB_COMPLETION_CODE_MAX_EXIT_LATENCY_ERROR 29

// Helper macro to easily construct TRB command objects
#define XHCI_CONSTRUCT_CMD_TRB(type) XhciTrb_t { .parameter = 0, .status = 0, .control = type << XHCI_TRB_TYPE_SHIFT }

/*
// xHci Spec Section 7.1.1 USB Legacy Support Capability (USBLEGSUP) (page 519)
*/
#define XHCI_LEGACY_SUPPORT_CAP_ID 1
#define XHCI_LEGACY_BIOS_OWNED_SEMAPHORE (1 << 16)
#define XHCI_LEGACY_OS_OWNED_SEMAPHORE (1 << 24)

/*
// xHci Spec Section 7.1.2 USB Legacy Support Control/Status (USBLEGCTLSTS) (page 520)
*/
#define XHCI_LEGACY_SMI_ENABLE_BITS ((1 << 0) | (1 << 4) | (1 << 13) | (1 << 14) | (1 << 15))

/*
// xHci Spec Section 5.3 Table 5-9: eXtensible Host Controller Capability Registers (page 346)

These registers specify the limits and capabilities of the host controller
implementation.
All Capability Registers are Read-Only (RO). The offsets for these registers are
all relative to the beginning of the host controller’s MMIO address space. The
beginning of the host controller’s MMIO address space is referred to as “Base”.
*/
struct XhciCapabilityRegisters {
    const uint8_t caplength;    // Capability Register Length
    const uint8_t reserved0;
    const uint16_t hciversion;  // Interface Version Number
    const uint32_t hcsparams1;  // Structural Parameters 1
    const uint32_t hcsparams2;  // Structural Parameters 2
    const uint32_t hcsparams3;  // Structural Parameters 3
    const uint32_t hccparams1;  // Capability Parameters 1
    const uint32_t dboff;       // Doorbell Offset
    const uint32_t rtsoff;      // Runtime Register Space Offset
    const uint32_t hccparams2;  // Capability Parameters 2
};

static_assert(sizeof(XhciCapabilityRegisters) == 32);

/*
// xHci Spec Section 7.0 Table 7-1: Format of xHCI Extended Capability Pointer Register

The xHC exports xHCI-specific extended capabilities utilizing a method similar to
the PCI extended capabilities. If an xHC implements any extended capabilities, it
specifies a non-zero value in the xHCI Extended Capabilities Pointer (xECP) field
of the HCCPARAMS1 register (5.3.6). This value is an offset into xHC MMIO space
from the Base, where the Base is the beginning of the host controller’s MMIO
address space. Each capability register has the format illustrated in Table 7-1
*/
struct XhciExtendedCapabilityEntry {
    union {
        struct {
            /*
                This field identifies the xHCI Extended capability.
                Refer to Table 7-2 for a list of the valid xHCI extended capabilities.
            */
            uint8_t id;

            /*
                This field points to the xHC MMIO space offset of
                the next xHCI extended capability pointer. A value of 00h indicates the end of the extended
                capability list. A non-zero value in this register indicates a relative offset, in Dwords, from this
                Dword to the beginning of the next extended capability.
                
                For example, assuming an effective address of this data structure is 350h and assuming a
                pointer value of 068h, we can calculate the following effective address:
                350h + (068h << 2) -> 350h + 1A0h -> 4F0h
            */
            uint8_t next;

            /*
                The definition and attributes of these bits depends on the specific capability.
            */
           uint16_t capSpecific;
        };

        // Extended capability entries must be read as 32-bit words
        uint32_t raw;
    };
};

static_assert(sizeof(XhciExtendedCapabilityEntry) == 4);

/*
// xHci Spec Section 7.0 Table 7-1: Format of xHCI Extended Capability Pointer Register
*/
#define XHCI_NEXT_EXT_CAP_PTR(ptr, next) (volatile uint32_t*)((char*)ptr + (next * sizeof(uint32_t)))

/*
// xHci Spec Section 7.0 Table 7-2: xHCI Extended Capability Codes
*/
enum class XhciExtendedCapabilityCode {
    Reserved = 0,
    UsbLegacySupport = 1,
    SupportedProtocol = 2,
    ExtendedPowerManagement = 3,
    IOVirtualizationSupport = 4,
    MessageInterruptSupport = 5,
    LocalMemorySupport = 6,
    UsbDebugCapabilitySupport = 10,
    ExtendedMessageInterruptSupport = 17
};


/*
// xHci Spec Section 5.4 Table 5-18: Host Controller Operational Registers (page 356)

The base address of this register space is referred to as Operational Base.
The Operational Base shall be Dword aligned and is calculated by adding the
value of the Capability Registers Length (CAPLENGTH) register (refer to Section
5.3.1) to the Capability Base address. All registers are multiples of 32 bits in
length.
Unless otherwise stated, all registers should be accessed as a 32-bit width on
reads with an appropriate software mask, if needed. A software
read/modify/write mechanism should be invoked for partial writes.
These registers are located at a positive offset from the Capabilities Registers
(refer to Section 5.3).
*/
struct XhciOperationalRegisters {
    uint32_t usbcmd;        // USB Command
    uint32_t usbsts;        // USB Status
    uint32_t pagesize;      // Page Size
    uint32_t reserved0[2];
    uint32_t dnctrl;        // Device Notification Control
    uint64_t crcr;          // Command Ring Control
    uint32_t reserved1[4];
    uint64_t dcbaap;        // Device Context Base Address Array Pointer
    uint32_t config;        // Configure
    uint32_t reserved2[49];
    // Port Register Set offset has to be calculated dynamically based on MAXPORTS
};

/*
// xHci Spec Section 4.11 Figure 4-13: TRB Template (page 188)

This section discusses the properties and uses of TRBs that are outside of the
scope of the general data structure descriptions that are provided in section
6.4.
*/
typedef struct XhciTransferRequestBlock {
    uint64_t parameter; // TRB-specific parameter
    uint32_t status;    // Status information
    union {
        struct {
            uint32_t cycleBit               : 1;
            uint32_t evalNextTrb            : 1;
            uint32_t interruptOnShortPkt    : 1;
            uint32_t noSnoop                : 1;
            uint32_t chainBit               : 1;
            uint32_t interruptOnCompletion  : 1;
            uint32_t immediateData          : 1;
            uint32_t rsvd0                  : 2;
            uint32_t blockEventInterrupt    : 1;
            uint32_t trbType                : 6;
            uint32_t rsvd1                  : 16;
        };
        uint32_t control;   // Control bits, including the TRB type
    };
} XhciTrb_t;
static_assert(sizeof(XhciTrb_t) == sizeof(uint32_t) * 4);

typedef struct XhciAddressDeviceRequestBlock {
    uint64_t inputContextPhysicalBase;
    uint32_t rsvd;
    struct {
        uint32_t cycleBit   : 1;
        uint32_t rsvd1      : 8;

        /*
            Block Set Address Request (BSR). When this flag is set to ‘0’ the Address Device Command shall
            generate a USB SET_ADDRESS request to the device. When this flag is set to ‘1’ the Address
            Device Command shall not generate a USB SET_ADDRESS request. Refer to section 4.6.5 for
            more information on the use of this flag.
        */
        uint32_t bsr        : 1; // Block Set Address Request bit
        
        uint32_t trbType    : 6;
        uint32_t rsvd2      : 8;
        uint32_t slotId     : 8;
    };
} XhciAddressDeviceCommandTrb_t;
static_assert(sizeof(XhciAddressDeviceCommandTrb_t) == sizeof(uint32_t) * 4);

typedef struct XhciCommandCompletionRequestBlock {
    uint64_t commandTrbPointer;
    struct {
        uint32_t rsvd0          : 24;
        uint32_t completionCode : 8;
    };
    struct {
        uint32_t cycleBit   : 1;
        uint32_t rsvd1      : 9;
        uint32_t trbType    : 6;
        uint32_t vfid       : 8;
        uint32_t slotId     : 8;
    };
} XhciCommandCompletionTrb_t;
static_assert(sizeof(XhciCommandCompletionTrb_t) == sizeof(uint32_t) * 4);

typedef struct XhciSetupDataStageCompletionRequestBlock {
    uint64_t commandTrbPointer;
    struct {
        uint32_t bytesTransfered    : 24;
        uint32_t completionCode     : 8;
    };
    struct {
        uint32_t cycleBit   : 1;
        uint32_t rsvd1      : 9;
        uint32_t trbType    : 6;
        uint32_t vfid       : 8;
        uint32_t slotId     : 8;
    };
} XhciSetupDataStageCompletionTrb_t;
static_assert(sizeof(XhciSetupDataStageCompletionTrb_t) == sizeof(uint32_t) * 4);

typedef struct XhciPortStatusChangeRequestBlock {
    struct {
        uint32_t rsvd0  : 24;
        uint32_t portId : 8;
    };
    uint32_t rsvd1;
    struct {
        uint32_t rsvd2          : 24;
        uint32_t completionCode : 8;
    };
    struct {
        uint32_t cycleBit  : 1;
        uint32_t rsvd3     : 9;
        uint32_t trbType   : 6;
        uint32_t rsvd4     : 16;
    };
} XhciPortStatusChangeTrb_t;
static_assert(sizeof(XhciPortStatusChangeTrb_t) == sizeof(uint32_t) * 4);

/*
// xHci Spec Section 4.11.2.2 Figure 4-14 SETUP Data, the Parameter Component of Setup Stage TRB (page 211)
*/
struct XhciDeviceRequestPacket {
    union {
        struct {
            /*
                Recipient Values:
                0       = Device
                1       = Interface
                2       = Endpoint
                3       = Other
                4..31   = Reserved
            */
            uint8_t recipient           : 5;

            /*
                Type Values:
                0       = Standard
                1       = Class
                2       = Vendor
                3       = Reserved
            */
            uint8_t type                : 2;

            /*
                Direction Values:
                0       = Host to Device
                1       = Device to Host
            */
            uint8_t transferDirection   : 1;
        };

        uint8_t bRequestType;
    };

    // Desired request
    uint8_t bRequest;

    // Request-specific
    uint16_t wValue;

    // Request-specific
    uint16_t wIndex;

    // Number of bytes to transfer if in data phase
    uint16_t wLength;
};
static_assert(sizeof(XhciDeviceRequestPacket) == 8);

/*
// xHci Spec Section 6.4.1.2.1 Setup Stage TRB (page 468)

A Setup Stage TRB is created by system software to initiate a USB Setup packet
on a control endpoint. Refer to section 3.2.9 for more information on Setup
Stage TRBs and the operation of control endpoints. Also refer to section 8.5.3 in
the USB2 spec. for a description of “Control Transfers”.
*/
typedef struct XhciSetupStageTransferRequestBlock {
    XhciDeviceRequestPacket requestPacket;
    
    struct {
        // Always 8
        uint32_t trbTransferLength  : 17;

        // Reserved
        uint32_t rsvd0              : 5;

        /*
            This field defines the index of the Interrupter that will receive events
            generated by this TRB. Valid values are between 0 and MaxIntrs-1.
        */
        uint32_t interrupterTarget  : 10;
    };

    struct {
        // This bit is used to mark the Enqueue point of a Transfer ring
        uint32_t cycleBit       : 1;

        // Reserved
        uint32_t rsvd1          : 4;

        /*
            Interrupt On Completion (IOC). If this bit is set to ‘1’, it specifies that when this TRB
            completes, the Host Controller shall notify the system of the completion by placing an
            Event TRB on the Event ring and sending an interrupt at the next interrupt threshold.
            Refer to section 4.10.4.
        */
        uint32_t ioc             : 1;

        /*
            Immediate Data (IDT). This bit shall be set to ‘1’ in a Setup Stage TRB.
            It specifies that the Parameter component of this TRB contains Setup Data.
        */
        uint32_t idt            : 1;

        // Reserved
        uint32_t rsvd2          : 3;

        /*
            TRB Type. This field is set to Setup Stage TRB type.
            Refer to Table 6-91 for the definition of the Type TRB IDs.
        */
        uint32_t trbType        : 6;

        /*
            Transfer Type (TRT). This field indicates the type and direction of the control transfer.
            Value Definition
            0 No Data Stage
            1 Reserved
            2 OUT Data Stage
            3 IN Data Stage
            Refer to section 4.11.2.2 for more information on the use of TRT.
        */
        uint32_t trt            : 2;

        // Reserved
        uint32_t rsvd3          : 14;
    };
} XhciSetupStageTrb_t;
static_assert(sizeof(XhciSetupStageTrb_t) == sizeof(uint32_t) * 4);

/*
// xHci Spec Section 6.4.1.2.2 Data Stage TRB Figure 6-10: Data Stage TRB (page 470)

A Data Stage TRB is used generate the Data stage transaction of a USB Control
transfer. Refer to section 3.2.9 for more information on Control transfers and
the operation of control endpoints. Also refer to section 8.5.3 in the USB2 spec.
for a description of “Control Transfers”.
*/
typedef struct XhciDataStageTransferRequestBlock {
    /*
        Data Buffer Pointer Hi and Lo. These fields represent the 64-bit address of the Data
        buffer area for this transaction.

        The memory structure referenced by this physical memory pointer is allowed to begin on
        a byte address boundary. However, user may find other alignments, such as 64-byte or
        128-byte alignments, to be more efficient and provide better performance.
    */
    uint64_t dataBuffer;

    struct {
        /*
            TRB Transfer Length. For an OUT, this field is the number of data bytes the xHC will send
            during the execution of this TRB.

            For an IN, the initial value of the field identifies the size of the data buffer referenced
            by the Data Buffer Pointer, i.e. the number of bytes the host expects the endpoint to deliver.
            Valid values are 1 to 64K.
        */
        uint32_t trbTransferLength  : 17;

        /*
            TD Size. This field provides an indicator of the number of packets remaining in the TD.
            Refer to section 4.11.2.4 for how this value is calculated.
        */
        uint32_t tdSize             : 5;

        /*
            This field defines the index of the Interrupter that will receive events
            generated by this TRB. Valid values are between 0 and MaxIntrs-1.
        */
        uint32_t interrupterTarget  : 10;
    };

    struct {
        // This bit is used to mark the Enqueue point of a Transfer ring
        uint32_t cycleBit       : 1;

        /*
            Evaluate Next TRB (ENT). If this flag is ‘1’ the xHC shall fetch and evaluate the
            next TRB before saving the endpoint state. Refer to section 4.12.3 for more information.
        */
        uint32_t ent            : 1;

        /*
            Interrupt-on Short Packet (ISP). If this flag is ‘1’ and a Short Packet is encountered
            for this TRB (i.e., less than the amount specified in TRB Transfer Length), then a
            Transfer Event TRB shall be generated with its Completion Code set to Short Packet.
            The TRB Transfer Length field in the Transfer Event TRB shall reflect the residual
            number of bytes not transferred into the associated data buffer. In either case, when
            a Short Packet is encountered, the TRB shall be retired without error and the xHC shall
            advance to the Status Stage TD.

            Note: if the ISP and IOC flags are both ‘1’ and a Short Packet is detected, then only one
            Transfer Event TRB shall be queued to the Event Ring. Also refer to section 4.10.1.1.
        */
        uint32_t isp            : 1;

        /*
            No Snoop (NS). When set to ‘1’, the xHC is permitted to set the No Snoop bit in the
            Requester Attributes of the PCIe transactions it initiates if the PCIe configuration
            Enable No Snoop flag is also set.
            
            When cleared to ‘0’, the xHC is not permitted to set PCIe packet No Snoop Requester
            Attribute. Refer to section 4.18.1 for more information.

            NOTE: If software sets this bit, then it is responsible for maintaining cache consistency.
        */
        uint32_t noSnoop        : 1;

        /*
            Chain bit (CH). Set to ‘1’ by software to associate this TRB with the next TRB on the Ring.
            A Data Stage TD is defined as a Data Stage TRB followed by zero or more Normal TRBs.
            The Chain bit is used to identify a multi-TRB Data Stage TD.
            
            The Chain bit is always ‘0’ in the last TRB of a Data Stage TD.
        */
        uint32_t chain          : 1;

        /*
            Interrupt On Completion (IOC). If this bit is set to ‘1’, it specifies that when this TRB
            completes, the Host Controller shall notify the system of the completion by placing an
            Event TRB on the Event ring and sending an interrupt at the next interrupt threshold.
            Refer to section 4.10.4.
        */
        uint32_t ioc             : 1;

        /*
            Immediate Data (IDT). This bit shall be set to ‘1’ in a Setup Stage TRB.
            It specifies that the Parameter component of this TRB contains Setup Data.
        */
        uint32_t idt            : 1;

        // Reserved
        uint32_t rsvd0          : 3;

        /*
            TRB Type. This field is set to Setup Stage TRB type.
            Refer to Table 6-91 for the definition of the Type TRB IDs.
        */
        uint32_t trbType        : 6;

        /*
            Direction (DIR). This bit indicates the direction of the data transfer as defined in the
            Data State TRB Direction column of Table 7. If cleared to ‘0’, the data stage transfer
            direction is OUT (Write Data).
            
            If set to ‘1’, the data stage transfer direction is IN (Read Data).
            Refer to section 4.11.2.2 for more information on the use of DIR.
        */
        uint32_t dir            : 1;

        // Reserved
        uint32_t rsvd1          : 15;
    };
} XhciDataStageTrb_t;
static_assert(sizeof(XhciDataStageTrb_t) == sizeof(uint32_t) * 4);

/*
// xHci Spec Section 6.4.1.2.3 Status Stage TRB Figure 6-11: Status Stage TRB (page 472).

A Status Stage TRB is used to generate the Status stage transaction of a USB
Control transfer. Refer to section 3.2.9 for more information on Control transfers
and the operation of control endpoints.
*/
typedef struct XhciStatusStageTransferRequestBlock {
    // Reserved
    uint64_t rsvd0;

    struct {
        // Reserved
        uint32_t rsvd1              : 22;

        /*
            This field defines the index of the Interrupter that will receive events
            generated by this TRB. Valid values are between 0 and MaxIntrs-1.
        */
        uint32_t interrupterTarget  : 10;
    };

    struct {
        // This bit is used to mark the Enqueue point of a Transfer ring
        uint32_t cycleBit       : 1;

        /*
            Evaluate Next TRB (ENT). If this flag is ‘1’ the xHC shall fetch and evaluate the
            next TRB before saving the endpoint state. Refer to section 4.12.3 for more information.
        */
        uint32_t ent            : 1;

        // Reserved
        uint32_t rsvd2          : 2;

        /*
            Chain bit (CH). Set to ‘1’ by software to associate this TRB with the next TRB on the Ring.
            A Data Stage TD is defined as a Data Stage TRB followed by zero or more Normal TRBs.
            The Chain bit is used to identify a multi-TRB Data Stage TD.
            
            The Chain bit is always ‘0’ in the last TRB of a Data Stage TD.
        */
        uint32_t chain          : 1;

        /*
            Interrupt On Completion (IOC). If this bit is set to ‘1’, it specifies that when this TRB
            completes, the Host Controller shall notify the system of the completion by placing an
            Event TRB on the Event ring and sending an interrupt at the next interrupt threshold.
            Refer to section 4.10.4.
        */
        uint32_t ioc            : 1;

        // Reserved
        uint32_t rsvd3          : 4;

        /*
            TRB Type. This field is set to Setup Stage TRB type.
            Refer to Table 6-91 for the definition of the Type TRB IDs.
        */
        uint32_t trbType        : 6;

        /*
            Direction (DIR). This bit indicates the direction of the data transfer as defined in the
            Data State TRB Direction column of Table 7. If cleared to ‘0’, the data stage transfer
            direction is OUT (Write Data).
            
            If set to ‘1’, the data stage transfer direction is IN (Read Data).
            Refer to section 4.11.2.2 for more information on the use of DIR.
        */
        uint32_t dir            : 1;

        // Reserved
        uint32_t rsvd4          : 15;
    };
} XhciStatusStageTrb_t;
static_assert(sizeof(XhciStatusStageTrb_t) == sizeof(uint32_t) * 4);

/*
// xHci Spec Section 6.4.4.2 Event Data TRB Figure 6-39: Event Data TRB (page 505).

An Event Data TRB allows system software to generate a software defined event
and specify the Parameter field of the generated Transfer Event.

Note: When applying Event Data TRBs to control transfer: 1) An Event Data TRB may
be inserted at the end of a Data Stage TD in order to report the accumulated
transfer length of a multi-TRB TD. 2) An Event Data TRB may be inserted at the
end of a Status Stage TD in order to provide Event Data associated with the
control transfer completion.

Refer to section 4.11.5.2 for more information
*/
typedef struct XhciEventDataTransferRequestBlock {
    /*
        Event Data Hi and Lo. This field represents the 64-bit value that shall be copied to
        the TRB Pointer field (Parameter Component) of the Transfer Event TRB.
    */
    uint64_t eventData;

    struct {
        // Reserved
        uint32_t rsvd0              : 22;

        /*
            This field defines the index of the Interrupter that will receive events
            generated by this TRB. Valid values are between 0 and MaxIntrs-1.
        */
        uint32_t interrupterTarget  : 10;
    };
    
    struct {
        // This bit is used to mark the Enqueue point of a Transfer ring
        uint32_t cycleBit       : 1;

        /*
            Evaluate Next TRB (ENT). If this flag is ‘1’ the xHC shall fetch and evaluate the
            next TRB before saving the endpoint state. Refer to section 4.12.3 for more information.
        */
        uint32_t ent            : 1;

        // Reserved
        uint32_t rsvd1          : 2;

        /*
            Chain bit (CH). Set to ‘1’ by software to associate this TRB with the next TRB on the Ring.
            A Data Stage TD is defined as a Data Stage TRB followed by zero or more Normal TRBs.
            The Chain bit is used to identify a multi-TRB Data Stage TD.
            
            The Chain bit is always ‘0’ in the last TRB of a Data Stage TD.
        */
        uint32_t chain          : 1;

        /*
            Interrupt On Completion (IOC). If this bit is set to ‘1’, it specifies that when this TRB
            completes, the Host Controller shall notify the system of the completion by placing an
            Event TRB on the Event ring and sending an interrupt at the next interrupt threshold.
            Refer to section 4.10.4.
        */
        uint32_t ioc            : 1;

        // Reserved
        uint32_t rsvd2          : 3;

        /*
            Block Event Interrupt (BEI). If this bit is set to '1' and IOC = '1', then the
            Transfer Event generated by IOC shall not assert an interrupt to the host at the
            next interrupt threshold. Refer to section 4.17.5.
        */
        uint32_t bei            : 1;

        /*
            TRB Type. This field is set to Setup Stage TRB type.
            Refer to Table 6-91 for the definition of the Type TRB IDs.
        */
        uint32_t trbType        : 6;

        // Reserved
        uint32_t rsvd3          : 16;
    };
} XhciEventDataTrb_t;
static_assert(sizeof(XhciEventDataTrb_t) == sizeof(uint32_t) * 4);

/*
// xHci Spec Section 6.5 Event Ring Segment Table Figure 6-40: Event Ring Segment Table Entry

Note: The Ring Segment Size may be set to any value from 16 to 4096, however
software shall allocate a buffer for the Event Ring Segment that rounds up its
size to the nearest 64B boundary to allow full cache-line accesses.
*/
struct XhciErstEntry {
    uint64_t ringSegmentBaseAddress;  // Base address of the Event Ring segment
    uint32_t ringSegmentSize;         // Size of the Event Ring segment (only low 16 bits are used)
    uint32_t rsvd;
} __attribute__((packed));

/*
// xHci Spec Section 5.5.2.1 (page 390)

Address: Runtime Base + 020h + (32 * Interrupter)
        where: Interrupter is 0, 1, 2, 3, … 1023
Default Value: 0000 0000h
Attribute: RW
Size: 32 bits

The Interrupter Management register allows system software to enable,
disable, and detect xHC interrupts.
*/
struct XhciInterrupterManagementRegister {
    union {
        uint32_t raw;
        struct {
            uint32_t interruptPending   : 1;  // Interrupt Pending (IP), bit 0
            uint32_t interruptEnabled   : 1;  // Interrupt Enable (IE), bit 1
            uint32_t reserved           : 30; // Reserved bits, bits 2-31
        } bits __attribute__((packed));
    };
} __attribute__((packed));
static_assert(sizeof(XhciInterrupterManagementRegister) == 4);

/*
// xHci Spec Section 5.5.2 (page 389)

Note: All registers of the Primary Interrupter shall be initialized before
setting the Run/Stop (RS) flag in the USBCMD register to ‘1’. Secondary
Interrupters may be initialized after RS = ‘1’, however all Secondary
Interrupter registers shall be initialized before an event that targets them is
generated. Not following these rules, shall result in undefined xHC behavior.
*/
struct XhciInterrupterRegisters {
    uint32_t iman;         // Interrupter Management (offset 00h)
    uint32_t imod;         // Interrupter Moderation (offset 04h)
    uint32_t erstsz;       // Event Ring Segment Table Size (offset 08h)
    uint32_t rsvdP;        // Reserved (offset 0Ch)
    uint64_t erstba;       // Event Ring Segment Table Base Address (offset 10h)
    union {
        struct {
            // This index is used to accelerate the checking of
            // an Event Ring Full condition. This field can be 0.
            uint64_t dequeueErstSegmentIndex : 3;

            // This bit is set by the controller when it sets the
            // Interrupt Pending bit. Then once your handler is finished
            // handling the event ring, you clear it by writing a '1' to it.
            uint64_t eventHandlerBusy        : 1;

            // Physical address of the _next_ item in the event ring
            uint64_t eventRingDequeuePointer : 60;
        };
        uint64_t erdp;     // Event Ring Dequeue Pointer (offset 18h)
    };
};

/*
// xHci Spec Section 5.5 Table 5-35: Host Controller Runtime Registers (page 388)

This section defines the xHCI Runtime Register space. The base address of this
register space is referred to as Runtime Base. The Runtime Base shall be 32-
byte aligned and is calculated by adding the value Runtime Register Space
Offset register (refer to Section 5.3.8) to the Capability Base address. All
Runtime registers are multiples of 32 bits in length.
Unless otherwise stated, all registers should be accessed with Dword references
on reads, with an appropriate software mask if needed. A software
read/modify/write mechanism should be invoked for partial writes.
Software should write registers containing a Qword address field using only
Qword references. If a system is incapable of issuing Qword references, then 
388 Document Number: 625472, Revision: 1.2b Intel Confidential
writes to the Qword address fields shall be performed using 2 Dword
references; low Dword-first, high-Dword second.
*/
struct XhciRuntimeRegisters {
    uint32_t mfIndex;                          // Microframe Index (offset 0000h)
    uint32_t rsvdZ[7];                         // Reserved (offset 001Fh:0004h)
    XhciInterrupterRegisters ir[1024];         // Interrupter Register Sets (offset 0020h to 8000h)
};

/*
// xHci Spec Section 6.2.2 Figure 6-2: Slot Context Data Structure (page 407)

Slot State. This field is updated by the xHC when a Device Slot transitions from one state to
another.

Value Slot State
    0 Disabled/Enabled
    1 Default
    2 Addressed
    3 Configured
    4 Reserved

Slot States are defined in section 4.5.3.
As Output, since software initializes all fields of the Device Context data structure to ‘0’, this field
shall initially indicate the Disabled state.
As Input, software shall initialize the field to ‘0’.
Refer to section 4.5.3 for more information on Slot State.
*/
#define XHCI_SLOT_STATE_DISABLED_ENABLED    0
#define XHCI_SLOT_STATE_DEFAULT             1
#define XHCI_SLOT_STATE_ADDRESSED           2
#define XHCI_SLOT_STATE_CONFIGURED          3
#define XHCI_SLOT_STATE_RESERVED            4

/*
// xHci Spec Section 6.2.2 Figure 6-2: Slot Context Data Structure (page 407)

The Slot Context data structure defines information that applies to a device as
a whole.

Note: Figure 6-2 illustrates a 32 byte Slot Context. That is, the Context Size (CSZ)
field in the HCCPARAMS1 register = ‘0’. If the Context Size (CSZ) field = ‘1’
then each Slot Context data structure consumes 64 bytes, where bytes 32 to
63 are also xHCI Reserved (RsvdO).
*/
struct XhciSlotContext32 {
    union {
        struct {
            /*
                Route String. This field is used by hubs to route packets to the correct downstream port. The
                format of the Route String is defined in section 8.9 the USB3 specification.
                As Input, this field shall be set for all USB devices, irrespective of their speed, to indicate their
                location in the USB topology.
            */
            uint32_t routeString    : 20;

            /*
                Speed. This field is deprecated in this version of the specification and shall be Reserved.
                This field indicates the speed of the device. Refer to the PORTSC Port Speed field in Table 5-27
                for the definition of the valid values.
            */
            uint32_t speed          : 4;

            // Reserved
            uint32_t rz             : 1;

            /*
                Multi-TT (MTT)107. This flag is set to '1' by software if this is a High-speed hub that supports
                Multiple TTs and the Multiple TT Interface has been enabled by software, or if this is a Low-
                /Full-speed device or Full-speed hub and connected to the xHC through a parent108 High-speed
                hub that supports Multiple TTs and the Multiple TT Interface of the parent hub has been
                enabled by software, or ‘0’ if not.
            */
            uint32_t mtt            : 1;

            // Hub. This flag is set to '1' by software if this device is a USB hub, or '0' if it is a USB function.
            uint32_t hub            : 1;

            /*
                Context Entries. This field identifies the index of the last valid Endpoint Context within this
                Device Context structure. The value of ‘0’ is Reserved and is not a valid entry for this field. Valid
                entries for this field shall be in the range of 1-31. This field indicates the size of the Device
                Context structure. For example, ((Context Entries+1) * 32 bytes) = Total bytes for this structure.
                Note, Output Context Entries values are written by the xHC, and Input Context Entries values are
                written by software.
            */
            uint32_t contextEntries : 5;
        };
        uint32_t dword0;
    };

    union {
        struct {
            /*
                Max Exit Latency. The Maximum Exit Latency is in microseconds, and indicates the worst case
                time it takes to wake up all the links in the path to the device, given the current USB link level
                power management settings.
                Refer to section 4.23.5.2 for more information on the use of this field.
            */
            uint16_t    maxExitLatency;
            
            /*
                Root Hub Port Number. This field identifies the Root Hub Port Number used to access the USB
                device. Refer to section 4.19.7 for port numbering information.
                Note: Ports are numbered from 1 to MaxPorts.
            */
            uint8_t     rootHubPortNum;

            /*
                Number of Ports. If this device is a hub (Hub = ‘1’), then this field is set by software to identify
                the number of downstream facing ports supported by the hub. Refer to the bNbrPorts field
                description in the Hub Descriptor (Table 11-13) of the USB2 spec. If this device is not a hub (Hub
                = ‘0’), then this field shall be ‘0’.
            */
            uint8_t     portCount;
        };
        uint32_t dword1;
    };

    union {
        struct {
            /*
                Parent Hub Slot ID. If this device is Low-/Full-speed and connected through a High-speed hub,
                then this field shall contain the Slot ID of the parent High-speed hub109.
                For SS and SSP bus instance, if this device is connected through a higher rank hub110 then this
                field shall contain the Slot ID of the parent hub. For example, a Gen1 x1 connected behind a
                Gen1 x2 hub, or Gen1 x2 device connected behind Gen2 x2 hub.
                This field shall be ‘0’ if any of the following are true:
                    Device is attached to a Root Hub port
                    Device is a High-Speed device
                    Device is the highest rank SS/SSP device supported by xHCI
            */
            uint32_t parentHubSlotId    : 8;
            
            /*
                Parent Port Number. If this device is Low-/Full-speed and connected through a High-speed
                hub, then this field shall contain the number of the downstream facing port of the parent High-
                speed hub109.
                For SS and SSP bus instance, if this device is connected through a higher rank hub110 then this
                field shall contain the number of the downstream facing port of the parent hub. For example, a
                Gen1 x1 connected behind a Gen1 x2 hub, or Gen1 x2 device connected behind Gen2 x2 hub.
                This field shall be ‘0’ if any of the following are true:
                    Device is attached to a Root Hub port
                    Device is a High-Speed device
                    Device is the highest rank SS/SSP device supported by xHCI
            */
            uint32_t parentPortNumber   : 8;

            /*
                TT Think Time (TTT). If this is a High-speed hub (Hub = ‘1’ and Speed = High-Speed), then this
                field shall be set by software to identify the time the TT of the hub requires to proceed to the
                next full-/low-speed transaction.
                Value Think Time
                0 TT requires at most 8 FS bit times of inter-transaction gap on a full-/low-speed
                downstream bus.
                    1 TT requires at most 16 FS bit times.
                    2 TT requires at most 24 FS bit times.
                    3 TT requires at most 32 FS bit times.

                Refer to the TT Think Time sub-field of the wHubCharacteristics field description in the Hub
                Descriptor (Table 11-13) and section 11.18.2 of the USB2 spec for more information on TT
                Think Time. If this device is not a High-speed hub (Hub = ‘0’ or Speed != High-speed), then this
                field shall be ‘0’.
            */
            uint32_t ttThinkTime        : 2;

            // Reserved
            uint32_t rsvd0              : 4;
            
            /*
                Interrupter Target. This field defines the index of the Interrupter that will receive Bandwidth
                Request Events and Device Notification Events generated by this slot, or when a Ring Underrun
                or Ring Overrun condition is reported (refer to section 4.10.3.1). Valid values are between 0 and
                MaxIntrs-1.
            */
            uint32_t interrupterTarget  : 10;
        };
        uint32_t dword2;
    };

    union {
        struct {
            /*
                USB Device Address. This field identifies the address assigned to the USB device by the xHC,
                and is set upon the successful completion of a Set Address Command. Refer to the USB2 spec
                for a more detailed description.
                As Output, this field is invalid if the Slot State = Disabled or Default.
                As Input, software shall initialize the field to ‘0’.
            */
            uint32_t deviceAddress  : 8;

            // Reserved
            uint32_t rsvd1          : 19;
            
            /*
                Slot State. This field is updated by the xHC when a Device Slot transitions from one state to
                another.
                
                Value Slot State
                    0 Disabled/Enabled
                    1 Default
                    2 Addressed
                    3 Configured
                    31-4 Reserved

                Slot States are defined in section 4.5.3.
                As Output, since software initializes all fields of the Device Context data structure to ‘0’, this field
                shall initially indicate the Disabled state.
                As Input, software shall initialize the field to ‘0’.
                Refer to section 4.5.3 for more information on Slot State.
            */
            uint32_t slotState      : 5;
        };
        uint32_t dword3;
    };

    /*
        The remaining bytes (10-1Fh) within the Slot Context are dedicated for exclusive
        use by the xHC and shall be treated by system software as Reserved and Opaque
        (RsvdO).
    */
    uint32_t rsvdZ[4];
} __attribute__((packed));
static_assert(sizeof(XhciSlotContext32) == 32);

// 64-byte context version
struct XhciSlotContext64 {
    // Default 32-byte context fields
    XhciSlotContext32 ctx32;

    // Padding to fill up to 64 bytes
    uint32_t rsvd[8];
};
static_assert(sizeof(XhciSlotContext64) == 64);

/*
// xHci Spec Section 6.2.3 Figure 6-3: Endpoint Context Data Structure (page 412)

The Endpoint Context data structure defines information that applies to a
specific endpoint.

Note: Unless otherwise stated: As Input, all fields of the Endpoint Context shall be
initialized to the appropriate value by software before issuing a command. As
Output, the xHC shall update each field to reflect the current value that
it is using.

Note: The remaining bytes (14-1Fh) within the Endpoint Context are dedicated for
exclusive use by the xHC and shall be treated by system software as
Reserved and Opaque (RsvdO).

Note: Figure 6-3 illustrates a 32 byte Endpoint Context. That is, the Context Size
(CSZ) field in the HCCPARAMS1 register = ‘0’. If the Context Size (CSZ) field
= ‘1’ then each Endpoint Context data structure consumes 64 bytes, where
bytes 32 to 63 are xHCI Reserved (RsvdO).
*/
struct XhciEndpointContext32 {
    union {
        struct {
            /*
                Endpoint State (EP State). The Endpoint State identifies the current operational state of the
                endpoint.
                
                Value Definition
                    0 Disabled The endpoint is not operational
                    1 Running The endpoint is operational, either waiting for a doorbell ring or processing
                    TDs.
                    2 HaltedThe endpoint is halted due to a Halt condition detected on the USB. SW shall issue
                    Reset Endpoint Command to recover from the Halt condition and transition to the Stopped
                    state. SW may manipulate the Transfer Ring while in this state.
                    3 Stopped The endpoint is not running due to a Stop Endpoint Command or recovering
                    from a Halt condition. SW may manipulate the Transfer Ring while in this state.
                    4 Error The endpoint is not running due to a TRB Error. SW may manipulate the Transfer
                    Ring while in this state.
                    5-7 Reserved

                As Output, a Running to Halted transition is forced by the xHC if a STALL condition is detected
                on the endpoint. A Running to Error transition is forced by the xHC if a TRB Error condition is
                detected.
                As Input, this field is initialized to ‘0’ by software.
                Refer to section 4.8.3 for more information on Endpoint State.
            */
            uint32_t endpointState      : 3;

            // Reserved
            uint32_t rsvd0              : 7;

            /*
                Mult. If LEC = ‘0’, then this field indicates the maximum number of bursts within an Interval that
                this endpoint supports. Mult is a “zero-based” value, where 0 to 3 represents 1 to 4 bursts,
                respectively. The valid range of values is ‘0’ to ‘2’. This field shall be ‘0’ for all endpoint types
                except for SS Isochronous.
                If LEC = ‘1’, then this field shall be RsvdZ and Mult is calculated as:
                ROUNDUP(Max ESIT Payload / Max Packet Size / (Max Burst Size + 1)) - 1
            */
            uint32_t mult               : 2;
            
            /*
                Max Primary Streams (MaxPStreams). This field identifies the maximum number of Primary
                Stream IDs this endpoint supports. Valid values are defined below. If the value of this field is ‘0’,
                then the TR Dequeue Pointer field shall point to a Transfer Ring. If this field is > '0' then the TR
                Dequeue Pointer field shall point to a Primary Stream Context Array. Refer to section 4.12 for
                more information.
                A value of ‘0’ indicates that Streams are not supported by this endpoint and the Endpoint
                Context TR Dequeue Pointer field references a Transfer Ring.
                A value of ‘1’ to ‘15’ indicates that the Primary Stream ID Width is MaxPstreams+1 and the
                Primary Stream Array contains 2MaxPStreams+1 entries.
                For SS Bulk endpoints, the range of valid values for this field is defined by the MaxPSASize field
                in the HCCPARAMS1 register (refer to Table 5-13).
                This field shall be '0' for all SS Control, Isoch, and Interrupt endpoints, and for all non-SS
                endpoints
            */
            uint32_t maxPrimaryStreams  : 5;
            
            /*
                Linear Stream Array (LSA). This field identifies how a Stream ID shall be interpreted.
                Setting this bit to a value of ‘1’ shall disable Secondary Stream Arrays and a Stream ID shall be
                interpreted as a linear index into the Primary Stream Array, where valid values for MaxPStreams
                are ‘1’ to ‘15’.
                A value of ‘0’ shall enable Secondary Stream Arrays, where the low order (MaxPStreams+1) bits
                of a Stream ID shall be interpreted as a linear index into the Primary Stream Array, where valid
                values for MaxPStreams are ‘1’ to ‘7’. And the high order bits of a Stream ID shall be interpreted
                as a linear index into the Secondary Stream Array.
                If MaxPStreams = ‘0’, this field RsvdZ.
                Refer to section 4.12.2 for more information.
            */
            uint32_t linearStreamArray  : 1;
            
            /*
                Interval. The period between consecutive requests to a USB endpoint to send or receive data.
                Expressed in 125 μs. increments. The period is calculated as 125 μs. * 2Interval; e.g., an Interval
                value of 0 means a period of 125 μs. (20 = 1 * 125 μs.), a value of 1 means a period of 250 μs. (21
                = 2 * 125 μs.), a value of 4 means a period of 2 ms. (24 = 16 * 125 μs.), etc. Refer to Table 6-12
                for legal Interval field values. See further discussion of this field below. Refer to section 6.2.3.6
                for more information.
            */
            uint32_t interval           : 8;
            
            /*
                Max Endpoint Service Time Interval Payload High (Max ESIT Payload Hi). If LEC = '1', then this
                field indicates the high order 8 bits of the Max ESIT Payload value. If LEC = '0', then this field
                shall be RsvdZ. Refer to section 6.2.3.8 for more information.
            */
            uint32_t maxEsitPayloadHi   : 8;
        };
        uint32_t dword0;
    };

    union {
        struct {
            // Reserved
            uint32_t rsvd1                  : 1;

            /*
                Error Count (CErr)112. This field defines a 2-bit down count, which identifies the number of
                consecutive USB Bus Errors allowed while executing a TD. If this field is programmed with a
                non-zero value when the Endpoint Context is initialized, the xHC loads this value into an internal
                Bus Error Counter before executing a USB transaction and decrements it if the transaction fails.
                If the Bus Error Counter counts from ‘1’ to ‘0’, the xHC ceases execution of the TRB, sets the
                endpoint to the Halted state, and generates a USB Transaction Error Event for the TRB that
                caused the internal Bus Error Counter to decrement to ‘0’. If system software programs this field
                to ‘0’, the xHC shall not count errors for TRBs on the Endpoint’s Transfer Ring and there shall be
                no limit on the number of TRB retries. Refer to section 4.10.2.7 for more information on the
                operation of the Bus Error Counter.
                Note: CErr does not apply to Isoch endpoints and shall be set to ‘0’ if EP Type = Isoch Out ('1') or
                Isoch In ('5').
            */
            uint32_t errorCount             : 2;

            /*
                Endpoint Type (EP Type). This field identifies whether an Endpoint Context is Valid, and if so,
                what type of endpoint the context defines.
                
                Value  Endpoint      Type Direction
                0      Not Valid     N/A
                1      Isoch         Out
                2      Bulk          Out
                3      Interrupt     Out
                4      Control       Bidirectional
                5      Isoch         In
                6      Bulk          In
                7      Interrupt     In
            */
            uint32_t endpointType           : 3;
            
            // Reserved
            uint32_t rsvd2                  : 1;
            
            /*
                Host Initiate Disable (HID). This field affects Stream enabled endpoints, allowing the Host
                Initiated Stream selection feature to be disabled for the endpoint. Setting this bit to a value of
                ‘1’ shall disable the Host Initiated Stream selection feature. A value of ‘0’ will enable normal
                Stream operation. Refer to section 4.12.1.1 for more information.
            */
            uint32_t hostInitiateDisable    : 1;
            
            /*
                Max Burst Size. This field indicates to the xHC the maximum number of consecutive USB
                transactions that should be executed per scheduling opportunity. This is a “zero-based” value,
                where 0 to 15 represents burst sizes of 1 to 16, respectively. Refer to section 6.2.3.4 for more
                information.
            */
            uint32_t maxBurstSize           : 8;
            
            /*
                Max Packet Size. This field indicates the maximum packet size in bytes that this endpoint is
                capable of sending or receiving when configured. Refer to section 6.2.3.5 for more information
            */
            uint32_t maxPacketSize          : 16;
        };
        uint32_t dword1;
    };

    union {
        struct {
            /*
                Dequeue Cycle State (DCS). This bit identifies the value of the xHC Consumer Cycle State (CCS)
                flag for the TRB referenced by the TR Dequeue Pointer. Refer to section 4.9.2 for more
                information. This field shall be ‘0’ if MaxPStreams > ‘0’.
            */
            uint64_t dcs                        : 1;

            // Reserved
            uint64_t rsvd3                      : 3;

            /*
                TR Dequeue Pointer. As Input, this field represents the high order bits of the 64-bit base
                address of a Transfer Ring or a Stream Context Array associated with this endpoint. If
                MaxPStreams = '0' then this field shall point to a Transfer Ring. If MaxPStreams > '0' then this
                field shall point to a Stream Context Array.
                As Output, if MaxPStreams = ‘0’ this field shall be used by the xHC to store the value of the
                Dequeue Pointer when the endpoint enters the Halted or Stopped states, and the value of the
                this field shall be undefined when the endpoint is not in the Halted or Stopped states. if
                MaxPStreams > ‘0’ then this field shall point to a Stream Context Array.
                The memory structure referenced by this physical memory pointer shall be aligned to a 16-byte
                boundary.
            */
            uint64_t trDequeuePtrAddressBits    : 60;
        };
        struct {
            uint32_t dword2;
            uint32_t dword3;
        };
        uint64_t transferRingDequeuePtr;        
    };

    union {
        struct {
            /*
                Average TRB Length. This field represents the average Length of the TRBs executed by this
                endpoint. The value of this field shall be greater than ‘0’. Refer to section 4.14.1.1 and the
                implementation note TRB Lengths and System Bus Bandwidth for more information.
                The xHC shall use this parameter to calculate system bus bandwidth requirements.
            */
            uint16_t averageTrbLength;

            /*
                Max Endpoint Service Time Interval Payload Low (Max ESIT Payload Lo). This field indicates
                the low order 16 bits of the Max ESIT Payload. The Max ESIT Payload represents the total
                number of bytes this endpoint will transfer during an ESIT. This field is only valid for periodic
                endpoints. Refer to section 6.2.3.8 for more information.
            */
            uint16_t maxEsitPayloadLo;
        };
        uint32_t dword4;
    };
};
static_assert(sizeof(XhciEndpointContext32) == 32);

// 64-byte context version
struct XhciEndpointContext64 {
    // Default 32-byte context fields
    XhciEndpointContext32 ctx32;

    // Padding to fill up to 64 bytes
    uint32_t rsvd[8];
};
static_assert(sizeof(XhciEndpointContext64) == 64);

#define XHCI_ENDPOINT_STATE_DISABLED    0
#define XHCI_ENDPOINT_STATE_RUNNING     1
#define XHCI_ENDPOINT_STATE_HALTED      2
#define XHCI_ENDPOINT_STATE_STOPPED     3
#define XHCI_ENDPOINT_STATE_ERROR       4

#define XHCI_ENDPOINT_TYPE_INVALID          0
#define XHCI_ENDPOINT_TYPE_ISOCHRONOUS_OUT  1
#define XHCI_ENDPOINT_TYPE_BULK_OUT         2
#define XHCI_ENDPOINT_TYPE_INTERRUPT_OUT    3
#define XHCI_ENDPOINT_TYPE_CONTROL          4
#define XHCI_ENDPOINT_TYPE_ISOCHRONOUS_IN   5
#define XHCI_ENDPOINT_TYPE_BULK_IN          6
#define XHCI_ENDPOINT_TYPE_INTERRUPT_IN     7

/*
// xHci Spec Section 6.2.1 Device Context (page 406)

The Device Context data structure is used in the xHCI architecture as Output
by the xHC to report device configuration and state information to system
software. The Device Context data structure is pointed to by an entry in the
Device Context Base Address Array (refer to section 6.1).
The Device Context Index (DCI) is used to reference the respective element of
the Device Context data structure.
All unused entries of the Device Context shall be initialized to ‘0’ by software.

    Note: Figure 6-1 illustrates offsets with 32-byte Device Context data
    structures. That is, the Context Size (CSZ) field in the HCCPARAMS1 register
    = '0'. If the Context Size (CSZ) field = '1' then the Device Context data
    structures consume 64 bytes each. The offsets shall be 040h for the EP
    Context 0, 080h for EP Context 1, and so on.

    Note: Ownership of the Output Device Context data structure is passed to
    the xHC when software rings the Command Ring doorbell for the first Address
    Device Command issued to a Device Slot after an Enable Slot Command, that
    is, the first transition of the Slot from the Enabled to the Default or Addressed
    state. Software shall initialize the Output Device Context to 0 prior to the
    execution of the first Address Device Command.
*/
struct XhciDeviceContext32 {
    // Slot context
    XhciSlotContext32 slotContext;

    // Primary control endpoint
    XhciEndpointContext32 controlEndpointContext;

    // Optional communication endpoints
    XhciEndpointContext32 ep[30];
};
static_assert(sizeof(XhciDeviceContext32) == 1024);

struct XhciDeviceContext64 {
    // Slot context
    XhciSlotContext64 slotContext;

    // Primary control endpoint
    XhciEndpointContext64 controlEndpointContext;

    // Optional communication endpoints
    XhciEndpointContext64 ep[30];
};
static_assert(sizeof(XhciDeviceContext64) == 2048);

/*
// xHci Sped Section 6.2.5.1 Figure 6-6: Input Control Context (page 461)

The Input Control Context data structure defines which Device Context data
structures are affected by a command and the operations to be performed on
those contexts
*/
struct XhciInputControlContext32 {
    /*
        Drop Context flags (D2 - D31). These single bit fields identify which Device Context data
        structures should be disabled by command. If set to ‘1’, the respective Endpoint Context shall
        be disabled. If cleared to ‘0’, the Endpoint Context is ignored.
    */
    uint32_t dropFlags;

    /*
        Add Context flags (A0 - A31). These single bit fields identify which Device Context data
        structures shall be evaluated and/or enabled by a command. If set to ‘1’, the respective Context
        shall be evaluated. If cleared to ‘0’, the Context is ignored.
    */
    uint32_t addFlags;

    uint32_t rsvd[5];

    /*
        Configuration Value. If CIC = ‘1’, CIE = ‘1’, and this Input Context is associated with a Configure
        Endpoint Command, then this field contains the value of the Standard Configuration Descriptor
        bConfigurationValue field associated with the command, otherwise the this field shall be
        cleared to ‘0’.
    */
    uint8_t configValue;

    /*
        Interface Number. If CIC = ‘1’, CIE = ‘1’, this Input Context is associated with a Configure
        Endpoint Command, and the command was issued due to a SET_INTERFACE request, then this
        field contains the value of the Standard Interface Descriptor bInterfaceNumber field associated
        with the command, otherwise the this field shall be cleared to ‘0’.
    */
    uint8_t interfaceNumber;

    /*
        Alternate Setting. If CIC = ‘1’, CIE = ‘1’, this Input Context is associated with a Configure
        Endpoint Command, and the command was issued due to a SET_INTERFACE request, then this
        field contains the value of the Standard Interface Descriptor bAlternateSetting field associated
        with the command, otherwise the this field shall be cleared to ‘0’
    */
    uint8_t alternateSetting;

    // Reserved and zero'd
    uint8_t rsvdZ;
} __attribute__((packed));
static_assert(sizeof(XhciInputControlContext32) == 32);

// 64-byte context version
struct XhciInputControlContext64 {
    // Default 32-byte context fields
    XhciInputControlContext32 ctx32;

    // Padding to fill up to 64 bytes
    uint32_t rsvd[8];
};
static_assert(sizeof(XhciInputControlContext64) == 64);

/*
// xHci Sped Section 6.2.5 Input Context (page 459)

The Input Context data structure specifies the endpoints and the operations to
be performed on those endpoints by the Address Device, Configure Endpoint,
and Evaluate Context Commands. Refer to section 4.6 for more information on
these commands.
The Input Context is pointed to by an Input Context Pointer field of an Address
Device, Configure Endpoint, and Evaluate Context Command TRBs. The Input
Context is an array of up to 33 context data structure entries.
*/
struct XhciInputContext32 {
    XhciInputControlContext32   controlContext;
    XhciDeviceContext32         deviceContext;
};
static_assert(sizeof(XhciInputContext32) == sizeof(XhciInputControlContext32) + sizeof(XhciDeviceContext32));

// 64-byte context version
struct XhciInputContext64 {
    XhciInputControlContext64   controlContext;
    XhciDeviceContext64         deviceContext;
};
static_assert(sizeof(XhciInputContext64) == sizeof(XhciInputControlContext64) + sizeof(XhciDeviceContext64));

/*
// xHci Spec Section 5.4.8 Figure 5-20: Port Status and Control Register (PORTSC) (page 369-370)

Address: Operational Base + (400h + (10h * (n–1)))
where: n = Port Number (Valid values are 1, 2, 3, … MaxPorts)
Default: Field dependent
Attribute: RO, RW, RW1C (field dependent)
Size 32 bits

A host controller shall implement one or more port registers. The number of
port registers implemented by a particular instantiation of a host controller is
documented in the HCSPARAMS1 register (Section 5.3.3). Software uses this 
370 Document Number: 625472, Revision: 1.2b
Intel Confidential
information as an input parameter to determine how many ports need to be
serviced. All ports have the structure defined below.
This register is in the Aux Power well. It is only reset by platform hardware
during a cold reset or in response to a Host Controller Reset (HCRST). The
initial conditions of a port are described in Section 4.19.

Note: Port Status Change Events cannot be generated if the xHC is stopped
(HCHalted (HCH) = ‘1’). Refer to section 4.19.2 for more information about
change flags.

Note: Software shall ensure that the xHC is running (HCHalted (HCH) = ‘0’) before
attempting to write to this register.
Software cannot change the state of the port unless Port Power (PP) is asserted
(‘1’), regardless of the Port Power Control (PPC) capability (section 5.3.6). The
host is required to have power stable to the port within 20 milliseconds of the
‘0’ to ‘1’ transition of PP. If PPC = ‘1’ software is responsible for waiting 20 ms.
after asserting PP, before attempting to change the state of the port.

Note: If a port has been assigned to the Debug Capability, then the port shall not
report device connected (that is, CCS = ‘0’) and enabled when the Port Power
Flag is ‘1’. Refer to Section 7.6 for more information on the xHCI Debug
Capability operation.
*/
struct XhciPortscRegister {
    union {
        struct {
            // Current connect status (RO), if PP is 0, this bit is also 0
            uint32_t    ccs         : 1;

            // Port Enable/Disable (R/WC), if PP is 0, this bit is also 0
            uint32_t    ped         : 1;

            // Reserved and zeroed
            uint32_t    rsvd0       : 1;
            
            // Over-current active (RO)
            uint32_t    oca         : 1;

            // Port reset (R/W), if PP is 0, this bit is also 0
            uint32_t    pr          : 1;

            // Port link state (R/W), if PP is 0, this bit is also 0
            uint32_t    pls         : 4;

            // Port power (R/W)
            uint32_t    pp          : 1;

            // Port speed (RO)
            uint32_t    portSpeed   : 4;

            // Port indicator control (R/W), if PP is 0, this bit is also 0
            uint32_t    pic         : 2;

            // Port link state write strobe (R/W), if PP is 0, this bit is also 0
            uint32_t    lws         : 1;

            // Connect status change (R/WC), if PP is 0, this bit is also 0.
            // ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            // ** Clear this bit by writing a '1' to it.
            uint32_t    csc         : 1;

            /*
            Port enable/disable change (R/WC), if PP is 0, this bit is also 0.
            ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            ** For a USB2 protocol port, this bit shall be set to ‘1’ only when the port is disabled (EOF2)
            ** For a USB3 protocol port, this bit shall never be set to ‘1’
            ** Software shall clear this bit by writing a ‘1’ to it. Refer to section 4.19.2
            */
            uint32_t    pec         : 1;

            // Warm port reset change (R/WC), if PP is 0, this bit is also 0.
            // ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            // ** Reserved and zeroed on USB2 ports.
            // ** Software shall clear this bit by writing a '1' to it.
            uint32_t    wrc         : 1;

            // Over-current change (R/WC), if PP is 0, this bit is also 0.
            // ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            // ** Software shall clear this bit by writing a '1' to it.
            uint32_t    occ         : 1;

            // Port reset change (R/WC), if PP is 0, this bit is also 0.
            // ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            // ** Software shall clear this bit by writing a '1' to it.
            uint32_t    prc         : 1;

            // Port link state change (R/WC), if PP is 0, this bit is also 0.
            // ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            uint32_t    plc         : 1;

            // Port config error change (R/WC), if PP is 0, this bit is also 0.
            // ** When transitioning from 0 to a 1, will trigger a Port Status Change Event.
            // ** Reserved and zeroed on USB2 ports.
            // ** Software shall clear this bit by writing a '1' to it.
            uint32_t    cec         : 1;

            // Cold attach status (R/O), if PP is 0, this bit is also 0.
            uint32_t    cas         : 1;

            // Wake on connect enable (R/W)
            uint32_t    wce         : 1;

            // Wake on disconnect enable (R/W)
            uint32_t    wde         : 1;

            // Wake on over-current enable (R/W)
            uint32_t    woe         : 1;

            // Reserved and zeroed
            uint32_t    rsvd1        : 2;

            // Device removable (RO)
            uint32_t    dr          : 1;

            // Warm port reset (R/WC).
            // ** Reserved and zeroed on USB2 ports.
            uint32_t    wpr         : 1;
        } __attribute__((packed));

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));
static_assert(sizeof(XhciPortscRegister) == sizeof(uint32_t));

/*
// xHci Spec Section 7.2.1 Protocol Speed ID (PSI) (page 524)

Protocol Speed ID (PSI) Dwords immediately follow the Dword at offset 10h in
an xHCI Supported Protocol Capability data structure. Table 7-10 defines the
fields of a PSI Dword.
*/
#define XHCI_USB_SPEED_UNDEFINED            0
#define XHCI_USB_SPEED_FULL_SPEED           1 // 12 MB/s USB 2.0
#define XHCI_USB_SPEED_LOW_SPEED            2 // 1.5 Mb/s USB 2.0
#define XHCI_USB_SPEED_HIGH_SPEED           3 // 480 Mb/s USB 2.0
#define XHCI_USB_SPEED_SUPER_SPEED          4 // 5 Gb/s (Gen1 x1) USB 3.0
#define XHCI_USB_SPEED_SUPER_SPEED_PLUS     5 // 10 Gb/s (Gen2 x1) USB 3.1

// TO-DO add spec page
struct XhciPortpmscRegisterUsb2 {
    union {
        struct {
            uint32_t l1Status                       : 3;
            uint32_t remoteWakeEnable               : 1;
            uint32_t hostInitiatedResumeDuration    : 4;
            uint32_t l1DeviceSlot                   : 8;
            uint32_t hardwareLpmEnable              : 1;
            uint32_t rsvd                           : 11;
            uint32_t portTestControl                : 4;
        } __attribute__((packed));

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));
static_assert(sizeof(XhciPortpmscRegisterUsb2) == sizeof(uint32_t));

struct XhciPortpmscRegisterUsb3 {
    union {
        struct {
            uint32_t u1Timeout          : 8;
            uint32_t u2Timeout          : 8;
            uint32_t forceLinkPmAccept  : 1;
            uint32_t rsvd               : 15;
        } __attribute__((packed));

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));
static_assert(sizeof(XhciPortpmscRegisterUsb3) == sizeof(uint32_t));

// For USB2.0 this register is reserved and preserved
struct XhciPortliRegister {
    union {
        struct {
            uint32_t linkErrorCount : 16;
            uint32_t rxLaneCount    : 4;
            uint32_t txLaneCount    : 4;
            uint32_t rsvd           : 8;
        } __attribute__((packed));

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));
static_assert(sizeof(XhciPortliRegister) == sizeof(uint32_t));

// Port Hardware LPM Control Register
struct XhciPorthlpmcRegisterUsb2 {
    union {
        struct {
            uint32_t hirdm      : 2;
            uint32_t l1Timeout  : 8;
            uint32_t besld      : 4;
            uint32_t rsvd       : 18;
        } __attribute__((packed));

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));
static_assert(sizeof(XhciPorthlpmcRegisterUsb2) == sizeof(uint32_t));

struct XhciPorthlpmcRegisterUsb3 {
    union {
        struct {
            uint16_t linkSoftErrorCount;
            uint16_t rsvd;
        } __attribute__((packed));

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));
static_assert(sizeof(XhciPorthlpmcRegisterUsb3) == sizeof(uint32_t));

/*
// xHci Spec Section 5.6 Figure 5-29: Doorbell Register (page 394)

The Doorbell Array is organized as an array of up to 256 Doorbell Registers.
One 32-bit Doorbell Register is defined in the array for each Device Slot.
System software utilizes the Doorbell Register to notify the xHC that it has
Device Slot related work for the xHC to perform.
The number of Doorbell Registers implemented by a particular instantiation of a
host controller is documented in the Number of Device Slots (MaxSlots) field of
the HCSPARAMS1 register (section 5.3.3).
These registers are pointed to by the Doorbell Offset Register (DBOFF) in the
xHC Capability register space. The Doorbell Array base address shall be Dword
aligned and is calculated by adding the value in the DBOFF register (section
5.3.7) to “Base” (the base address of the xHCI Capability register address
space).

All registers are 32 bits in length. Software should read and write these
registers using only Dword accesses

Note: Software shall not write the Doorbell of an endpoint until after it has issued a
Configure Endpoint Command for the endpoint and received a successful
Command Completion Event.
*/
struct XhciDoorbellRegister {
    union {
        struct {
            uint8_t     dbTarget;
            uint8_t     rsvd;
            uint16_t    dbStreamId;
        };

        // Must be accessed using 32-bit dwords
        uint32_t raw;
    };
} __attribute__((packed));

// (TO-DO: Find spec page)
#define XHCI_DOORBELL_TARGET_COMMAND_RING       0
#define XHCI_DOORBELL_TARGET_CONTROL_EP_RING    1

/*
// xHci Spec Section 7.2 (page 521)
At least one of these capability structures is required for all xHCI
implementations. More than one may be defined for implementations that
support more than one bus protocol. Refer to section 4.19.7 for more
information.
*/
struct XhciUsbSupportedProtocolCapability {
    union {
        struct {
            uint8_t id;
            uint8_t next;
            uint8_t minorRevisionVersion;
            uint8_t majorRevisionVersion;
        };

        // Extended capability entries must be read as 32-bit words
        uint32_t dword0;
    };

    union {
        uint32_t dword1;
        uint32_t name; // "USB "
    };

    union {
        struct {
            uint8_t compatiblePortOffset;
            uint8_t compatiblePortCount;
            uint8_t protocolDefined;
            uint8_t protocolSpeedIdCount; // (PSIC)
        };

        uint32_t dword2;
    };

    union {
        struct {
            uint32_t slotType : 4;
            uint32_t reserved : 28;
        };

        uint32_t dword3;
    };

    XhciUsbSupportedProtocolCapability() = default;
    XhciUsbSupportedProtocolCapability(volatile uint32_t* cap) {
        dword0 = cap[0];
        dword1 = cap[1];
        dword2 = cap[2];
        dword3 = cap[3];
    }
};

static_assert(sizeof(XhciUsbSupportedProtocolCapability) == (4 * sizeof(uint32_t)));

/*
// xHci Spec Section 4.2 Host Controller Initialization (page 68)

When the system boots, the host controller is enumerated, assigned a base
address for the xHC register space, and the system software sets the Frame
Length Adjustment (FLADJ) register to a system-specific value.
Refer to Section 4.23.1 Power Wells for a discussion of the effect of Power
Wells on register state after power-on and light resets.
Document Number: 625472, Revision: 1.2b 69
Following are a review of the operations that system software would perform in
order to initialize the xHC using MSI-X as the interrupt mechanism:
    • Initialize the system I/O memory maps, if supported.
    
    • After Chip Hardware Reset6 wait until the Controller Not Ready (CNR) flag
in the USBSTS is ‘0’ before writing any xHC Operational or Runtime
registers.

Note: This text does not imply a specific order for the following operations, however
these operations shall be completed before setting the USBCMD register
Run/Stop (R/S) bit to ‘1’.


    • Program the Max Device Slots Enabled (MaxSlotsEn) field in the CONFIG
register (5.4.7) to enable the device slots that system software is going to
use.

    • Program the Device Context Base Address Array Pointer (DCBAAP) register
(Section 5.4.6 Device Context Base Address Array Pointer Register
(DCBAAP)) with a 64-bit address pointing to where the Device Context
Base Address Array is located.

    • Define the Command Ring Dequeue Pointer by programming the Command
Ring Control Register (Section 5.4.5 Command Ring Control Register
(CRCR)) with a 64-bit address pointing to the starting address of the first
TRB of the Command Ring.

    • Initialize interrupts by:
        o Allocate and initialize the MSI-X Message Table (Section 5.2.8.3 MSI-X
        Table), setting the Message Address and Message Data, and enable the
        vectors. At a minimum, table vector entry 0 shall be initialized and
        enabled. Refer to the PCI specification for more details.
        o Allocate and initialize the MSI-X Pending Bit Array (PBA, Section 5.2.8.4
        MSI-X PBA).
        o Point the Table Offset and PBA Offsets in the MSI-X Capability Structure
        to the MSI-X Message Control Table and Pending Bit Array,
        respectively.
        o Initialize the Message Control register (Section 5.2.8.3 MSI-X Table) of
        the MSI-X Capability Structure.
        o Initialize each active interrupter by:
            ▪ Defining the Event Ring: (refer to Section 4.9.4 Event Ring
            Management for a discussion of Event Ring Management.)

    • Allocate and initialize the Event Ring Segment(s).
Refer to the PCI spec for the initialization and use of MSI or PIN interrupt mechanisms
A Chip Hardware Reset may be either a PCI reset input or an optional power-on reset input to the xHC.

Interrupts are optional. The xHC may be managed by polling Event Rings.
Document Number: 625472, Revision: 1.2b Intel Confidential

    • Allocate the Event Ring Segment Table (ERST) (Section 6.5
Event Ring Segment Table). Initialize ERST table entries to
point to and to define the size (in TRBs) of the respective Event
Ring Segment.

    • Program the Interrupter Event Ring Segment Table Size
(ERSTSZ) register (Section 5.5.2.3.1 Event Ring Segment Table
Size Register (ERSTSZ)) with the number of segments
described by the Event Ring Segment Table.

    • Program the Interrupter Event Ring Dequeue Pointer (ERDP)
register (Section 5.5.2.3.3 Event Ring Dequeue Pointer Register
(ERDP)) with the starting address of the first segment
described by the Event Ring Segment Table.

    • Program the Interrupter Event Ring Segment Table Base
Address (ERSTBA) register (Section 5.5.2.3.2 Event Ring
Segment Table Base Address Register (ERSTBA)) with a 64-bit
address pointer to where the Event Ring Segment Table is
located.

Note that writing the ERSTBA enables the Event Ring. Refer to
Section 4.9.4 Event Ring Management for more information on
the Event Ring registers and their initialization.

    ▪ Defining the interrupts:
        • Enable the MSI-X interrupt mechanism by setting the MSI-X
        Enable flag in the MSI-X Capability Structure Message Control
        register (5.2.8.3).
        • Initializing the Interval field of the Interrupt Moderation register
        (5.5.2.2) with the target interrupt moderation rate.
        • Enable system bus interrupt generation by writing a ‘1’ to the
        Interrupter Enable (INTE) flag of the USBCMD register (5.4.1).
        • Enable the Interrupter by writing a ‘1’ to the Interrupt Enable
        (IE) field of the Interrupter Management register (5.5.2.1).
        • Write the USBCMD (5.4.1) to turn the host controller ON via setting the
        Run/Stop (R/S) bit to ‘1’. This operation allows the xHC to begin accepting
        doorbell references.
*/

class XhciExtendedCapability {
public:
    XhciExtendedCapability(volatile uint32_t* capPtr);

    inline volatile uint32_t* base() const { return m_base; }
    
    inline XhciExtendedCapabilityCode id() const {
        return static_cast<XhciExtendedCapabilityCode>(m_entry.id);
    }

    inline kstl::SharedPtr<XhciExtendedCapability> next() const { return m_next; }

private:
    volatile uint32_t* m_base;
    XhciExtendedCapabilityEntry m_entry;

    kstl::SharedPtr<XhciExtendedCapability> m_next;

private:
    void _readNextExtCaps();
};

class XhciPortRegisterManager {
public:
    XhciPortRegisterManager(uint64_t base) : m_base(base) {}

    void readPortscReg(XhciPortscRegister& reg) const;
    void writePortscReg(XhciPortscRegister& reg) const;

    void readPortpmscRegUsb2(XhciPortpmscRegisterUsb2& reg) const;
    void writePortpmscRegUsb2(XhciPortpmscRegisterUsb2& reg) const;

    void readPortpmscRegUsb3(XhciPortpmscRegisterUsb3& reg) const;
    void writePortpmscRegUsb3(XhciPortpmscRegisterUsb3& reg) const;


    void readPortliReg(XhciPortliRegister& reg) const;
    void writePortliReg(XhciPortliRegister& reg) const;

    void readPorthlpmcRegUsb2(XhciPorthlpmcRegisterUsb2& reg) const;
    void writePorthlpmcRegUsb2(XhciPorthlpmcRegisterUsb2& reg) const;

    void readPorthlpmcRegUsb3(XhciPorthlpmcRegisterUsb3& reg) const;
    void writePorthlpmcRegUsb3(XhciPorthlpmcRegisterUsb3& reg) const;

private:
    uint64_t m_base;

    const size_t m_portscOffset     = 0x00;
    const size_t m_portpmscOffset   = 0x04;
    const size_t m_portliOffset     = 0x08;
    const size_t m_porthlpmcOffset  = 0x0C;
};

class XhciRuntimeRegisterManager {
public:
    XhciRuntimeRegisterManager(uint64_t base, uint8_t maxInterrupters)
        : m_base(reinterpret_cast<XhciRuntimeRegisters*>(base)),
          m_maxInterrupters(maxInterrupters) {}

    XhciInterrupterRegisters* getInterrupterRegisters(uint8_t interrupter) const;

private:
    XhciRuntimeRegisters*   m_base;
    uint8_t                 m_maxInterrupters;
};

class XhciDoorbellManager {
public:
    XhciDoorbellManager(uint64_t base);

    // TargeValue = 2 + (ZeroBasedEndpoint * 2) + (isOutEp ? 0 : 1)
    void ringDoorbell(uint8_t doorbell, uint8_t target);

    void ringCommandDoorbell();
    void ringControlEndpointDoorbell(uint8_t doorbell);

private:
    XhciDoorbellRegister* m_doorbellRegisters;
};

class XhciCommandRing {
public:
    XhciCommandRing(size_t maxTrbs);

    inline uint64_t getVirtualBase() const { return (uint64_t)m_trbRing; }
    inline uint64_t getPhysicalBase() const { return m_physicalRingBase; }
    inline uint8_t  getCycleBit() const { return m_rcsBit; }

    void enqueue(XhciTrb_t* trb);

private:
    size_t      m_maxTrbCount;      // Number of valid TRBs in the ring including the LINK_TRB
    size_t      m_enqueuePtr;       // Index in the ring where to enqueue next TRB
    XhciTrb_t*  m_trbRing;          // Virtual ring base
    uint64_t    m_physicalRingBase; // Physical ring base
    uint8_t     m_rcsBit;           // Ring cycle state
};

// For now only 1 segment is going to be used
class XhciEventRing {
public:
    XhciEventRing(size_t maxTrbs, XhciInterrupterRegisters* primaryInterrupterRegisters);

    inline uint64_t getVirtualBase() const { return (uint64_t)m_primarySegmentRing; }
    inline uint64_t getPhysicalBase() const { return m_primarySegmentPhysicalBase; }
    inline uint8_t  getCycleBit() const { return m_rcsBit; }

    bool hasUnprocessedEvents();
    void dequeueEvents(kstl::vector<XhciTrb_t*>& receivedEventTrbs);

    void flushUnprocessedEvents();

private:
    XhciInterrupterRegisters* m_interrupterRegs;

    size_t      m_segmentTrbCount;              // Max TRBs allowed on the segment

    XhciTrb_t*  m_primarySegmentRing;           // Primary segment's ring's virtual address
    uint64_t    m_primarySegmentPhysicalBase;   // Primary segment's ring's physical address

    const uint64_t m_segmentCount = 1;          // Number of segments to be allocated in the segment table
    XhciErstEntry* m_segmentTable;              // Segment table's virtual address
    uint64_t       m_segmentTablePhysicalBase;  // Segment table's physical address

    uint64_t       m_dequeuePtr;                // Event ring dequeue pointer
    uint8_t        m_rcsBit;                    // Ring cycle state

private:
    void _updateErdpInterrupterRegister();
    XhciTrb_t* _dequeueTrb();
};

class XhciTransferRing {
public:
    XhciTransferRing(size_t maxTrbs, uint8_t doorbellId);

    inline uint64_t getVirtualBase() const { return (uint64_t)m_trbRing; }
    inline uint64_t getPhysicalBase() const { return m_physicalRingBase; }
    inline uint8_t  getCycleBit() const { return m_dcsBit; }
    inline uint8_t getDoorbellId() const { return m_doorbellId; }

    void enqueue(XhciTrb_t* trb);

private:
    size_t      m_maxTrbCount;          // Number of valid TRBs in the ring including the LINK_TRB
    size_t      m_dequeuePtr;           // Transfer ring consumer dequeue pointer
    size_t      m_enqueuePtr;           // Transfer ring producer enqueue pointer
    XhciTrb_t*  m_trbRing;              // Virtual ring base
    uint64_t    m_physicalRingBase;     // Physical ring base
    uint8_t     m_dcsBit;               // Dequeue cycle state
    uint8_t     m_doorbellId;           // ID of the doorbell associated with the ring
};

class XhciDriver {
public:
    static XhciDriver& get();

public:
    XhciDriver() = default;
    ~XhciDriver() = default;

    bool init(PciDeviceInfo& deviceInfo);

private:
    uint64_t m_xhcBase;

    volatile XhciCapabilityRegisters* m_capRegs;
    volatile XhciOperationalRegisters* m_opRegs;

    void _parseCapabilityRegisters();
    void _logCapabilityRegisters();

    void _parseExtendedCapabilityRegisters();

    void _configureOperationalRegisters();
    void _logUsbsts();
    void _logOperationalRegisters();

    void _configureRuntimeRegisters();

    bool _isUSB3Port(uint8_t portNum);
    XhciPortRegisterManager _getPortRegisterSet(uint8_t portNum);

    void _setupDcbaa();

    // Creates a device context buffer and inserts it into DCBAA
    void _createDeviceContext(uint8_t slotId);

    XhciCommandCompletionTrb_t* _sendXhciCommand(XhciTrb_t* trb);

private:
    void _mapDeviceMmio(uint64_t pciBarAddress);

private:
    bool _resetHostController();
    void _startHostController();

    bool _resetPort(uint8_t portNum);
    uint8_t _requestDeviceSlot();
    void _setDeviceAddress(uint8_t port, uint8_t slotId, uint8_t portSpeed);

    void _markXhciInterruptCompleted(uint8_t interrupter);
    void _processEventRingTrb(XhciTrb_t* trb);

    void _handleDeviceConnected(uint8_t port);
    void _handleDeviceDiconnected(uint8_t port);

private:
    // CAPLENGTH
    uint8_t m_capabilityRegsLength;
    
    // HCSPARAMS1
    uint8_t m_maxDeviceSlots;
    uint8_t m_maxInterrupters;
    uint8_t m_maxPorts;

    // HCSPARAMS2
    uint8_t m_isochronousSchedulingThreshold;
    uint8_t m_erstMax;
    uint8_t m_maxScratchpadBuffers;

    // HCCPARAMS1
    bool m_64bitAddressingCapability;
    bool m_bandwidthNegotiationCapability;
    bool m_64ByteContextSize;
    bool m_portPowerControl;
    bool m_portIndicators;
    bool m_lightResetCapability;
    uint32_t m_extendedCapabilitiesOffset;

    // Linked list of extended capabilities
    kstl::SharedPtr<XhciExtendedCapability> m_extendedCapabilitiesHead;

    // Page size supported by host controller
    uint64_t m_hcPageSize;

    // USB3.x-specific ports
    kstl::vector<uint8_t> m_usb3Ports;

    // Device context base address array's virtual address
    uint64_t* m_dcbaa;

    // Controller class for runtime registers
    kstl::SharedPtr<XhciRuntimeRegisterManager> m_runtimeRegisterManager;

    // Main command ring
    kstl::SharedPtr<XhciCommandRing> m_commandRing;

    // Main event ring
    kstl::SharedPtr<XhciEventRing> m_eventRing;

    // Doorbell register array manager
    kstl::SharedPtr<XhciDoorbellManager> m_doorbellManager;
};
} // namespace drivers

#endif
