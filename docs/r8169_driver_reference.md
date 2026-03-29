# Linux r8169 Driver Reference for Stellux Port

Extracted from Linux kernel `drivers/net/ethernet/realtek/r8169_main.c` and `r8169.h`.

---

## 1. Key Constants and Sizing

```c
#define TX_DMA_BURST    7       /* Maximum PCI burst, '7' is unlimited */
#define InterFrameGap   0x03    /* Shortest InterFrameGap */
#define R8169_REGS_SIZE 256
#define R8169_RX_BUF_SIZE  (SZ_16K - 1)   /* 16383 bytes */
#define NUM_TX_DESC     256
#define NUM_RX_DESC     256
#define R8169_TX_RING_BYTES  (NUM_TX_DESC * sizeof(struct TxDesc))  /* 256 * 16 = 4096 */
#define R8169_RX_RING_BYTES  (NUM_RX_DESC * sizeof(struct RxDesc))  /* 256 * 16 = 4096 */
#define R8169_TX_STOP_THRS   (MAX_SKB_FRAGS + 1)
#define R8169_TX_START_THRS  (2 * R8169_TX_STOP_THRS)
#define OCP_STD_PHY_BASE     0xa400

/* MMIO access macros */
#define RTL_W8(tp, reg, val8)   writeb((val8), tp->mmio_addr + (reg))
#define RTL_W16(tp, reg, val16) writew((val16), tp->mmio_addr + (reg))
#define RTL_W32(tp, reg, val32) writel((val32), tp->mmio_addr + (reg))
#define RTL_R8(tp, reg)         readb(tp->mmio_addr + (reg))
#define RTL_R16(tp, reg)        readw(tp->mmio_addr + (reg))
#define RTL_R32(tp, reg)        readl(tp->mmio_addr + (reg))
```

---

## 2. MAC Version Enum (`enum mac_version`)

From `r8169.h`:

```c
enum mac_version {
    RTL_GIGA_MAC_VER_02,   // RTL8169s
    RTL_GIGA_MAC_VER_03,   // RTL8110s
    RTL_GIGA_MAC_VER_04,   // RTL8169sb/8110sb
    RTL_GIGA_MAC_VER_05,   // RTL8169sc/8110sc
    RTL_GIGA_MAC_VER_06,   // RTL8169sc/8110sc
    RTL_GIGA_MAC_VER_07,   // RTL8102e
    RTL_GIGA_MAC_VER_08,   // RTL8102e
    RTL_GIGA_MAC_VER_09,   // RTL8102e/8103e
    RTL_GIGA_MAC_VER_10,   // RTL8101e/8100e
    RTL_GIGA_MAC_VER_14,   // RTL8401
    RTL_GIGA_MAC_VER_17,   // RTL8168b/8111b
    RTL_GIGA_MAC_VER_18,   // RTL8168cp/8111cp
    RTL_GIGA_MAC_VER_19,   // RTL8168c/8111c
    RTL_GIGA_MAC_VER_20,   // RTL8168c/8111c
    RTL_GIGA_MAC_VER_21,   // RTL8168c/8111c
    RTL_GIGA_MAC_VER_22,   // RTL8168c/8111c
    RTL_GIGA_MAC_VER_23,   // RTL8168cp/8111cp
    RTL_GIGA_MAC_VER_24,   // RTL8168cp/8111cp
    RTL_GIGA_MAC_VER_25,   // RTL8168d/8111d
    RTL_GIGA_MAC_VER_26,   // RTL8168d/8111d
    RTL_GIGA_MAC_VER_28,   // RTL8168dp/8111dp
    RTL_GIGA_MAC_VER_29,   // RTL8105e
    RTL_GIGA_MAC_VER_30,   // RTL8105e
    RTL_GIGA_MAC_VER_31,   // RTL8168dp/8111dp
    RTL_GIGA_MAC_VER_32,   // RTL8168e/8111e
    RTL_GIGA_MAC_VER_33,   // RTL8168e/8111e
    RTL_GIGA_MAC_VER_34,   // RTL8168evl/8111evl
    RTL_GIGA_MAC_VER_35,   // RTL8168f/8111f
    RTL_GIGA_MAC_VER_36,   // RTL8168f/8111f
    RTL_GIGA_MAC_VER_37,   // RTL8402
    RTL_GIGA_MAC_VER_38,   // RTL8411
    RTL_GIGA_MAC_VER_39,   // RTL8106e
    RTL_GIGA_MAC_VER_40,   // RTL8168g/8111g
    RTL_GIGA_MAC_VER_42,   // RTL8168gu/8111gu
    RTL_GIGA_MAC_VER_43,   // (same handler as 42)
    RTL_GIGA_MAC_VER_44,   // RTL8411b
    RTL_GIGA_MAC_VER_46,   // RTL8168h/8111h
    RTL_GIGA_MAC_VER_48,   // (same handler as 46)
    RTL_GIGA_MAC_VER_51,   // RTL8168ep/8111ep
    RTL_GIGA_MAC_VER_52,   // RTL8168fp/RTL8117
    RTL_GIGA_MAC_VER_61,   // RTL8125A  (2.5G)
    RTL_GIGA_MAC_VER_63,   // RTL8125B  (2.5G)
    RTL_GIGA_MAC_VER_64,   // RTL8125D  (2.5G)
    RTL_GIGA_MAC_VER_66,   // RTL8125BP (2.5G)
    RTL_GIGA_MAC_VER_70,   // RTL8126A  (5G)
    RTL_GIGA_MAC_VER_80,   // RTL8127A  (10G?)
    RTL_GIGA_MAC_NONE
};
```

### Chip Version Detection (PCI ID `10ec:8168`)

The chip version is read from `TxConfig` register bits `[31:20]` masked with the chip table:

```c
xid = (RTL_R32(tp, TxConfig) >> 20) & 0xfcf;
```

Then matched against `rtl_chip_infos[]` entries `{ mask, val, mac_version, name, fw_name }`:

