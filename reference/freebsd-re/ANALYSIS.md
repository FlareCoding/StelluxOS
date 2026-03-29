# FreeBSD re(4) Driver Analysis — Cross-Reference with Linux r8169

## Source Files Fetched

- `if_re.c` — Main driver source (~4155 lines), from `sys/dev/re/if_re.c`
- `if_rlreg.h` — Shared register header (~1167 lines), from `sys/dev/rl/if_rlreg.h`

The re(4) driver shares its register header (`if_rlreg.h`) with the rl(4) driver (for older RTL8139 chips). There is no separate `if_rereg.h`.

---

## 1. Register Definitions and Linux Comparison

### Base Register Map (from `if_rlreg.h`)

| FreeBSD Name | Offset | Linux Equivalent | Description |
|---|---|---|---|
| `RL_IDR0`–`RL_IDR5` | 0x00–0x05 | `MAC0`–`MAC5` | Station MAC address |
| `RL_MAR0`–`RL_MAR7` | 0x08–0x0F | `MAR0`–`MAR7` | Multicast hash filter |
| `RL_DUMPSTATS_LO/HI` | 0x10/0x14 | `CounterAddrLow/High` | DMA counter dump |
| `RL_TXLIST_ADDR_LO/HI` | 0x20/0x24 | `TxDescStartAddrLow/High` | TX ring base (256-byte aligned) |
| `RL_TXLIST_ADDR_HPRIO_LO/HI` | 0x28/0x2C | `TxHDescStartAddrLow/High` | High-priority TX ring |
| `RL_COMMAND` | 0x37 | `ChipCmd` | Command register |
| `RL_GTXSTART` | 0x38 | `TxPoll` | TX poll (8169 only) |
| `RL_IMR` | 0x3C | `IntrMask` | Interrupt mask |
| `RL_ISR` | 0x3E | `IntrStatus` | Interrupt status |
| `RL_TXCFG` | 0x40 | `TxConfig` | TX config (includes HWREV) |
| `RL_RXCFG` | 0x44 | `RxConfig` | RX config |
| `RL_EECMD` | 0x50 | `Cfg9346` | EEPROM command/config write enable |
| `RL_CFG0`–`RL_CFG5` | 0x51–0x56 | `Config0`–`Config5` | Config registers (8169) |
| `RL_PHYAR` | 0x60 | `PHYAR` | Gigabit PHY access register |
| `RL_GMEDIASTAT` | 0x6C | `PHYstatus` | Gigabit media status |
| `RL_MACDBG` | 0x6D | — | MAC debug (8168C SPIN2) |
| `RL_GPIO` | 0x6E | — | GPIO (8168C SPIN2) |
| `RL_PMCH` | 0x6F | — | Power management channel |
| `RL_CPLUS_CMD` | 0xE0 | `CPlusCmd` | C+ mode command |
| `RL_RXLIST_ADDR_LO/HI` | 0xE4/0xE8 | `RxDescAddrLow/High` | RX ring base |
| `RL_MAXRXPKTLEN` | 0xDA | `RxMaxSize` | Max RX packet length (chip multiplies by 8) |
| `RL_INTRMOD` | 0xE2 | `IntrMitigate` | Interrupt moderation |
| `RL_MISC` | 0xF0 | `MISC` | Miscellaneous register |

### Key Differences from Linux

1. **Naming convention**: FreeBSD uses `RL_` prefix (inherited from rl(4)), Linux uses descriptive names or `enum` values.
2. **Config register offsets**: FreeBSD has separate macros for 8139 vs 8169 config register offsets:
   - 8139: `RL_8139_CFG0` = 0x51, `RL_8139_CFG3` = 0x59, `RL_8139_CFG4` = 0x5A, `RL_8139_CFG5` = 0xD8
   - 8169: `RL_CFG0` = 0x51, `RL_CFG3` = 0x54, `RL_CFG4` = 0x55, `RL_CFG5` = 0x56
   - The softc stores these offsets (`sc->rl_cfg0` through `sc->rl_cfg5`) for chip-generic access.
