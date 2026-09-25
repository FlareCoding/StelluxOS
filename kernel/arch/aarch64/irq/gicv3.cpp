#include "irq/gic.h"
#include "irq/irq.h"
#include "acpi/madt_arch.h"
#include "hw/cpu.h"
#include "hw/gic_cpuif.h"
#include "hw/mmio.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "percpu/percpu.h"
#include "common/logging.h"

namespace irq {

// GICD_CTLR as seen from non-secure EL1. With a single security state the two
// low bits enable groups 0 and 1 instead, and setting both is harmless.
constexpr uint32_t GICD_CTLR_ENABLE_G1  = 1u << 0;
constexpr uint32_t GICD_CTLR_ENABLE_G1A = 1u << 1;
constexpr uint32_t GICD_CTLR_ARE_NS     = 1u << 4;
constexpr uint32_t GICD_CTLR_RWP        = 1u << 31;
constexpr uint32_t GICD_TYPER_RSS       = 1u << 26; // SGI targets may use Aff0 16 to 255

// Each CPU's redistributor is an RD_base frame followed by an SGI_base frame,
// which lays out SGI and PPI state the way the distributor lays out SPIs
constexpr uint32_t GICR_CTLR                  = 0x0000;
constexpr uint32_t GICR_TYPER                 = 0x0008;
constexpr uint32_t GICR_WAKER                 = 0x0014;
constexpr uint32_t GICR_CTLR_RWP              = 1u << 3;
constexpr uint32_t GICR_WAKER_PROCESSOR_SLEEP = 1u << 1;
constexpr uint32_t GICR_WAKER_CHILDREN_ASLEEP = 1u << 2;
constexpr uint64_t GICR_TYPER_VLPIS           = 1ULL << 1;
constexpr uint64_t GICR_TYPER_LAST            = 1ULL << 4;
constexpr uint32_t GICR_TYPER_AFFINITY_SHIFT  = 32;
constexpr uint32_t GICR_TYPER_AFF_BITS        = 8; // Width of each packed affinity level
constexpr size_t   GICR_SGI_FRAME             = 0x10000;
constexpr size_t   GICR_STRIDE                = 0x20000;
constexpr size_t   GICR_STRIDE_VLPI           = 0x40000; // GICv4 adds two frames for virtual LPIs

// GICR_TYPER and GICD_IROUTER are 64-bit registers, accessed whole since
// Apple's hypervisor GIC reads a 32-bit half as zero
constexpr uint32_t GICD_IROUTER       = 0x6000;
constexpr uint32_t GICD_IROUTER_BYTES = 8;

// ICC_SGI1R_EL1 names targets by Aff3.Aff2.Aff1 and a list of 16 Aff0 values,
// with the range selector choosing which block of 16 the list covers
constexpr uint32_t SGI1R_AFF1_SHIFT     = 16;
constexpr uint32_t SGI1R_INTID_SHIFT    = 24;
constexpr uint32_t SGI1R_AFF2_SHIFT     = 32;
constexpr uint32_t SGI1R_RS_SHIFT       = 44;
constexpr uint32_t SGI1R_AFF3_SHIFT     = 48;
constexpr uint32_t SGI1R_INTID_MASK     = 0xF;
constexpr uint32_t SGI1R_TARGETS_PER_RS = 16;

constexpr size_t   GICD_FRAME_SIZE      = 0x10000;
constexpr uint32_t GIC_ALL_INTIDS       = 0xFFFFFFFF; // Every bit of a one bit per interrupt register
constexpr uint32_t GICD_ICFGR_ALL_LEVEL = 0;
constexpr uint32_t PRIORITY_WORD        = 0xA0A0A0A0; // Mid priority in each byte of a GICD_IPRIORITYR
constexpr uint32_t POLL_LIMIT           = 1000000;

__PRIVILEGED_BSS static uintptr_t g_gicd_va;
__PRIVILEGED_BSS static uintptr_t g_gicd_base_kva;
__PRIVILEGED_BSS static uintptr_t g_gicr_va;
__PRIVILEGED_BSS static uintptr_t g_gicr_base_kva;
__PRIVILEGED_BSS static size_t    g_gicr_len;
__PRIVILEGED_BSS static uint32_t  g_intid_end;          // One past the highest implemented interrupt ID
__PRIVILEGED_BSS static bool      g_range_selector;     // SGIs can reach CPUs with Aff0 above 15
__PRIVILEGED_BSS static uint64_t  g_affinity[MAX_CPUS]; // MPIDR affinity fields of each CPU
__PRIVILEGED_BSS static uintptr_t g_rd_va[MAX_CPUS];    // RD_base frame of each CPU, 0 until it initializes

// The affinity GICR_TYPER reports for a CPU, packed as Aff3.Aff2.Aff1.Aff0
static uint32_t packed_affinity(uint64_t mpidr) {
    uint32_t packed = 0;
    for (uint32_t level = 0; level < cpu::MPIDR_AFF_LEVELS; level++) {
        packed |= static_cast<uint32_t>(cpu::mpidr_affinity(mpidr, level)) << (level * GICR_TYPER_AFF_BITS);
    }

    return packed;
}

/**
 * Waits for a register write pending bit to clear, so a disable or a control
 * change has taken effect before the caller relies on it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void wait_rwp(uintptr_t ctlr, uint32_t rwp_bit) {
    for (uint32_t i = 0; i < POLL_LIMIT; i++) {
        if ((mmio::read32(ctlr) & rwp_bit) == 0) {
            return;
        }
    }

    log::warn("irq: GICv3 register write still pending after %u polls", POLL_LIMIT);
}

/**
 * @brief Whether logical CPU `cpu_id` has initialized and can be targeted.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool can_target(uint32_t cpu_id) {
    return cpu_id < MAX_CPUS && g_rd_va[cpu_id] != 0;
}

/**
 * @brief The registers holding `intid`: the calling CPU's SGI frame for SGIs
 * and PPIs, the distributor for SPIs.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uintptr_t frame_of(uint32_t intid) {
    if (intid < GIC_SPI_BASE) {
        return g_rd_va[percpu::current_cpu_id()] + GICR_SGI_FRAME;
    }

    return g_gicd_va;
}

/**
 * @brief Find the RD_base frame whose affinity matches `mpidr`, or 0.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uintptr_t find_redistributor(uint64_t mpidr) {
    uint32_t affinity = packed_affinity(mpidr);
    size_t offset = 0;
    while (offset + GICR_STRIDE <= g_gicr_len) {
        uintptr_t rd = g_gicr_va + offset;
        uint64_t typer = mmio::read64(rd + GICR_TYPER);
        if (static_cast<uint32_t>(typer >> GICR_TYPER_AFFINITY_SHIFT) == affinity) {
            return rd;
        }

        if (typer & GICR_TYPER_LAST) {
            break;
        }

        offset += (typer & GICR_TYPER_VLPIS) ? GICR_STRIDE_VLPI : GICR_STRIDE;
    }

    return 0;
}

/**
 * @brief Wake the redistributor at `rd` so it forwards interrupts to its CPU.
 * @return true once the redistributor reports itself awake.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool wake_redistributor(uintptr_t rd) {
    mmio::write32(rd + GICR_WAKER, mmio::read32(rd + GICR_WAKER) & ~GICR_WAKER_PROCESSOR_SLEEP);
    for (uint32_t i = 0; i < POLL_LIMIT; i++) {
        if ((mmio::read32(rd + GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP) == 0) {
            return true;
        }
    }

    return false;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void write_route(uint32_t intid, uint64_t affinity) {
    mmio::write64(g_gicd_va + GICD_IROUTER + intid * GICD_IROUTER_BYTES, affinity);
}

/**
 * Every SPI starts disabled, idle, group 1, level triggered, and mid priority,
 * whatever state firmware left it in.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void reset_spis() {
    for (uint32_t intid = GIC_SPI_BASE; intid < g_intid_end; intid += GIC_INTIDS_PER_REG) {
        uint32_t offset = bit_array_offset(intid);
        mmio::write32(g_gicd_va + GICD_ICENABLER + offset, GIC_ALL_INTIDS);
        mmio::write32(g_gicd_va + GICD_ICPENDR + offset, GIC_ALL_INTIDS);
        mmio::write32(g_gicd_va + GICD_ICACTIVER + offset, GIC_ALL_INTIDS);
        mmio::write32(g_gicd_va + GICD_IGROUPR + offset, GIC_ALL_INTIDS);
    }
    wait_rwp(g_gicd_va + GICD_CTLR, GICD_CTLR_RWP);

    for (uint32_t intid = GIC_SPI_BASE; intid < g_intid_end; intid += GIC_INTIDS_PER_ICFGR) {
        mmio::write32(g_gicd_va + GICD_ICFGR + icfgr_offset(intid), GICD_ICFGR_ALL_LEVEL);
    }

    for (uint32_t intid = GIC_SPI_BASE; intid < g_intid_end; intid += GIC_INTIDS_PER_IPRIORITYR) {
        mmio::write32(g_gicd_va + GICD_IPRIORITYR + intid, PRIORITY_WORD);
    }
}

/**
 * Every SGI and PPI of the CPU owning `rd` starts disabled, idle, group 1, and
 * mid priority. Each private interrupt is enabled later by whoever owns it.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void reset_private_interrupts(uintptr_t rd) {
    uintptr_t sgi = rd + GICR_SGI_FRAME;
    mmio::write32(sgi + GICD_ICENABLER, GIC_ALL_INTIDS);
    mmio::write32(sgi + GICD_ICPENDR, GIC_ALL_INTIDS);
    mmio::write32(sgi + GICD_ICACTIVER, GIC_ALL_INTIDS);
    wait_rwp(rd + GICR_CTLR, GICR_CTLR_RWP);

    mmio::write32(sgi + GICD_IGROUPR, GIC_ALL_INTIDS);
    for (uint32_t intid = 0; intid < GIC_SPI_BASE; intid += GIC_INTIDS_PER_IPRIORITYR) {
        mmio::write32(sgi + GICD_IPRIORITYR + intid, PRIORITY_WORD);
    }
}

/**
 * Turns on the calling CPU's system register interface and lets every
 * priority through. Fails if firmware locked the interface off.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static bool enable_cpu_interface() {
    gic_cpuif::write_sre(gic_cpuif::read_sre() | gic_cpuif::SRE_ENABLE |
                         gic_cpuif::SRE_DISABLE_FIQ_BYPASS | gic_cpuif::SRE_DISABLE_IRQ_BYPASS);
    if ((gic_cpuif::read_sre() & gic_cpuif::SRE_ENABLE) == 0) {
        return false;
    }

    gic_cpuif::write_pmr(gic_cpuif::PMR_ALLOW_ALL);
    gic_cpuif::write_bpr1(gic_cpuif::BPR_MOST_PREEMPTION);
    gic_cpuif::write_ctlr(gic_cpuif::read_ctlr() & ~gic_cpuif::CTLR_EOI_MODE);
    gic_cpuif::write_igrpen1(gic_cpuif::IGRPEN_ENABLE);
    return true;
}

/**
 * Wakes the calling CPU's redistributor, resets its private interrupts, and
 * enables its CPU interface, then records how the GIC addresses the CPU.
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t init_this_cpu() {
    uint32_t cpu_id = percpu::current_cpu_id();
    uint64_t mpidr = cpu::read_mpidr();
    uintptr_t rd = find_redistributor(mpidr);
    if (rd == 0 || !wake_redistributor(rd)) {
        log::error("irq: GICv3 has no awake redistributor for CPU %u (MPIDR 0x%lx)", cpu_id, mpidr);
        return ERR_NO_CPU_INTERFACE;
    }

    reset_private_interrupts(rd);
    if (!enable_cpu_interface()) {
        log::error("irq: GICv3 system register interface is locked off on CPU %u", cpu_id);
        return ERR_NO_CPU_INTERFACE;
    }

    // MPIDR bit 31 is RES1 where GICD_IROUTER keeps its routing mode, so only
    // the affinity fields are recorded
    g_affinity[cpu_id] = mpidr & cpu::MPIDR_AFFINITY_MASK;
    g_rd_va[cpu_id] = rd;
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t gicv3_init(const acpi::madt_info& madt) {
    if (madt.gicr_base == 0 || madt.gicr_length == 0) {
        log::error("irq: GICv3 reported without a redistributor region");
        return ERR_NO_MADT;
    }

    int32_t rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(madt.gicd_base),
        GICD_FRAME_SIZE,
        paging::PAGE_KERNEL_RW,
        g_gicd_base_kva,
        g_gicd_va);
    if (rc != vmm::OK) {
        log::error("irq: failed to map GICD at 0x%lx", madt.gicd_base);
        return ERR_MAP;
    }

    rc = vmm::map_device(
        static_cast<pmm::phys_addr_t>(madt.gicr_base),
        madt.gicr_length,
        paging::PAGE_KERNEL_RW,
        g_gicr_base_kva,
        g_gicr_va);
    if (rc != vmm::OK) {
        log::error("irq: failed to map GICR region at 0x%lx", madt.gicr_base);
        return ERR_MAP;
    }

    g_gicr_len = madt.gicr_length;
    uint32_t typer = mmio::read32(g_gicd_va + GICD_TYPER);
    g_intid_end = GIC_INTIDS_PER_REG * ((typer & GICD_TYPER_LINES) + 1);
    if (g_intid_end > GIC_SPI_END) {
        g_intid_end = GIC_SPI_END;
    }

    // Affinity routing can only be switched on while both groups are disabled,
    // and they stay disabled until every SPI has a target
    mmio::write32(g_gicd_va + GICD_CTLR, 0);
    wait_rwp(g_gicd_va + GICD_CTLR, GICD_CTLR_RWP);
    reset_spis();
    mmio::write32(g_gicd_va + GICD_CTLR, GICD_CTLR_ARE_NS);
    wait_rwp(g_gicd_va + GICD_CTLR, GICD_CTLR_RWP);

    rc = init_this_cpu();
    if (rc != OK) {
        return rc;
    }

    g_range_selector = (typer & GICD_TYPER_RSS) != 0 && (gic_cpuif::read_ctlr() & gic_cpuif::CTLR_RSS) != 0;

    uint64_t boot_affinity = g_affinity[percpu::current_cpu_id()];
    for (uint32_t intid = GIC_SPI_BASE; intid < g_intid_end; intid++) {
        write_route(intid, boot_affinity);
    }
    mmio::write32(g_gicd_va + GICD_CTLR, GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1A | GICD_CTLR_ENABLE_G1);
    wait_rwp(g_gicd_va + GICD_CTLR, GICD_CTLR_RWP);

    log::info("irq: GICv3 initialized (GICD=0x%lx GICR=0x%lx, %u interrupt IDs)",
              madt.gicd_base, madt.gicr_base, g_intid_end);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t gicv3_init_ap() {
    return init_this_cpu();
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static uint32_t gicv3_acknowledge() {
    return static_cast<uint32_t>(gic_cpuif::read_iar1());
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void gicv3_eoi(uint32_t ack) {
    gic_cpuif::write_eoir1(ack);
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t gicv3_send_sgi(uint32_t intid, uint32_t target_cpu) {
    if (!can_target(target_cpu)) {
        return ERR_INVAL;
    }

    uint64_t affinity = g_affinity[target_cpu];
    uint64_t aff0 = cpu::mpidr_affinity(affinity, 0);
    uint64_t aff1 = cpu::mpidr_affinity(affinity, 1);
    uint64_t aff2 = cpu::mpidr_affinity(affinity, 2);
    uint64_t aff3 = cpu::mpidr_affinity(affinity, 3);
    uint64_t range = aff0 / SGI1R_TARGETS_PER_RS;
    if (range != 0 && !g_range_selector) {
        return ERR_INVAL;
    }

    uint64_t sgi1r = (aff3 << SGI1R_AFF3_SHIFT) | (range << SGI1R_RS_SHIFT) | (aff2 << SGI1R_AFF2_SHIFT) |
                     (static_cast<uint64_t>(intid & SGI1R_INTID_MASK) << SGI1R_INTID_SHIFT) |
                     (aff1 << SGI1R_AFF1_SHIFT) | (1ULL << (aff0 % SGI1R_TARGETS_PER_RS));
    gic_cpuif::write_sgi1r(sgi1r);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static int32_t gicv3_set_spi_target(uint32_t intid, uint32_t target_cpu) {
    if (intid < GIC_SPI_BASE || intid >= g_intid_end || !can_target(target_cpu)) {
        return ERR_INVAL;
    }

    write_route(intid, g_affinity[target_cpu]);
    return OK;
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void gicv3_unmask(uint32_t intid) {
    mmio::write32(frame_of(intid) + GICD_ISENABLER + bit_array_offset(intid), bit_array_mask(intid));
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void gicv3_mask(uint32_t intid) {
    mmio::write32(frame_of(intid) + GICD_ICENABLER + bit_array_offset(intid), bit_array_mask(intid));

    // The interrupt counts as masked only once the write pending bit clears
    if (intid < GIC_SPI_BASE) {
        wait_rwp(g_rd_va[percpu::current_cpu_id()] + GICR_CTLR, GICR_CTLR_RWP);
    } else {
        wait_rwp(g_gicd_va + GICD_CTLR, GICD_CTLR_RWP);
    }
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void gicv3_set_group1(uint32_t intid) {
    uintptr_t addr = frame_of(intid) + GICD_IGROUPR + bit_array_offset(intid);
    mmio::write32(addr, mmio::read32(addr) | bit_array_mask(intid));
}

/**
 * @note Privilege: **required**
 */
__PRIVILEGED_CODE static void gicv3_set_trigger(uint32_t intid, trigger mode) {
    uintptr_t addr = frame_of(intid) + GICD_ICFGR + icfgr_offset(intid);
    uint32_t shift = icfgr_shift(intid);

    uint32_t val = mmio::read32(addr) & ~(GIC_ICFGR_FIELD_MASK << shift);
    if (mode == trigger::edge) {
        val |= GICD_ICFGR_EDGE << shift;
    }
    mmio::write32(addr, val);
}

static const gic_backend g_gicv3 = {
    .init           = gicv3_init,
    .init_ap        = gicv3_init_ap,
    .acknowledge    = gicv3_acknowledge,
    .eoi            = gicv3_eoi,
    .send_sgi       = gicv3_send_sgi,
    .set_spi_target = gicv3_set_spi_target,
    .unmask         = gicv3_unmask,
    .mask           = gicv3_mask,
    .set_group1     = gicv3_set_group1,
    .set_trigger    = gicv3_set_trigger,
};

const gic_backend& gicv3_backend() {
    return g_gicv3;
}

} // namespace irq