| mask  | val   | mac_version | Chip Name |
|-------|-------|-------------|-----------|
| 0x7c8 | 0x380 | VER_17 | RTL8168b/8111b |
| 0x7cf | 0x3c0 | VER_19 | RTL8168c/8111c |
| 0x7cf | 0x3c2 | VER_20 | RTL8168c/8111c |
| 0x7cf | 0x3c3 | VER_21 | RTL8168c/8111c |
| 0x7c8 | 0x3c0 | VER_22 | RTL8168c/8111c |
| 0x7cf | 0x3c8 | VER_18 | RTL8168cp/8111cp |
| 0x7cf | 0x3c9 | VER_23 | RTL8168cp/8111cp |
| 0x7c8 | 0x3c8 | VER_24 | RTL8168cp/8111cp |
| 0x7cf | 0x281 | VER_25 | RTL8168d/8111d |
| 0x7c8 | 0x280 | VER_26 | RTL8168d/8111d |
| 0x7cf | 0x28a | VER_28 | RTL8168dp/8111dp |
| 0x7cf | 0x28b | VER_31 | RTL8168dp/8111dp |
| 0x7cf | 0x2c1 | VER_32 | RTL8168e/8111e |
| 0x7c8 | 0x2c0 | VER_33 | RTL8168e/8111e |
| 0x7c8 | 0x2c8 | VER_34 | RTL8168evl/8111evl |
| 0x7cf | 0x480 | VER_35 | RTL8168f/8111f |
| 0x7cf | 0x481 | VER_36 | RTL8168f/8111f |
| 0x7c8 | 0x488 | VER_38 | RTL8411 |
| 0x7cf | 0x4c0 | VER_40 | RTL8168g/8111g |
| 0x7cf | 0x509 | VER_42 | RTL8168gu/8111gu |
| 0x7cf | 0x5c8 | VER_44 | RTL8411b |
| 0x7cf | 0x541 | VER_46 | RTL8168h/8111h |
| 0x7cf | 0x6c0 | VER_46 | RTL8168M (handled as 8168H) |
| 0x7cf | 0x502 | VER_51 | RTL8168ep/8111ep |
| 0x7cf | 0x54a | VER_52 | RTL8168fp/RTL8117 |
| 0x7cf | 0x54b | VER_52 | RTL8168fp/RTL8117 |

---

## 3. Register Map (`enum rtl_registers`)

### Core Registers (all chips)
| Offset | Name | Description |
|--------|------|-------------|
| 0x00 | MAC0 | Ethernet MAC address bytes 0-3 |
| 0x04 | MAC4 | Ethernet MAC address bytes 4-5 |
| 0x08 | MAR0 | Multicast filter (64-bit) |
| 0x10 | CounterAddrLow | Tally counter DMA address (low) |
| 0x14 | CounterAddrHigh | Tally counter DMA address (high) |
| 0x20 | TxDescStartAddrLow | TX descriptor ring base (low) |
| 0x24 | TxDescStartAddrHigh | TX descriptor ring base (high) |
| 0x28 | TxHDescStartAddrLow | TX high-prio descriptor (low) |
| 0x2c | TxHDescStartAddrHigh | TX high-prio descriptor (high) |
| 0x30 | FLASH | Flash memory access |
| 0x36 | ERSR | Early Rx Status |
| **0x37** | **ChipCmd** | **Chip command register** |
| **0x38** | **TxPoll** | **Transmit polling** |
| **0x3c** | **IntrMask** | **Interrupt mask (16-bit)** |
| **0x3e** | **IntrStatus** | **Interrupt status (16-bit)** |
| **0x40** | **TxConfig** | **TX configuration (also contains chip version ID)** |
| **0x44** | **RxConfig** | **RX configuration** |
| 0x50 | Cfg9346 | EEPROM control (lock/unlock config) |
| 0x51 | Config0 | |
| 0x52 | Config1 | |
| 0x53 | Config2 | |
| 0x54 | Config3 | |
| 0x55 | Config4 | |
| 0x56 | Config5 | |
| **0x60** | **PHYAR** | **PHY access register** |
| **0x6c** | **PHYstatus** | **PHY status (link speed/duplex)** |
| 0xda | RxMaxSize | Max RX packet size |
| **0xe0** | **CPlusCmd** | **C+ command register** |
| 0xe2 | IntrMitigate | Interrupt moderation |
| **0xe4** | **RxDescAddrLow** | **RX descriptor ring base (low)** |
| **0xe8** | **RxDescAddrHigh** | **RX descriptor ring base (high)** |
| 0xec | EarlyTxThres / MaxTxPacketSize | 8169: early TX thresh; 8168: max TX size |

### 8168/8101 Additional Registers (`enum rtl8168_8101_registers`)
| Offset | Name | Description |
|--------|------|-------------|
| 0x64 | CSIDR | CSI data register |
| 0x68 | CSIAR | CSI access register |
| 0x6f | PMCH | Power management channel |
| 0x80 | EPHYAR | Embedded PHY access |
| 0xd0 | DLLPR | DLL parameter |
| 0xd1 | DBG_REG | Debug register |
| 0xd2 | TWSI | Two-wire serial interface |
| 0xd3 | MCU | MCU register |
| 0xdc | EFUSEAR | eFuse access |
| 0xf2 | MISC_1 | Miscellaneous 1 |

### 8168 Additional Registers (`enum rtl8168_registers`)
| Offset | Name | Description |
|--------|------|-------------|
| 0x18 | LED_CTRL | LED control |
| 0x1a | LED_FREQ | LED frequency |
| 0x1b | EEE_LED | EEE LED control |
| 0x70 | ERIDR | Extended Register Interface data |
| 0x74 | ERIAR | Extended Register Interface access |
| 0x7c | EPHY_RXER_NUM | EPHY RX error count |
| 0xb0 | OCPDR | OCP GPHY access data |
| 0xb4 | OCPAR | OCP GPHY access parameters |
| 0xb8 | GPHY_OCP | GPHY OCP register |
| 0xd0 | RDSAR1 | 8168c only |
| 0xf0 | MISC | 8168e+ miscellaneous |