3. **No ERIAR/OCP registers**: FreeBSD lacks the Extended Register Indirect Access Register (ERIAR at 0xDC) and OCP/MCU access used heavily by Linux r8169 for firmware loading and internal PHY configuration of newer chips.

---

## 2. Hardware Revision Identification

### Mechanism

Both FreeBSD and Linux read `TxConfig` (offset 0x40) to determine chip revision:

```c
// FreeBSD
hwrev = CSR_READ_4(sc, RL_TXCFG);
hwrev &= RL_TXCFG_HWREV;  // mask = 0x7CC00000
```

The `RL_TXCFG_HWREV` mask is `0x7CC00000` (bits 22-23, 26-30).

### Supported Chip Revisions (FreeBSD `re_hwrevs[]` table)

| HWREV Value | FreeBSD Desc | Type | Max MTU |
|---|---|---|---|
| `0x00000000` | 8169 | RL_8169 | JUMBO (7422) |
| `0x00800000` | 8169S | RL_8169 | JUMBO |
| `0x04000000` | 8110S | RL_8169 | JUMBO |
| `0x10000000` | 8169SB/8110SB | RL_8169 | JUMBO |
| `0x18000000` | 8169SC/8110SC | RL_8169 | JUMBO |
| `0x28000000` | 8168D/8111D | RL_8169 | JUMBO_9K |
| `0x28800000` | 8168DP/8111DP | RL_8169 | JUMBO_9K |
| `0x2C000000` | 8168E/8111E | RL_8169 | JUMBO_9K |
| `0x2C800000` | 8168E-VL/8111E-VL | RL_8169 | JUMBO_6K |
| `0x30000000` | 8168B (spin1) | RL_8169 | JUMBO |
| `0x38000000` | 8168B (spin2) | RL_8169 | JUMBO |
| `0x38400000` | 8168B (spin3) | RL_8169 | JUMBO |
| `0x3C000000` | 8168C/8111C | RL_8169 | JUMBO_6K |
| `0x3C400000` | 8168C SPIN2 | RL_8169 | JUMBO_6K |
| `0x3C800000` | 8168CP/8111CP | RL_8169 | JUMBO_6K |
| `0x48000000` | 8168F/8111F | RL_8169 | JUMBO_9K |
| `0x48800000` | 8411 | RL_8169 | JUMBO_9K |
| `0x4C000000` | 8168G/8111G | RL_8169 | JUMBO_9K |
| `0x50000000` | 8168EP/8111EP | RL_8169 | JUMBO_9K |
| `0x50800000` | 8168GU/8111GU | RL_8169 | JUMBO_9K |
| `0x54000000` | 8168H/8111H | RL_8169 | JUMBO_9K |
| `0x54800000` | 8168FP/8111FP | RL_8169 | JUMBO_9K |
| `0x5C800000` | 8411B | RL_8169 | JUMBO_9K |

### Linux Comparison

Linux uses `enum mac_version` (e.g., `RTL_GIGA_MAC_VER_02` through `RTL_GIGA_MAC_VER_80`) with a lookup table indexed by the TXCFG HWREV value. Linux also:
- Reads the MAC revision bits (`sc->rl_macrev` in FreeBSD, at bits 20-22)
- Supports newer chips (RTL8125, RTL8126) that FreeBSD re(4) does not
- Has finer-grained version distinctions for firmware loading

---

## 3. Init/Reset Sequence

### Reset (`re_reset`)

```
1. Write RL_CMD_RESET to RL_COMMAND (0x37)
2. Poll RL_COMMAND until RESET bit clears (timeout: RL_TIMEOUT = 1000 iterations × 10µs)
3. If RL_FLAG_MACRESET set: write 1 to register 0x82 (magic)
4. If chip is 8169S: write 0 to PHY register 0x0b via GMII
```

### Full Init Sequence (`re_init_locked`)

```
 1. re_stop(sc)             — Cancel I/O, free buffers
 2. re_reset(sc)            — Hardware reset
 3. Initialize RX descriptor ring (re_rx_list_init or re_jrx_list_init for jumbo)
 4. Initialize TX descriptor ring (re_tx_list_init)
    - EOR bit set on last descriptor
 5. Configure C+ mode (RL_CPLUS_CMD at 0xE0):
    - PCI_MRW, RXCSUM_ENB, VLANSTRIP
    - For MACSTAT chips: MACSTAT_DIS | 0x0001
    - For non-MACSTAT: RXENB | TXENB
 6. For 8169SC/8110SC: set register 0x7C to magic value, disable interrupt mitigation at 0xE2
 7. Write MAC address: RL_EECMD → WRITECFG mode, write IDR0/IDR4, back to OFF
 8. Load RX/TX ring addresses into RXLIST_ADDR and TXLIST_ADDR registers (64-bit)
 9. For 8168G+: disable RXDV gate (RL_MISC &= ~0x00080000)
10. For pre-8168G: enable TX/RX MACs (RL_COMMAND = TX_ENB|RX_ENB) BEFORE RX/TX config
11. Set TX config (RL_TXCFG_CONFIG = IFG | DMA_2048)
12. Set early TX threshold to 16
13. Set RX mode (re_set_rxmode) — configures RXCFG with filter + multicast hash
14. Configure interrupt moderation (RL_INTRMOD = 0x5100 for 8169)
15. For 8168G+: enable TX/RX MACs AFTER RX/TX config
16. Enable interrupts (RL_IMR = RL_INTRS_CPLUS)
17. Clear pending interrupts (RL_ISR = RL_INTRS_CPLUS)
18. Set TX threshold (96 bytes)
19. Clear missed packet counter
20. Set timer interrupt for TX moderation
21. Set max RX packet length (RL_MAXRXPKTLEN) per chip capabilities
22. Set RL_CFG1 DRVLOAD bit
23. Start MII autonegotiation
24. Start stat callout timer (1Hz)
```

### Key Ordering Difference for 8168G+

Pre-8168G chips: enable RX/TX MACs **before** setting TX/RX config.
8168G and later: enable RX/TX MACs **after** setting TX/RX config. Also requires RXDV gate manipulation (RL_MISC bit 19).

### Linux Comparison

Linux r8169 has per-chip-version init functions (`rtl_hw_start_8168X()`) with much more detailed PHY and internal register programming via ERIAR/OCP. FreeBSD uses a single init path with flag-based conditionals.

---

## 4. PHY Access Method

### GMII PHY Access (8169 family, including all RTL8168/8111)

The PHY is accessed via the `RL_PHYAR` register at offset 0x60:

**Read:**
```c
CSR_WRITE_4(sc, RL_PHYAR, reg << 16);     // Write register number
// Poll until BUSY bit (0x80000000) is SET (indicates read complete)
// Timeout: RL_PHY_TIMEOUT = 2000 iterations × 25µs
rval = CSR_READ_4(sc, RL_PHYAR) & RL_PHYAR_PHYDATA;  // Lower 16 bits
DELAY(20);  // 20µs delay required between MDIO operations
```

**Write:**
```c
CSR_WRITE_4(sc, RL_PHYAR, (reg << 16) | (data & 0xFFFF) | RL_PHYAR_BUSY);
// Poll until BUSY bit CLEARS
DELAY(20);  // 20µs inter-operation delay
```

**GMEDIASTAT special case:**
Register 0x6C (`RL_GMEDIASTAT`) is read directly as a 1-byte CSR read, not through PHYAR.

### 8139C+ PHY Access

Direct register-mapped MII registers:
- `RL_BMCR` (0x62), `RL_BMSR` (0x64), `RL_ANAR` (0x66), `RL_LPAR` (0x68), `RL_ANER` (0x6A)

### Linux Comparison

Linux uses the same PHYAR mechanism but also implements:
- Extended PHY register access via ERIAR for internal PHY pages
- OCP register access for newer chips (8168G+)
- Dedicated firmware loading through MCU access
- PHY register page switching (writing to register 0x1F)