---

## 4. Descriptor Format

Both TX and RX descriptors are 16 bytes (128 bits), 256-byte aligned rings:

```c
struct TxDesc {
    __le32 opts1;    /* flags + length */
    __le32 opts2;    /* VLAN tag, checksum offload */
    __le64 addr;     /* DMA buffer physical address */
};

struct RxDesc {
    __le32 opts1;    /* flags + length */
    __le32 opts2;    /* VLAN tag, protocol info */
    __le64 addr;     /* DMA buffer physical address */
};
```

### Descriptor Flag Bits (opts1)

```c
/* Common to both TX and RX */
DescOwn   = (1 << 31)   /* NIC owns this descriptor */
RingEnd   = (1 << 30)   /* Last descriptor in ring (wraps) */
FirstFrag = (1 << 29)   /* First segment of packet */
LastFrag  = (1 << 28)   /* Last segment of packet */
```

### TX-specific opts1 flags
```c
/* Generic */
TD_LSO = (1 << 27)      /* Large Send Offload (TSO) */
/* TD_MSS_MAX = 0x07ff  (11-bit MSS value) */

/* 8169/8168b/810x (not 8102e) - "v1" checksum */
TD0_TCP_CS = (1 << 16)
TD0_UDP_CS = (1 << 17)
TD0_IP_CS  = (1 << 18)
/* TD0_MSS_SHIFT = 16 */

/* 8102e, 8168c and beyond - "v2" checksum */
TD1_GTSENV4 = (1 << 26)  /* Giant Send IPv4 */
TD1_GTSENV6 = (1 << 25)  /* Giant Send IPv6 */
/* GTTCPHO_SHIFT = 18, GTTCPHO_MAX = 0x7f */
/* opts2 v2 flags: */
TD1_IPv6_CS = (1 << 28)
TD1_IPv4_CS = (1 << 29)
TD1_TCP_CS  = (1 << 30)
TD1_UDP_CS  = (1 << 31)
/* TD1_MSS_SHIFT = 18 in opts2 */
/* TCPHO_SHIFT = 18, TCPHO_MAX = 0x3ff */
```

### TX opts2 flags
```c
TxVlanTag = (1 << 17)    /* Insert VLAN tag */
/* VLAN TCI in low 16 bits, byte-swapped */
```

### RX opts1 flags
```c
/* Bits [13:0] = packet length (including FCS) */
/* Error flags in opts1: */
RxRWT  = (1 << 22)       /* Watchdog timer expired */
RxRES  = (1 << 21)       /* Receive error summary */
RxRUNT = (1 << 20)       /* Runt packet */
RxCRC  = (1 << 19)       /* CRC error */
```

### RX opts2 flags (checksum / protocol)
```c
PID1 = (1 << 18)
PID0 = (1 << 17)
RxProtoUDP  = PID1
RxProtoTCP  = PID0
RxProtoIP   = (PID1 | PID0)

IPFail  = (1 << 16)      /* IP checksum failed */
UDPFail = (1 << 15)      /* UDP checksum failed */
TCPFail = (1 << 14)      /* TCP checksum failed */

RxVlanTag = (1 << 16)    /* VLAN tag present */
/* VLAN TCI in low 16 bits of opts2, byte-swapped */
```

---

## 5. rtl8169_private Structure

```c
struct rtl8169_private {
    void __iomem *mmio_addr;        /* MMIO base address */
    struct pci_dev *pci_dev;
    struct net_device *dev;
    struct phy_device *phydev;
    struct napi_struct napi;
    enum mac_version mac_version;
    enum rtl_dash_type dash_type;

    u32 cur_rx;                     /* Next RX descriptor to check */
    u32 cur_tx;                     /* Next TX descriptor to use */
    u32 dirty_tx;                   /* Oldest uncompleted TX descriptor */

    struct TxDesc *TxDescArray;     /* 256-aligned TX descriptor ring (DMA-coherent) */
    struct RxDesc *RxDescArray;     /* 256-aligned RX descriptor ring (DMA-coherent) */
    dma_addr_t TxPhyAddr;          /* TX ring physical address */
    dma_addr_t RxPhyAddr;          /* RX ring physical address */

    struct page *Rx_databuff[NUM_RX_DESC];  /* RX data buffer pages */
    struct ring_info tx_skb[NUM_TX_DESC];   /* TX metadata (skb + len) */

    u16 cp_cmd;                     /* Cached CPlusCmd register value */
    u16 tx_lpi_timer;
    u32 irq_mask;                   /* Active interrupt mask */
    int irq;
    struct clk *clk;

    struct {
        DECLARE_BITMAP(flags, RTL_FLAG_MAX);
        struct work_struct work;
    } wk;

    raw_spinlock_t mac_ocp_lock;
    struct mutex led_lock;

    unsigned supports_gmii:1;
    unsigned aspm_manageable:1;
    unsigned dash_enabled:1;
    bool sfp_mode:1;

    dma_addr_t counters_phys_addr;
    struct rtl8169_counters *counters;
    struct rtl8169_tc_offsets tc_offset;
    u32 saved_wolopts;

    const char *fw_name;
    struct rtl_fw *rtl_fw;
    struct r8169_led_classdev *leds;
    u32 ocp_base;                   /* OCP PHY base register */
};
```

---

## 6. PCI Probe / Initialization (`rtl_init_one`)

Full sequence:

```
1. alloc_etherdev(sizeof(rtl8169_private))
2. pcim_enable_device(pdev)
3. pcim_set_mwi(pdev)
4. pcim_iomap_region(pdev, region)  → tp->mmio_addr
5. Read TxConfig register:
     xid = (RTL_R32(tp, TxConfig) >> 20) & 0xfcf
6. Match xid against rtl_chip_infos[] → mac_version, fw_name
7. pci_disable_link_state(PCIE_LINK_STATE_L1) unless ASPM safe
8. tp->dash_type = rtl_get_dash_type(tp)
9. tp->cp_cmd = RTL_R16(tp, CPlusCmd) & CPCMD_MASK
10. Set DMA mask (64-bit for VER_18+)
11. rtl_init_rxcfg(tp)         — set initial RX config per chip
12. rtl8169_irq_mask_and_ack(tp) — disable & ack all interrupts
13. rtl_hw_initialize(tp)      — chip-specific early init
14. rtl_hw_reset(tp)           — write CmdReset, wait for clear
15. Allocate IRQ (MSI/MSI-X for VER_18+, INTX for older)
16. tp->irq = pci_irq_vector(pdev, 0)
17. INIT_WORK(&tp->wk.work, rtl_task)
18. rtl_init_mac_address(tp)
19. netif_napi_add(dev, &tp->napi, rtl8169_poll)
20. Configure hw_features / features
21. rtl_set_irq_mask(tp)       — sets tp->irq_mask
22. register_netdev(dev)
23. r8169_mdio_register(tp)
```

### IRQ Mask Setup
```c
static void rtl_set_irq_mask(struct rtl8169_private *tp)
{
    tp->irq_mask = RxOK | RxErr | TxOK | TxErr | LinkChg;
    if (tp->mac_version <= RTL_GIGA_MAC_VER_06)
        tp->irq_mask |= SYSErr | RxFIFOOver;
}
```

Interrupt status bits:
```c
SYSErr         = 0x8000
PCSTimeout     = 0x4000
SWInt          = 0x0100
TxDescUnavail  = 0x0080
RxFIFOOver     = 0x0040
LinkChg        = 0x0020
RxOverflow     = 0x0010
TxErr          = 0x0008
TxOK           = 0x0004
RxErr          = 0x0002
RxOK           = 0x0001
```

---

## 7. Hardware Reset and Init Sequence

### Reset
```c
static void rtl_hw_reset(struct rtl8169_private *tp)
{
    RTL_W8(tp, ChipCmd, CmdReset);
    rtl_loop_wait_low(tp, &rtl_chipcmd_cond, 100, 100);
    // Waits for CmdReset bit to clear (up to 10ms)
}
```

### `rtl_hw_start` — Main hardware start (called from rtl_open / rtl_reset_work)

```c
static void rtl_hw_start(struct rtl8169_private *tp)
{
    rtl_unlock_config_regs(tp);          // Cfg9346 = 0xC0
    rtl_hw_aspm_clkreq_enable(tp, false); // Disable ASPM before ephy access
    RTL_W16(tp, CPlusCmd, tp->cp_cmd);

    rtl_set_eee_txidle_timer(tp);

    // Dispatch to chip-family-specific init:
    if (tp->mac_version <= RTL_GIGA_MAC_VER_06)
        rtl_hw_start_8169(tp);
    else if (rtl_is_8125(tp))
        rtl_hw_start_8125(tp);
    else
        rtl_hw_start_8168(tp);           // ← RTL8168/8111 path

    rtl_enable_exit_l1(tp);
    rtl_hw_aspm_clkreq_enable(tp, true);

    rtl_set_rx_max_size(tp);             // RxMaxSize = R8169_RX_BUF_SIZE + 1
    rtl_set_rx_tx_desc_registers(tp);    // Write ring base addresses
    rtl_lock_config_regs(tp);            // Cfg9346 = 0x00

    rtl_jumbo_config(tp);
    rtl_pci_commit(tp);                  // Flush PCI writes

    RTL_W8(tp, ChipCmd, CmdTxEnb | CmdRxEnb);  // Enable TX+RX
    rtl_init_rxcfg(tp);
    rtl_set_tx_config_registers(tp);
    rtl_set_rx_config_features(tp, tp->dev->features);
    rtl_set_rx_mode(tp->dev);
    rtl_irq_enable(tp);
}
```

### TX Config
```c
static void rtl_set_tx_config_registers(struct rtl8169_private *tp)
{
    u32 val = TX_DMA_BURST << TxDMAShift |      // 7 << 8
              InterFrameGap << TxInterFrameGapShift;  // 3 << 24
    if (rtl_is_8168evl_up(tp))
        val |= TXCFG_AUTO_FIFO;                 // BIT(7)
    RTL_W32(tp, TxConfig, val);
}
```

### Descriptor Ring Setup
```c
static void rtl_set_rx_tx_desc_registers(struct rtl8169_private *tp)
{
    RTL_W32(tp, TxDescStartAddrHigh, ((u64)tp->TxPhyAddr) >> 32);
    RTL_W32(tp, TxDescStartAddrLow,  ((u64)tp->TxPhyAddr) & 0xFFFFFFFF);
    RTL_W32(tp, RxDescAddrHigh, ((u64)tp->RxPhyAddr) >> 32);
    RTL_W32(tp, RxDescAddrLow,  ((u64)tp->RxPhyAddr) & 0xFFFFFFFF);
}
```

### Config Register Lock/Unlock
```c
Cfg9346_Lock   = 0x00
Cfg9346_Unlock = 0xC0

static void rtl_lock_config_regs(struct rtl8169_private *tp)   { RTL_W8(tp, Cfg9346, 0x00); }
static void rtl_unlock_config_regs(struct rtl8169_private *tp) { RTL_W8(tp, Cfg9346, 0xC0); }
```

---

## 8. RTL8168 HW Start (`rtl_hw_start_8168`)

```c
static void rtl_hw_start_8168(struct rtl8169_private *tp)
{
    if (rtl_is_8168evl_up(tp))   // VER_34..VER_52 (excl VER_39)
        RTL_W8(tp, MaxTxPacketSize, EarlySize);   // 0x27
    else
        RTL_W8(tp, MaxTxPacketSize, TxPacketMax);  // 8064>>7 = 63

    rtl_hw_config(tp);       // Dispatch to variant-specific function

    RTL_W16(tp, IntrMitigate, 0x0000);  // Disable interrupt coalescing
}
```