FreeBSD does set PHY page 0 during attach (`re_gmii_writereg(dev, 1, 0x1f, 0)`) but doesn't do the extensive per-chip PHY initialization that Linux does.

---

## 5. TX/RX Descriptor Format

### Descriptor Structure (`struct rl_desc`)

```c
struct rl_desc {
    uint32_t rl_cmdstat;       // Command/status
    uint32_t rl_vlanctl;       // VLAN control
    uint32_t rl_bufaddr_lo;    // Buffer address (low 32)
    uint32_t rl_bufaddr_hi;    // Buffer address (high 32)
};
```

16 bytes per descriptor. Identical layout in Linux.

### TX Descriptor Fields (`rl_cmdstat`)

| Bit(s) | FreeBSD Name | Value | Description |
|---|---|---|---|
| 0-15 | `RL_TDESC_CMD_FRAGLEN` | 0x0000FFFF | Fragment length |
| 16 | `RL_TDESC_CMD_TCPCSUM` | 0x00010000 | TCP checksum enable (v1) |
| 17 | `RL_TDESC_CMD_UDPCSUM` | 0x00020000 | UDP checksum enable (v1) |
| 18 | `RL_TDESC_CMD_IPCSUM` | 0x00040000 | IP checksum enable (v1) |
| 16-26 | `RL_TDESC_CMD_MSSVAL` | 0x07FF0000 | TSO MSS value (v1) |
| 27 | `RL_TDESC_CMD_LGSEND` | 0x08000000 | TSO enable |
| 28 | `RL_TDESC_CMD_EOF` | 0x10000000 | End of frame |
| 29 | `RL_TDESC_CMD_SOF` | 0x20000000 | Start of frame |
| 30 | `RL_TDESC_CMD_EOR` | 0x40000000 | End of ring |
| 31 | `RL_TDESC_CMD_OWN` | 0x80000000 | Owned by hardware |

### TX Descriptor v2 Fields (`rl_vlanctl`, for 8168C+ with RL_FLAG_DESCV2)

| Bit(s) | FreeBSD Name | Value | Description |
|---|---|---|---|
| 18-28 | `RL_TDESC_CMD_MSSVALV2` | 0x1FFC0000 | TSO MSS (shifted by 18) |
| 29 | `RL_TDESC_CMD_IPCSUMV2` | 0x20000000 | IP checksum |
| 30 | `RL_TDESC_CMD_TCPCSUMV2` | 0x40000000 | TCP checksum |
| 31 | `RL_TDESC_CMD_UDPCSUMV2` | 0x80000000 | UDP checksum |

### RX Descriptor Fields (`rl_cmdstat`)

| Bit(s) | FreeBSD Name | Value | Description |
|---|---|---|---|
| 0-12 | `RL_RDESC_STAT_FRAGLEN` | 0x00001FFF | Frame length (8139C+) |
| 0-13 | `RL_RDESC_STAT_GFRAGLEN` | 0x00003FFF | Frame length (8169, 14 bits) |
| 13 | `RL_RDESC_STAT_TCPSUMBAD` | 0x00002000 | TCP checksum bad |
| 14 | `RL_RDESC_STAT_UDPSUMBAD` | 0x00004000 | UDP checksum bad |
| 15 | `RL_RDESC_STAT_IPSUMBAD` | 0x00008000 | IP checksum bad |
| 16-17 | `RL_RDESC_STAT_PROTOID` | 0x00030000 | Protocol ID |
| 18 | `RL_RDESC_STAT_CRCERR` | 0x00040000 | CRC error |
| 19 | `RL_RDESC_STAT_RUNT` | 0x00080000 | Runt frame |
| 20 | `RL_RDESC_STAT_RXERRSUM` | 0x00100000 | RX error summary |
| 21 | `RL_RDESC_STAT_GIANT` | 0x00200000 | Giant frame |
| 28 | `RL_RDESC_STAT_EOF` | 0x10000000 | End of frame |
| 29 | `RL_RDESC_STAT_SOF` | 0x20000000 | Start of frame |
| 30 | `RL_RDESC_CMD_EOR` | 0x40000000 | End of ring |
| 31 | `RL_RDESC_CMD_OWN` | 0x80000000 | Owned by hardware |