### HW Config Dispatch Table (8168 variants)
```c
[VER_17] = rtl_hw_start_8168b        // 8168b: just disable Beacon
[VER_18] = rtl_hw_start_8168cp_1     // 8168cp
[VER_19] = rtl_hw_start_8168c_1      // 8168c
[VER_20] = rtl_hw_start_8168c_2
[VER_21] = rtl_hw_start_8168c_2
[VER_22] = rtl_hw_start_8168c_4
[VER_23] = rtl_hw_start_8168cp_2
[VER_24] = rtl_hw_start_8168cp_3
[VER_25] = rtl_hw_start_8168d        // 8168d
[VER_26] = rtl_hw_start_8168d
[VER_28] = rtl_hw_start_8168d_4      // 8168dp
[VER_31] = rtl_hw_start_8168d
[VER_32] = rtl_hw_start_8168e_1      // 8168e
[VER_33] = rtl_hw_start_8168e_1
[VER_34] = rtl_hw_start_8168e_2      // 8168evl
[VER_35] = rtl_hw_start_8168f_1      // 8168f
[VER_36] = rtl_hw_start_8168f_1
[VER_38] = rtl_hw_start_8411         // 8411
[VER_40] = rtl_hw_start_8168g_1      // 8168g
[VER_42] = rtl_hw_start_8168g_2
[VER_43] = rtl_hw_start_8168g_2
[VER_44] = rtl_hw_start_8411_2       // 8411b
[VER_46] = rtl_hw_start_8168h_1      // 8168h
[VER_48] = rtl_hw_start_8168h_1
[VER_51] = rtl_hw_start_8168ep_3     // 8168ep
[VER_52] = rtl_hw_start_8117         // 8117
```

### Common 8168g+ init pattern:
```c
rtl_set_fifo_size(tp, 0x08, 0x10, 0x02, 0x06);
rtl8168g_set_pause_thresholds(tp, 0x38, 0x48);
rtl_set_def_aspm_entry_latency(tp);    // L0=7us, L1=16us
rtl_reset_packet_filter(tp);
rtl_disable_rxdvgate(tp);
rtl_eri_write(tp, 0xc0, ERIAR_MASK_0011, 0x0000);
rtl_eri_write(tp, 0xb8, ERIAR_MASK_0011, 0x0000);
rtl_eri_clear_bits(tp, 0x1b0, BIT(12));
rtl_pcie_state_l2l3_disable(tp);
```

---

## 9. PHY Access Methods

### Direct MDIO (RTL8169 / older chips)
```c
static void r8169_mdio_write(struct rtl8169_private *tp, int reg, int value)
{
    RTL_W32(tp, PHYAR, 0x80000000 | (reg & 0x1f) << 16 | (value & 0xffff));
    rtl_loop_wait_low(tp, &rtl_phyar_cond, 25, 20);
    udelay(20);  // Required post-write delay
}

static int r8169_mdio_read(struct rtl8169_private *tp, int reg)
{
    RTL_W32(tp, PHYAR, 0x0 | (reg & 0x1f) << 16);
    // Wait for PHYAR flag to set, then:
    return RTL_R32(tp, PHYAR) & 0xffff;
}
```

### OCP-based MDIO (RTL8168g and later)
```c
static void r8168g_mdio_write(struct rtl8169_private *tp, int reg, int value)
{
    if (reg == 0x1f) {
        tp->ocp_base = value ? value << 4 : OCP_STD_PHY_BASE;
        return;
    }
    r8168_phy_ocp_write(tp, tp->ocp_base + reg * 2, value);
}

static int r8168g_mdio_read(struct rtl8169_private *tp, int reg)
{
    if (reg == 0x1f)
        return tp->ocp_base == OCP_STD_PHY_BASE ? 0 : tp->ocp_base >> 4;
    if (tp->ocp_base != OCP_STD_PHY_BASE)
        reg -= 0x10;   // for indirect paging
    return r8168_phy_ocp_read(tp, tp->ocp_base + reg * 2);
}

// Low-level OCP PHY read/write:
static void r8168_phy_ocp_write(struct rtl8169_private *tp, u32 reg, u32 data)
{
    RTL_W32(tp, GPHY_OCP, OCPAR_FLAG | (reg << 15) | data);
    rtl_loop_wait_low(tp, &rtl_phyar_cond, 25, 20);
}

static int r8168_phy_ocp_read(struct rtl8169_private *tp, u32 reg)
{
    RTL_W32(tp, GPHY_OCP, (reg << 15));
    // Wait for OCPAR_FLAG, then:
    return RTL_R32(tp, GPHY_OCP) & 0xffff;
}
```

### MDIO Dispatch (rtl_writephy / rtl_readphy)
```c
// Based on mac_version:
// VER_28, VER_31: r8168dp_2_mdio_{read,write}
// VER_40+: r8168g_mdio_{read,write}  (OCP-based)
// Others: r8169_mdio_{read,write}    (direct PHYAR)
```

### Extended Register Interface (ERI) — 8168 only
```c
static void rtl_eri_write(struct rtl8169_private *tp, int addr, u32 mask, u32 val)
{
    u32 cmd = ERIAR_WRITE_CMD | ERIAR_EXGMAC | mask | addr;
    RTL_W32(tp, ERIDR, val);
    RTL_W32(tp, ERIAR, cmd);
    rtl_loop_wait_low(tp, &rtl_eriar_cond, 100, 100);
}

static u32 rtl_eri_read(struct rtl8169_private *tp, int addr)
{
    u32 cmd = ERIAR_READ_CMD | ERIAR_EXGMAC | ERIAR_MASK_1111 | addr;
    RTL_W32(tp, ERIAR, cmd);
    // Wait for ERIAR_FLAG, then:
    return RTL_R32(tp, ERIDR);
}
```