### RX Descriptor v2 (`rl_vlanctl`, for 8168C+)

| Bit(s) | FreeBSD Name | Value | Description |
|---|---|---|---|
| 30 | `RL_RDESC_IPV4` | 0x40000000 | IPv4 packet |
| 31 | `RL_RDESC_IPV6` | 0x80000000 | IPv6 packet |

### Ring Parameters

| Parameter | 8139C+ | 8169/8168 |
|---|---|---|
| TX descriptors | 64 | 256 |
| RX descriptors | 64 | 256 |
| Ring alignment | 256 bytes | 256 bytes |
| Max TX segments | 35 | 35 |
| RX length mask | 13 bits (0x1FFF) | 14 bits (0x3FFF) |

### Descriptor Ring Wrapping

FreeBSD uses `RL_TDESC_CMD_EOR` / `RL_RDESC_CMD_EOR` on the **last** descriptor in the ring to mark ring wrap. This is the same as Linux. **No next-pointer chaining** — the ring is a flat array.

---

## 6. Chip-Specific Quirks for RTL8168 Variants

### Per-Revision Flags Set During `re_attach`

| Chip | Flags |
|---|---|
| **8168B spin1/2** | WOLRXENB, PHYWAKE, MACSTAT |
| **8168B spin3** | PHYWAKE, MACSTAT |
| **8168C** | PHYWAKE, PAR, DESCV2, MACSTAT, CMDSTOP, AUTOPAD, JUMBOV2, WOL_MANLINK; MACSLEEP if macrev==0x00200000 |
| **8168C SPIN2** | Same as 8168C + MACSLEEP always |
| **8168CP** | Same flags as 8168C (via fallthrough) |
| **8168D** | PHYWAKE, PHYWAKE_PM, PAR, DESCV2, MACSTAT, CMDSTOP, AUTOPAD, JUMBOV2, WOL_MANLINK |
| **8168DP** | PHYWAKE, PAR, DESCV2, MACSTAT, AUTOPAD, JUMBOV2, WAIT_TXPOLL, WOL_MANLINK |
| **8168E** | PHYWAKE, PHYWAKE_PM, PAR, DESCV2, MACSTAT, CMDSTOP, AUTOPAD, JUMBOV2, WOL_MANLINK |
| **8168E-VL** | EARLYOFF + PHYWAKE, PAR, DESCV2, MACSTAT, CMDSTOP, AUTOPAD, JUMBOV2, CMDSTOP_WAIT_TXQ, WOL_MANLINK |
| **8168F** | EARLYOFF + same as 8168E-VL |
| **8168G** | 8168G_PLUS + PAR, DESCV2, MACSTAT, CMDSTOP, AUTOPAD, JUMBOV2, CMDSTOP_WAIT_TXQ, WOL_MANLINK |
| **8168GU/8168H** | 8168G_PLUS flags; if device ID is 8101E → add FASTETHER (RTL8106E/8107E) |

### Notable Quirks

1. **TX checksum corruption (8168C/8168CP)**: TX checksum offload is **disabled** for these chips because they generate corrupt frames when IP options are present. Only RXCSUM and TSO4 are enabled.

2. **Multicast hash register byte order (PCIe chips)**: PCIe variants have **reversed** multicast hash register order. The driver byte-swaps `hashes[0]`/`hashes[1]` for `RL_FLAG_PCIE` chips.

3. **Multicast filter bug (8168F)**: Multicast filtering is **disabled** (all-ones hash) due to a silicon bug.

4. **AUTOPAD bug**: For non-AUTOPAD chips, short frames with IP checksum offload get garbled payload. The driver manually pads frames < 60 bytes when `RL_FLAG_AUTOPAD` is not set.

5. **Deep sleep mode (8168C SPIN2)**: Uses `RL_MACDBG` (0x6D) and `RL_GPIO` (0x6E) to enter/exit deep sleep.

6. **PHY power down (8168D+)**: Chips with `RL_FLAG_PHYWAKE_PM` require writing to `RL_PMCH` (0x6F) to take PHY out of power down. 8401E additionally needs register 0xD1 bit 3 cleared.

7. **ASPM disabled**: The driver unconditionally disables PCIe ASPM L0S/L1 and CLKREQ for all PCIe chips.

8. **BAR selection**: RTL8168/8101E use BAR(2) for MMIO; RTL8169SC forces I/O port mapping. Others default to BAR(1) MMIO with BAR(0) I/O fallback.

9. **DAC (64-bit DMA)**: Only enabled for PCIe chips. PCI chips are limited to 32-bit DMA addresses.

10. **8168G+ init ordering**: TX/RX enable comes AFTER TX/RX configuration (reversed from older chips). Also requires RXDV gate manipulation via `RL_MISC` register.

11. **Stop sequence varies by chip**:
    - `RL_FLAG_WAIT_TXPOLL` (8168DP): Poll TXSTART for completion, then clear COMMAND
    - `RL_FLAG_CMDSTOP` (8168C+): Issue STOPREQ command
    - `RL_FLAG_CMDSTOP_WAIT_TXQ` (8168E-VL+): Wait for TXCFG QUEUE_EMPTY after STOPREQ
    - Default (old chips): Just clear COMMAND register

12. **RXDV gate (8168G+)**: Must be disabled during init (`RL_MISC &= ~0x00080000`) and re-enabled during stop.

13. **Interrupt moderation (8169SC)**: Register 0x7C gets a magic clock value, and 0xE2 interrupt mitigation is explicitly disabled.

14. **Jumbo frame handling**: 
    - 8168C+: `RL_FLAG_JUMBOV2` — separate jumbo RX descriptor ring, checksum offload disabled for jumbo
    - 8168E-VL: Jumbo requires `pci_set_max_read_req(4096)`, no CFG3/CFG4 jumbo bits
    - 8168DP: Always `pci_set_max_read_req(4096)`
    - Others: `pci_set_max_read_req(512)` for jumbo, `4096` for normal

15. **TSO disabled by default**: Despite being capable, TSO is disabled by default due to known corrupted TCP segment generation.

---

## 7. Stop Sequence (`re_stop`)

```
1. Disable RX filters (clear ALLPHYS, INDIV, MULTI, BROAD from RXCFG)
2. For 8168G+: enable RXDV gate (RL_MISC |= 0x00080000)
3. Stop TX/RX based on chip flags:
   a. WAIT_TXPOLL: poll TXSTART until idle, clear COMMAND
   b. CMDSTOP: write STOPREQ|TX_ENB|RX_ENB to COMMAND
      - If CMDSTOP_WAIT_TXQ: wait for TXCFG QUEUE_EMPTY
   c. Default: write 0x00 to COMMAND
4. DELAY(1000)
5. Disable interrupts (IMR = 0)
6. Acknowledge all interrupts (ISR = 0xFFFF)
7. Free TX/RX mbuf chains
```

---

## 8. Interrupt Handling

### Handler Types

1. **Legacy INTx** (`re_intr`): Reads ISR, masks IMR to 0, enqueues taskqueue
2. **MSI** (`re_intr_msi`): Direct processing with RX interrupt moderation via timer

### ISR Bits Used

```c
#define RL_INTRS_CPLUS  (RX_OK|RX_ERR|TX_ERR|TX_OK|RX_OVERRUN|PKT_UNDERRUN|
                         FIFO_OFLOW|PCS_TIMEOUT|SYSTEM_ERR|TIMEOUT_EXPIRED)
```

With `RE_TX_MODERATION`: TX_OK is removed from the mask (timer fires instead).

### PCIe TX restart quirk