### Embedded PHY (EPHY) Registers
```c
static void rtl_ephy_write(struct rtl8169_private *tp, int reg_addr, int value)
{
    RTL_W32(tp, EPHYAR, EPHYAR_WRITE_CMD | (value & 0xffff) |
            (reg_addr & 0x1f) << 16);
    rtl_loop_wait_low(tp, &rtl_ephyar_cond, 10, 100);
}

static u16 rtl_ephy_read(struct rtl8169_private *tp, int reg_addr)
{
    RTL_W32(tp, EPHYAR, (reg_addr & 0x1f) << 16);
    return rtl_loop_wait_high(tp, &rtl_ephyar_cond, 10, 100) ?
           RTL_R32(tp, EPHYAR) & 0xffff : ~0;
}
```

---

## 10. Ring Setup

### Ring Init
```c
static void rtl8169_init_ring_indexes(struct rtl8169_private *tp)
{
    tp->dirty_tx = tp->cur_tx = tp->cur_rx = 0;
}

static int rtl8169_init_ring(struct rtl8169_private *tp)
{
    rtl8169_init_ring_indexes(tp);
    memset(tp->tx_skb, 0, sizeof(tp->tx_skb));
    memset(tp->Rx_databuff, 0, sizeof(tp->Rx_databuff));
    return rtl8169_rx_fill(tp);
}
```

### RX Ring Fill
```c
static int rtl8169_rx_fill(struct rtl8169_private *tp)
{
    for (i = 0; i < NUM_RX_DESC; i++) {
        data = rtl8169_alloc_rx_data(tp, tp->RxDescArray + i);
        if (!data) { rtl8169_rx_clear(tp); return -ENOMEM; }
        tp->Rx_databuff[i] = data;
    }
    // Mark last descriptor with RingEnd
    tp->RxDescArray[NUM_RX_DESC - 1].opts1 |= cpu_to_le32(RingEnd);
    return 0;
}
```

### RX Buffer Allocation
```c
static struct page *rtl8169_alloc_rx_data(struct rtl8169_private *tp, struct RxDesc *desc)
{
    data = alloc_pages_node(node, GFP_KERNEL, get_order(R8169_RX_BUF_SIZE));
    mapping = dma_map_page(d, data, 0, R8169_RX_BUF_SIZE, DMA_FROM_DEVICE);
    desc->addr = cpu_to_le64(mapping);
    rtl8169_mark_to_asic(desc);  // Sets DescOwn | eor | R8169_RX_BUF_SIZE
    return data;
}

static void rtl8169_mark_to_asic(struct RxDesc *desc)
{
    u32 eor = le32_to_cpu(desc->opts1) & RingEnd;
    desc->opts2 = 0;
    dma_wmb();
    WRITE_ONCE(desc->opts1, cpu_to_le32(DescOwn | eor | R8169_RX_BUF_SIZE));
}
```

### Opening the Device (`rtl_open`)
```c
1. dma_alloc_coherent → TxDescArray (R8169_TX_RING_BYTES, 4096 bytes)
2. dma_alloc_coherent → RxDescArray (R8169_RX_RING_BYTES, 4096 bytes)
3. rtl8169_init_ring(tp)    // zero indexes, fill RX ring
4. request_irq(tp->irq, rtl8169_interrupt, ...)
5. r8169_phy_connect(tp)
6. rtl8169_up(tp):
    a. pci_set_master(tp->pci_dev)
    b. phy_init_hw / phy_resume
    c. rtl8169_init_phy(tp)
    d. napi_enable(&tp->napi)
    e. rtl_reset_work(tp):
        - netif_stop_queue
        - rtl8169_cleanup (wait for TX/RX to drain)
        - Mark all RX descriptors to ASIC (DescOwn)
        - napi_enable
        - rtl_hw_start(tp)  ← full hardware init
    f. phy_start(tp->phydev)
7. netif_start_queue(dev)
```

---

## 11. Transmit Path

### TX Doorbell
```c
static void rtl8169_doorbell(struct rtl8169_private *tp)
{
    if (rtl_is_8125(tp))
        RTL_W16(tp, TxPoll_8125, BIT(0));
    else
        RTL_W8(tp, TxPoll, NPQ);      // NPQ = 0x40
}
```

### Start Xmit (`rtl8169_start_xmit`)
```c
static netdev_tx_t rtl8169_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    entry = tp->cur_tx % NUM_TX_DESC;

    // Check if ring has space
    if (!rtl_tx_slots_avail(tp)) { netif_stop_queue; return NETDEV_TX_BUSY; }

    // Setup checksum/TSO offload flags
    opts[1] = rtl8169_tx_vlan_tag(skb);  // VLAN tag in opts2
    opts[0] = 0;
    if (!rtl_chip_supports_csum_v2(tp))
        rtl8169_tso_csum_v1(skb, opts);
    else
        rtl8169_tso_csum_v2(tp, skb, opts);

    // Map head data → first descriptor
    rtl8169_tx_map(tp, opts, skb_headlen(skb), skb->data, entry, false);
    txd_first = tp->TxDescArray + entry;

    // Map scatter-gather fragments
    if (frags)
        rtl8169_xmit_frags(tp, skb, opts, entry);

    // Mark last fragment
    txd_last->opts1 |= cpu_to_le32(LastFrag);
    tp->tx_skb[entry].skb = skb;

    dma_wmb();

    // Set DescOwn + FirstFrag on first descriptor (NIC takes ownership)
    txd_first->opts1 |= cpu_to_le32(DescOwn | FirstFrag);

    smp_wmb();
    WRITE_ONCE(tp->cur_tx, tp->cur_tx + frags + 1);

    // Ring doorbell if needed
    if (door_bell || stop_queue)
        rtl8169_doorbell(tp);

    return NETDEV_TX_OK;
}
```

### TX Map (per-descriptor)
```c
static int rtl8169_tx_map(struct rtl8169_private *tp, const u32 *opts,
                          u32 len, void *addr, unsigned int entry, bool desc_own)
{
    struct TxDesc *txd = tp->TxDescArray + entry;
    mapping = dma_map_single(d, addr, len, DMA_TO_DEVICE);

    txd->addr = cpu_to_le64(mapping);
    txd->opts2 = cpu_to_le32(opts[1]);

    opts1 = opts[0] | len;
    if (entry == NUM_TX_DESC - 1)
        opts1 |= RingEnd;
    if (desc_own)
        opts1 |= DescOwn;
    txd->opts1 = cpu_to_le32(opts1);

    tp->tx_skb[entry].len = len;
    return 0;
}
```

---

## 12. Receive Path

### RX Processing (`rtl_rx`)
```c
static int rtl_rx(struct net_device *dev, struct rtl8169_private *tp, int budget)
{
    for (count = 0; count < budget; count++, tp->cur_rx++) {
        entry = tp->cur_rx % NUM_RX_DESC;
        desc = tp->RxDescArray + entry;

        status = le32_to_cpu(READ_ONCE(desc->opts1));
        if (status & DescOwn)
            break;           // NIC still owns this one

        dma_rmb();           // Ensure we read descriptor data after DescOwn check

        // Check for errors
        if (status & RxRES) {
            // Handle rx errors (RxRWT, RxRUNT, RxCRC)
            goto release_descriptor;
        }

        pkt_size = status & GENMASK(13, 0);   // Low 14 bits = length
        if (!(dev->features & NETIF_F_RXFCS))
            pkt_size -= ETH_FCS_LEN;          // Strip FCS (4 bytes)

        // Drop fragmented frames (FirstFrag + LastFrag must both be set)
        if (rtl8169_fragmented_frame(status))
            goto release_descriptor;

        skb = napi_alloc_skb(&tp->napi, pkt_size);

        addr = le64_to_cpu(desc->addr);
        rx_buf = page_address(tp->Rx_databuff[entry]);

        dma_sync_single_for_cpu(d, addr, pkt_size, DMA_FROM_DEVICE);
        skb_copy_to_linear_data(skb, rx_buf, pkt_size);
        skb->tail += pkt_size;
        skb->len = pkt_size;
        dma_sync_single_for_device(d, addr, pkt_size, DMA_FROM_DEVICE);

        rtl8169_rx_csum(skb, status);          // Set checksum status
        skb->protocol = eth_type_trans(skb, dev);
        rtl8169_rx_vlan_tag(desc, skb);        // Extract VLAN tag from opts2

        napi_gro_receive(&tp->napi, skb);

    release_descriptor:
        rtl8169_mark_to_asic(desc);            // Return descriptor to NIC
    }
    return count;
}
```

---

## 13. Interrupt Handler

```c
static irqreturn_t rtl8169_interrupt(int irq, void *dev_instance)
{
    struct rtl8169_private *tp = dev_instance;
    u32 status = rtl_get_events(tp);

    // Check for spurious / not-our interrupt
    if ((status & 0xffff) == 0xffff || !(status & tp->irq_mask))
        return IRQ_NONE;

    // System error (old chips only)
    if (unlikely(status & SYSErr && tp->mac_version <= RTL_GIGA_MAC_VER_06)) {
        rtl8169_pcierr_interrupt(tp->dev);
        goto out;
    }

    // Link change notification
    if (status & LinkChg)
        phy_mac_interrupt(tp->phydev);

    // Disable interrupts and schedule NAPI
    rtl_irq_disable(tp);
    napi_schedule(&tp->napi);

out:
    rtl_ack_events(tp, status);
    return IRQ_HANDLED;
}
```

### Interrupt Control
```c
static u32 rtl_get_events(struct rtl8169_private *tp)
{
    if (rtl_is_8125(tp))
        return RTL_R32(tp, IntrStatus_8125);  // 0x3c (32-bit)
    else
        return RTL_R16(tp, IntrStatus);        // 0x3e (16-bit)
}

static void rtl_ack_events(struct rtl8169_private *tp, u32 bits)
{
    if (rtl_is_8125(tp))
        RTL_W32(tp, IntrStatus_8125, bits);
    else
        RTL_W16(tp, IntrStatus, bits);   // Write-1-to-clear
}

static void rtl_irq_disable(struct rtl8169_private *tp)
{
    if (rtl_is_8125(tp))
        RTL_W32(tp, IntrMask_8125, 0);
    else
        RTL_W16(tp, IntrMask, 0);
}

static void rtl_irq_enable(struct rtl8169_private *tp)
{
    if (rtl_is_8125(tp))
        RTL_W32(tp, IntrMask_8125, tp->irq_mask);
    else
        RTL_W16(tp, IntrMask, tp->irq_mask);
}
```

---

## 14. NAPI Poll

```c
static int rtl8169_poll(struct napi_struct *napi, int budget)
{
    struct rtl8169_private *tp = container_of(napi, struct rtl8169_private, napi);

    rtl_tx(dev, tp, budget);                 // Clean completed TX descriptors
    work_done = rtl_rx(dev, tp, budget);     // Process received packets

    if (work_done < budget && napi_complete_done(napi, work_done))
        rtl_irq_enable(tp);                 // Re-enable interrupts

    return work_done;
}
```

### TX Completion (`rtl_tx`)
```c
static void rtl_tx(struct net_device *dev, struct rtl8169_private *tp, int budget)
{
    dirty_tx = tp->dirty_tx;

    while (READ_ONCE(tp->cur_tx) != dirty_tx) {
        entry = dirty_tx % NUM_TX_DESC;
        status = le32_to_cpu(READ_ONCE(tp->TxDescArray[entry].opts1));
        if (status & DescOwn)
            break;                   // NIC still owns it

        skb = tp->tx_skb[entry].skb;
        rtl8169_unmap_tx_skb(tp, entry);  // DMA unmap + zero descriptor

        if (skb) {
            pkts_compl++;
            bytes_compl += skb->len;
            napi_consume_skb(skb, budget);
        }
        dirty_tx++;
    }

    if (tp->dirty_tx != dirty_tx) {
        WRITE_ONCE(tp->dirty_tx, dirty_tx);
        netif_subqueue_completed_wake(dev, 0, pkts_compl, bytes_compl, ...);

        // 8168 hack: re-ring doorbell for back-to-back TX
        if (READ_ONCE(tp->cur_tx) != dirty_tx && skb)
            rtl8169_doorbell(tp);
    }
}
```