After TX_OK or TX_DESC_UNAVAIL on PCIe chips, the driver writes `RL_TXSTART_START` to restart the transmitter, as these chips ignore second TX requests while a transmission is in progress.

---

## 9. DMA Architecture

### TX Path (`re_encap` → `re_start_tx`)

1. `bus_dmamap_load_mbuf_sg()` maps mbuf into scatter-gather segments
2. If too many segments: `m_collapse()` to ≤ 35 segments
3. Fill descriptors: bufaddr, length, csum flags, VLAN tag
4. First descriptor gets OWN+SOF last, last descriptor gets EOF
5. DMA map is swapped to the last descriptor index (for cleanup)
6. `bus_dmamap_sync(PREWRITE)` flushes to device
7. Write `RL_TXSTART_START` to kick TX

### RX Path (`re_rxeof`)

1. `bus_dmamap_sync(POSTREAD|POSTWRITE)` on descriptor ring
2. Walk ring from `rl_rx_prodidx`, check OWN bit
3. Max 16 packets per call (interrupt moderation)
4. For 8169: status word shifted right by 1 bit for bit-layout compatibility with 8139C+
5. Multi-fragment packet support via `sc->rl_head`/`sc->rl_tail` chain
6. Allocate replacement mbuf before consuming received one
7. Checksum offload: v1 (protoid-based) vs v2 (explicit TCP/UDP/IP flags in vlanctl)

---

## 10. Key Differences: FreeBSD re(4) vs Linux r8169

| Feature | FreeBSD re(4) | Linux r8169 |
|---|---|---|
| **PHY init** | Minimal (set page 0, clear power down) | Extensive per-chip PHY programming, firmware loading |
| **Firmware** | None | Per-chip firmware blobs loaded via MCU/OCP |
| **Register access** | PHYAR only | PHYAR + ERIAR + OCP for internal registers |
| **Chip versions** | Up to 8168H/8411B | Up to RTL8126 (MAC_VER_80) |
| **Init path** | Single function with flag branches | Per-chip `rtl_hw_start_XXXX()` functions |
| **Interrupt coalescing** | Timer-based moderation | Finer-grained per-packet coalescing |
| **NAPI/polling** | DEVICE_POLLING (optional) | Always NAPI |
| **TSO** | Disabled by default | Enabled by default on capable chips |
| **DMA allocation** | FreeBSD bus_dma | Linux DMA API |
| **Thread safety** | Mutex (`mtx`) | NAPI serialization |
| **MSI-X** | Supported (1 vector) | Supported |
| **Netmap** | Optional support | Not in mainline |

---

## 11. EEPROM Access

```
1. Set EECMD to RL_EEMODE_PROGRAM mode
2. For each word:
   a. Set RL_EE_SEL (chip select)
   b. Clock out address + READ command bit-by-bit via EE_DATAIN/EE_CLK
   c. Clock in 16-bit data word via EE_DATAOUT/EE_CLK
   d. Clear RL_EE_SEL
3. Clear EEMODE_PROGRAM
```

EEPROM width auto-detected: read word 0; if not `0x8129`, switch from 8-bit (93C56) to 6-bit (93C46) addressing.

MAC address: stored at EEPROM offset 7 (`RL_EE_EADDR`), 3 words (6 bytes). Newer chips with `RL_FLAG_PAR` read MAC directly from `RL_IDR0`–`RL_IDR5` CSRs instead.

---

## 12. WOL (Wake-on-LAN) Implementation

1. Put chip in sleep mode (if MACSLEEP flag)
2. For WOL-capable chips:
   - Disable RXDV gate (8168G+)
   - Set RX mode for wake patterns
   - Negotiate 10/100 link (if WOL_MANLINK)
   - Enable RX (if WOLRXENB)
3. Set config registers:
   - CFG1: PME enable
   - CFG3: WOL_MAGIC
   - CFG5: WOL_UCAST, WOL_MCAST, WOL_BCAST, WOL_LANWAKE
4. Request PME from PCI subsystem