---

## 15. PHY Status Register (0x6c)

```c
TBI_Enable = 0x80
TxFlowCtrl = 0x40
RxFlowCtrl = 0x20
_1000bpsF  = 0x10
_100bps    = 0x08
_10bps     = 0x04
LinkStatus = 0x02
FullDup    = 0x01
```

---

## 16. ChipCmd Register (0x37)

```c
StopReq    = 0x80
CmdReset   = 0x10
CmdRxEnb   = 0x08
CmdTxEnb   = 0x04
RxBufEmpty = 0x01
```

---

## 17. CPlusCmd Register (0xe0) — Key Bits

```c
EnableBist      = (1 << 15)
Mac_dbgo_oe     = (1 << 14)
Force_half_dup  = (1 << 12)
Force_rxflow_en = (1 << 11)
Force_txflow_en = (1 << 10)
ASF             = (1 << 8)
PktCntrDisable  = (1 << 7)
RxVlan          = (1 << 6)
RxChkSum        = (1 << 5)
PCIDAC          = (1 << 4)
PCIMulRW        = (1 << 3)
```

---

## 18. TxPoll Register (0x38)

```c
HPQ    = 0x80   /* Poll high-priority queue */
NPQ    = 0x40   /* Poll normal-priority queue */  ← used by doorbell
FSWInt = 0x01   /* Forced software interrupt */
```

---

## 19. RX Config Register (0x44) Bits

```c
RX128_INT_EN      = (1 << 15)   /* 8111c+ */
RX_MULTI_EN       = (1 << 14)   /* 8111c only */
RX_FIFO_THRESH    = (7 << 13)   /* No threshold */
RX_EARLY_OFF      = (1 << 11)   /* 8168g+ */
RX_PAUSE_SLOT_ON  = (1 << 11)   /* 8125b+ */
RX_DMA_BURST      = (7 << 8)    /* Unlimited DMA */

/* RX accept mode bits (low byte): */
AcceptErr       = 0x20
AcceptRunt      = 0x10
AcceptBroadcast = 0x08
AcceptMulticast = 0x04
AcceptMyPhys    = 0x02
AcceptAllPhys   = 0x01
```

### RX Config Init by Chip Family
```c
VER_02..VER_17:  RX_FIFO_THRESH | RX_DMA_BURST
VER_18..VER_24, VER_34..VER_36, VER_38:
                 RX128_INT_EN | RX_MULTI_EN | RX_DMA_BURST
VER_40..VER_52:  RX128_INT_EN | RX_MULTI_EN | RX_DMA_BURST | RX_EARLY_OFF
```

---

## 20. Key Helper Functions

### PCI Commit (flush)
```c
static void rtl_pci_commit(struct rtl8169_private *tp)
{
    RTL_R8(tp, ChipCmd);   // Read to flush pending PCI writes
}
```

### Chip Classification
```c
static bool rtl_is_8125(struct rtl8169_private *tp)
{
    return tp->mac_version >= RTL_GIGA_MAC_VER_61;
}

static bool rtl_is_8168evl_up(struct rtl8169_private *tp)
{
    return tp->mac_version >= RTL_GIGA_MAC_VER_34 &&
           tp->mac_version != RTL_GIGA_MAC_VER_39 &&
           tp->mac_version <= RTL_GIGA_MAC_VER_52;
}
```

---

## 21. Summary: Initialization Order for Stellux Port

For an RTL8168/8111 (PCI ID `10ec:8168`), the essential sequence is:

1. **PCI Enable** — enable device, set bus master, map BAR0 as MMIO
2. **Chip Detect** — read `TxConfig` bits [31:20], mask with `0xfcf`, match table
3. **Reset** — write `CmdReset` to `ChipCmd` (0x37), wait for bit to clear
4. **Allocate IRQ** — MSI for VER_18+, legacy INTX for older
5. **Read MAC** — bytes at offset 0x00..0x05
6. **Allocate Rings** — 4KB coherent DMA for each of TxDescArray, RxDescArray
7. **Init Ring Indexes** — `cur_tx = cur_rx = dirty_tx = 0`
8. **Fill RX Ring** — allocate page buffers, DMA map, set `DescOwn` on all 256, set `RingEnd` on last
9. **Write Ring Bases** — `TxDescStartAddr{High,Low}`, `RxDescAddr{High,Low}`
10. **Unlock Config** — `Cfg9346 = 0xC0`
11. **Write CPlusCmd** — `RxVlan | RxChkSum`
12. **Chip-specific Init** — per dispatch table (FIFO sizes, ERI registers, ephy init)
13. **Set RxMaxSize** — `R8169_RX_BUF_SIZE + 1`
14. **Lock Config** — `Cfg9346 = 0x00`
15. **Enable TX+RX** — `ChipCmd = CmdTxEnb | CmdRxEnb`
16. **Set TxConfig** — `TX_DMA_BURST<<8 | InterFrameGap<<24 [| TXCFG_AUTO_FIFO]`
17. **Set RxConfig** — per chip family (DMA burst, FIFO threshold, accept flags)
18. **Enable Interrupts** — write `irq_mask` to `IntrMask` (0x3c)
19. **Start PHY**
20. **Start TX Queue**

The main runtime loop is:
- **Interrupt** → ack, disable IRQs, schedule NAPI
- **NAPI poll** → `rtl_tx()` (clean completed TX), `rtl_rx()` (receive up to budget), re-enable IRQs if done
- **Transmit** → fill descriptor, set `DescOwn|FirstFrag`, `smp_wmb`, write `TxPoll` doorbell
