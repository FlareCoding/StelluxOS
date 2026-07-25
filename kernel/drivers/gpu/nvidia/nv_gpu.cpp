#include "drivers/gpu/nvidia/nv_gpu.h"
#include "drivers/gpu/nvidia/nv_regs.h"
#include "drivers/gpu/nvidia/nv_firmware.h"
#include "drivers/gpu/nvidia/nv_vbios.h"
#include "drivers/gpu/nvidia/nv_fwsec.h"
#include "drivers/gpu/nvidia/nv_frts.h"
#include "drivers/gpu/nvidia/nv_radix3.h"
#include "drivers/gpu/nvidia/nv_bootimg.h"
#include "drivers/gpu/nvidia/nv_wprmeta.h"
#include "drivers/gpu/nvidia/nv_rpc.h"
#include "drivers/gpu/nvidia/nv_display.h"
#include "drivers/gpu/nvidia/nv_falcon.h"
#include "drivers/gpu/nvidia/nv_booter.h"
#include "pci/pci.h"
#include "mm/vmm.h"
#include "mm/paging_types.h"
#include "hw/mmio.h"
#include "hw/delay.h"
#include "clock/clock.h"
#include "common/logging.h"
#include "dynpriv/dynpriv.h"
#include "fs/node.h"
#include "fs/fs.h"
#include "fs/devfs/devfs.h"
#include "mm/uaccess.h"
#include "mm/heap.h"

namespace nvidia {

// Poll for GPU-firmware (GFW) devinit completion through BAR0, mirroring the open
// driver's _gpuIsGfwBootCompleted_TU102 (kern_gpu_tu102.c:321-370):
//   (1) the secure-scratch PRIV_LEVEL_MASK must report READ_PROTECTION_LEVEL0
//       ENABLE (FWSEC has lowered its PLM), THEN
//   (2) the GFW_BOOT PROGRESS field must read COMPLETED (0xFF).
// VBIOS devinit runs at power-on, so on a warm system this completes immediately;
// we still poll with a source-sized timeout (FWSEC start 50ms + complete 2s, padded).
static bool wait_gfw_boot(uintptr_t bar0_va) {
    constexpr uint64_t timeout_ns = 4ull * 1000 * 1000 * 1000; // 4 s
    const uint64_t start = clock::now_ns();
    bool logged = false;
    for (;;) {
        uint32_t plm = 0, gfw = 0;
        RUN_ELEVATED({
            plm = mmio::read32(bar0_va + NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK);
            gfw = mmio::read32(bar0_va + NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT);
        });
        const bool     plm_ok   = (plm & SCRATCH_05_PLM_READ_PROTECTION_LEVEL0_ENABLE) != 0;
        const uint32_t progress = gfw & GFW_BOOT_PROGRESS_MASK;

        if (!logged) {
            log::info("nvidia: GFW PRIV_LEVEL_MASK=0x%08x (FWSEC-PLM-lowered=%u) GFW_BOOT=0x%08x progress=0x%02x",
                      plm, plm_ok ? 1u : 0u, gfw, progress);
            logged = true;
        }

        if (plm_ok && progress == GFW_BOOT_PROGRESS_COMPLETED) {
            const uint64_t us = (clock::now_ns() - start) / 1000;
            log::info("nvidia: GFW devinit COMPLETED (progress=0x%02x, %lu us)",
                      progress, static_cast<unsigned long>(us));
            return true;
        }
        if (clock::now_ns() - start > timeout_ns) {
            const uint64_t ms = (clock::now_ns() - start) / 1000000;
            log::error("nvidia: GFW devinit TIMEOUT (PLM=0x%08x progress=0x%02x after %lu ms)",
                       plm, progress, static_cast<unsigned long>(ms));
            return false;
        }
        delay::us(200);
    }
}

// The pci_driver framework discovers + binds the GA102 (REGISTER_PCI_DRIVER in nv_driver.cpp) and
// passes init() the pci::device*, so there is no privileged find_gpu()/pci::device_count() scan here
// (pci::device_count reads __PRIVILEGED_DATA and would #PF from the ring-3 driver task).
static void log_bar(const char* name, const pci::bar& b) {
    log::info("nvidia:   %s phys=0x%lx size=%lu (0x%lx) type=%u prefetch=%u",
              name,
              static_cast<unsigned long>(b.phys),
              static_cast<unsigned long>(b.size),
              static_cast<unsigned long>(b.size),
              b.type, b.prefetchable ? 1u : 0u);
}

int32_t init(pci::device* dev) {
    log::info("nvidia: ============================================================");
    log::info("nvidia: RTX 3080 (GA102) bring-up - stages 1-5b: ...vbios, fwsec, ucode, FWSEC-FRTS (WPR2)");
    log::info("nvidia: ============================================================");

    pci::device* gpu = dev; // provided by the pci_driver framework (no privileged find_gpu/device_count scan)
    if (!gpu) {
        log::error("nvidia: init() called with null device - aborting");
        return ERR_NO_DEVICE;
    }

    // The pci::device object lives in privileged memory and its accessors are either __PRIVILEGED_CODE
    // (config_read/enable/BME) or read privileged member fields (bus()/get_bar()/...). This bring-up runs
    // on the ring-3 driver task, so snapshot EVERYTHING we need from the device in ONE elevated block,
    // then never touch the device object from unprivileged code again -- only these cached locals + the
    // user-mapped BAR0 below. (Run #59 #PF'd dereferencing the device object directly from ring 3.)
    uint8_t  bus = 0, slot = 0, func = 0, revision = 0, class_code = 0, subclass = 0;
    uint16_t vendor = 0, device = 0, cmd_before = 0, cmd_after = 0;
    pci::bar bar0{}, bar1{}, bar3{};
    RUN_ELEVATED({
        bus = gpu->bus(); slot = gpu->slot(); func = gpu->func();
        vendor = gpu->vendor_id(); device = gpu->device_id();
        class_code = gpu->class_code(); subclass = gpu->subclass();
        revision = gpu->config_read8(pci::CFG_REVISION);
        bar0 = gpu->get_bar(BAR_REGS);   // regs   (16MB)
        bar1 = gpu->get_bar(BAR_FB);     // vram   (256MB)
        bar3 = gpu->get_bar(BAR_INST);   // inst   (32MB)
        // Enable PCI Memory-Space decode + Bus-Master (BME): MEM is needed to touch BAR0; BME lets the
        // GSP DMA sysmem later.
        cmd_before = gpu->config_read16(pci::CFG_COMMAND);
        gpu->enable();                // sets IO + MEMORY space
        gpu->enable_bus_mastering();  // sets Bus-Master Enable
        cmd_after = gpu->config_read16(pci::CFG_COMMAND);
    });

    log::info("nvidia: target at %02x:%02x.%x  %04x:%04x rev %02x  class %02x:%02x",
              bus, slot, func, vendor, device, revision, class_code, subclass);
    log::info("nvidia: BAR layout:");
    log_bar("BAR0 (regs, 16MB) ", bar0);
    log_bar("BAR1 (vram, 256MB)", bar1);
    log_bar("BAR3 (inst, 32MB) ", bar3);
    log::info("nvidia: PCI COMMAND 0x%04x -> 0x%04x  (MEM=%u BME=%u IO=%u)",
              cmd_before, cmd_after,
              (cmd_after & pci::CMD_MEMORY_SPACE) ? 1u : 0u,
              (cmd_after & pci::CMD_BUS_MASTER) ? 1u : 0u,
              (cmd_after & pci::CMD_IO_SPACE) ? 1u : 0u);

    if (bar0.phys == 0 || bar0.size == 0) {
        log::error("nvidia: BAR0 invalid (phys=0x%lx size=%lu) - aborting",
                   static_cast<unsigned long>(bar0.phys),
                   static_cast<unsigned long>(bar0.size));
        return ERR_NO_BAR;
    }

    // ioremap BAR0 as UNCACHED device memory (PAGE_DEVICE). This is the control
    // surface every register access and every GSP boot write will use.
    uintptr_t bar0_base = 0;
    uintptr_t bar0_va = 0;
    int32_t rc = vmm::ERR_PAGING;
    RUN_ELEVATED(rc = vmm::map_device(
        bar0.phys, bar0.size,
        // PAGE_USER: the bring-up runs on the ring-3 driver task, so BAR0 MMIO (registers, PRAMIN
        // window, EVO channels) must be user-accessible (matches xhci's map_bar(.., PAGE_USER)).
        paging::PAGE_READ | paging::PAGE_WRITE | paging::PAGE_DEVICE | paging::PAGE_USER,
        bar0_base, bar0_va));
    if (rc != vmm::OK) {
        log::error("nvidia: BAR0 ioremap failed rc=%d", rc);
        return ERR_MAP;
    }
    log::info("nvidia: BAR0 mapped uncached: phys=0x%lx -> va=0x%lx (base=0x%lx, %lu bytes)",
              static_cast<unsigned long>(bar0.phys),
              static_cast<unsigned long>(bar0_va),
              static_cast<unsigned long>(bar0_base),
              static_cast<unsigned long>(bar0.size));

    // The moment of truth: read the chip-identity registers through BAR0.
    uint32_t boot0 = 0, boot1 = 0;
    RUN_ELEVATED({
        boot0 = mmio::read32(bar0_va + NV_PMC_BOOT_0);
        boot1 = mmio::read32(bar0_va + NV_PMC_BOOT_1);
    });

    log::info("nvidia: --- chip-alive readback ---");
    log::info("nvidia: NV_PMC_BOOT_0  = 0x%08x", boot0);
    log::info("nvidia: NV_PMC_BOOT_1  = 0x%08x", boot1);

    if (boot0 == GPU_LOST) {
        log::error("nvidia: NV_PMC_BOOT_0 reads 0xFFFFFFFF - GPU off the bus / BAR0 dead");
        return ERR_CHIP_LOST;
    }

    const uint32_t chipset = (boot0 & PMC_BOOT_0_CHIPSET_MASK) >> PMC_BOOT_0_CHIPSET_SHIFT;
    const uint32_t arch    = (boot0 & PMC_BOOT_0_ARCH_MASK) >> PMC_BOOT_0_ARCH_SHIFT;
    const uint32_t impl    = (boot0 & PMC_BOOT_0_IMPL_MASK) >> PMC_BOOT_0_IMPL_SHIFT;
    const uint32_t major   = (boot0 & PMC_BOOT_0_MAJOR_MASK) >> PMC_BOOT_0_MAJOR_SHIFT;
    const uint32_t minor   = (boot0 & PMC_BOOT_0_MINOR_MASK);

    log::info("nvidia: decoded chipset=0x%03x arch=0x%02x impl=0x%x revision=%x.%x",
              chipset, arch, impl, major, minor);

    if (chipset == CHIPSET_GA102) {
        log::info("nvidia: *** GA102 CONFIRMED - RTX 3080 is alive and BAR0 decodes ***");
    } else {
        log::warn("nvidia: unexpected chipset 0x%03x (expected GA102 0x%03x) - continuing",
                  chipset, CHIPSET_GA102);
    }

    // ---- Stage 2: confirm GPU-firmware (devinit) completion ----
    log::info("nvidia: --- GFW devinit gate ---");
    if (!wait_gfw_boot(bar0_va)) {
        log::warn("nvidia: ===== stage 2 INCOMPLETE (chip alive, but GFW devinit unconfirmed) =====");
        return ERR_GFW;
    }

    // ---- Stage 3: locate + validate the GSP firmware image in the initrd ----
    log::info("nvidia: --- GSP firmware probe (535.183.01) ---");
    gsp_fw_image fw;
    const int32_t fw_rc = fw_probe_gsp(fw);
    if (fw_rc != FW_OK) {
        log::warn("nvidia: ===== stage 3 INCOMPLETE (firmware probe failed rc=%d) =====", fw_rc);
        return fw_rc;
    }

    // ---- Stage 4a: extract the VBIOS image from the GPU expansion ROM ----
    log::info("nvidia: --- VBIOS extraction (NV_PROM @ BAR0+0x300000) ---");
    vbios_image vbios;
    const int32_t vb_rc = vbios_extract_from_rom(bar0_va, vbios);
    if (vb_rc != VB_OK) {
        log::warn("nvidia: ===== stage 4a INCOMPLETE (VBIOS extract failed rc=%d) =====", vb_rc);
        return vb_rc;
    }
    log::info("nvidia: VBIOS extracted OK: %u bytes", vbios.size);

    // ---- Stage 4b: parse the FWSEC ucode descriptor out of the VBIOS ----
    log::info("nvidia: --- FWSEC parse (BIT -> Falcon ucode table -> FWSEC entry) ---");
    fwsec_info fwsec;
    const int32_t fs_rc = fwsec_parse(vbios, /*use_debug=*/false, fwsec);
    if (fs_rc != FWSEC_OK) {
        vbios_free(vbios);
        log::warn("nvidia: ===== stage 4b INCOMPLETE (FWSEC parse failed rc=%d) =====", fs_rc);
        return fs_rc;
    }

    // ---- Stage 5a: assemble the FWSEC ucode into a DMA buffer (host-side) ----
    log::info("nvidia: --- FWSEC ucode assembly (DMA buffer) ---");
    fwsec_ucode ucode;
    const int32_t ld_rc = fwsec_load_ucode(vbios, fwsec, ucode);
    if (ld_rc != FWSEC_OK) {
        vbios_free(vbios);
        log::warn("nvidia: ===== stage 5a INCOMPLETE (ucode assembly failed rc=%d) =====", ld_rc);
        return ld_rc;
    }

    // ---- Stage 5b: FWSEC-FRTS -> carve WPR2  *** FIRST GPU WRITES *** ----
    log::info("nvidia: --- FWSEC-FRTS (first GPU writes: Falcon DMA-load-and-go) ---");
    const int32_t frts_rc = fwsec_frts_execute(bar0_va, boot0, vbios, fwsec, ucode);
    fwsec_free_ucode(ucode);
    vbios_free(vbios);
    if (frts_rc != FRTS_OK) {
        log::warn("nvidia: ===== stage 5b INCOMPLETE (FWSEC-FRTS failed rc=%d) =====", frts_rc);
        return frts_rc;
    }

    log::info("nvidia: ===== stages 1-5b COMPLETE (WPR2 carved) =====");

    // ---- Stage 6: build the radix3 page table over the GSP-RM image (.fwimage) ----
    // The GSP MMU gathers the ~38 MB GSP-RM image from sysmem through this 3-level
    // table during boot. Host-side only (no GPU access yet); validates the large
    // image load + table geometry before we wire it into the GSP-RM boot args.
    log::info("nvidia: --- radix3 over gsp_ga10x.bin .fwimage ---");
    gsp_radix3 radix3;
    const int32_t r3_rc = gsp_build_radix3(GSP_FW_PATH, fw, radix3);
    if (r3_rc != RADIX3_OK) {
        log::warn("nvidia: ===== stage 6 INCOMPLETE (radix3 build failed rc=%d) =====", r3_rc);
        return r3_rc;
    }
    // radix3 is retained (its root phys goes into the WprMeta below).
    log::info("nvidia: ===== stage 6 COMPLETE (radix3 built over GSP-RM image) =====");

    // ---- Stage 7a: load the GSP-RM boot ucode (SK+BL image + RM_RISCV_UCODE_DESC) ----
    // Not a firmware file: decompressed offline from the open driver bindata
    // (scripts/extract-nv-bindata.py) and staged in the initrd. The descriptor's
    // monitor/manifest offsets feed the GspFwWprMeta we build next (Stage 7b).
    log::info("nvidia: --- GSP-RM boot ucode (SK+BL) load + desc parse ---");
    gsp_boot_ucode boot;
    const int32_t boot_rc = gsp_load_boot_ucode(boot);
    if (boot_rc != BOOTIMG_OK) {
        gsp_free_radix3(radix3);
        log::warn("nvidia: ===== stage 7a INCOMPLETE (boot ucode load failed rc=%d) =====", boot_rc);
        return boot_rc;
    }
    log::info("nvidia: ===== stage 7a COMPLETE (boot ucode + desc staged) =====");

    // ---- Stage 7b: compute the GSP FB layout + populate the 256B GspFwWprMeta ----
    log::info("nvidia: --- GSP FB layout + WprMeta (kgspCalculateFbLayout_TU102) ---");
    gsp_wprmeta wprmeta;
    const int32_t wm_rc = gsp_build_wprmeta(bar0_va, radix3, boot, wprmeta);
    if (wm_rc != WPRMETA_OK) {
        gsp_free_boot_ucode(boot);
        gsp_free_radix3(radix3);
        log::warn("nvidia: ===== stage 7b INCOMPLETE (WprMeta build failed rc=%d) =====", wm_rc);
        return wm_rc;
    }

    // radix3, boot ucode, and WprMeta are retained through the GSP boot below.
    log::info("nvidia: ===== stage 7b COMPLETE (WprMeta) =====");

    // ---- Stage 8: RPC message-queue rings + GSP/libos boot args ----
    log::info("nvidia: --- RPC queues + GSP args + libos args ---");
    gsp_rpc rpc;
    const int32_t rpc_rc = gsp_rpc_init(rpc);
    if (rpc_rc != RPC_OK) {
        gsp_free_wprmeta(wprmeta);
        gsp_free_boot_ucode(boot);
        gsp_free_radix3(radix3);
        log::warn("nvidia: ===== stage 8 INCOMPLETE (rpc init failed rc=%d) =====", rpc_rc);
        return rpc_rc;
    }
    log::info("nvidia: ===== stage 8 COMPLETE (RPC plane staged) =====");

    // ---- Stage 9: boot GSP-RM (signature -> reset-into-RISC-V -> SEC2 Booter Load) ----
    log::info("nvidia: --- GSP boot: signature + reset-into-RISC-V + SEC2 Booter Load ---");

    // 9b: load the GSP-RM signature and record it in the WprMeta (Booter verifies it).
    dma_buffer sig;
    const int32_t sig_rc = gsp_load_signature(fw, sig);
    if (sig_rc != BOOTER_OK) {
        gsp_rpc_free(rpc);
        gsp_free_wprmeta(wprmeta);
        gsp_free_boot_ucode(boot);
        gsp_free_radix3(radix3);
        log::warn("nvidia: ===== stage 9 INCOMPLETE (GSP-RM signature load failed rc=%d) =====", sig_rc);
        return sig_rc;
    }
    wprmeta.meta->sysmemAddrOfSignature = sig.phys;
    wprmeta.meta->sizeOfSignature       = sig.size;
    barrier::dma_full();

    // 9c: reset the GSP Falcon into RISC-V mode + hand it the libos boot args.
    falcon gsp = { bar0_va, NV_PGSP_FALCON_BASE, NV_FALCON2_GSP_BASE, NV_PGSP_FBIF_BASE };
    falcon_reset_into_riscv(gsp);

    // 9c.1: stuff the two async init RPCs into the command ring BEFORE the Booter
    // starts the GSP RISC-V, so GSP-RM consumes them during boot and proceeds to
    // post events/INIT_DONE (kgspBootstrapRiscvOSEarly_GA102: GSP_SET_SYSTEM_INFO
    // then SET_REGISTRY, both async, before kgspExecuteBooterLoad).
    {
        // bar1/bar3/bus/slot were snapshotted from the (privileged) device object up top.
        const uint64_t dbdf = (static_cast<uint64_t>(bus) << 8) |
                              static_cast<uint64_t>(slot);
        const int32_t si_rc  = gsp_rpc_send_set_system_info(rpc, bar0_va, bar0.phys,
                                                            bar1.phys, bar3.phys, dbdf);
        const int32_t reg_rc = gsp_rpc_send_set_registry(rpc, bar0_va);
        log::info("nvidia: gsp: init RPCs queued (SET_SYSTEM_INFO rc=%d, SET_REGISTRY rc=%d)",
                  si_rc, reg_rc);
    }

    RUN_ELEVATED({
        mmio::write32(bar0_va + NV_PGSP_FALCON_BASE + NV_PFALCON_FALCON_MAILBOX0,
                      static_cast<uint32_t>(static_cast<uint64_t>(rpc.libos_args.phys) & 0xFFFFFFFFu));
        mmio::write32(bar0_va + NV_PGSP_FALCON_BASE + NV_PFALCON_FALCON_MAILBOX1,
                      static_cast<uint32_t>(static_cast<uint64_t>(rpc.libos_args.phys) >> 32));
    });
    log::info("nvidia: gsp: libos boot args @ phys=0x%lx -> GSP MAILBOX0/1",
              static_cast<unsigned long>(rpc.libos_args.phys));

    // 9d: SEC2 Booter Load - authenticates GSP-RM into WPR2 and starts the GSP RISC-V.
    const int32_t bl_rc = booter_load_execute(bar0_va, boot0, wprmeta.buf.phys);
    if (bl_rc != BOOTER_OK) {
        dma_free(sig);
        gsp_free_wprmeta(wprmeta);
        gsp_free_boot_ucode(boot);
        gsp_free_radix3(radix3);
        gsp_rpc_free(rpc);
        log::warn("nvidia: ===== stage 9 INCOMPLETE (Booter Load failed rc=%d) =====", bl_rc);
        return bl_rc;
    }
    log::info("nvidia: ===== stage 9 COMPLETE (GSP-RM authenticated into WPR2; RISC-V started) =====");

    // ---- Stage 10: service the GSP CPU sequencer until GSP_INIT_DONE ----
    // Keep all sysmem boot sources (radix3 ELF, boot ucode, WprMeta, signature)
    // mapped through this phase: the early RISC-V boot DMAs the GSP-RM ELF out of
    // the radix3-mapped sysmem before the sequencer / INIT_DONE handshake.
    log::info("nvidia: --- Stage 10: CPU sequencer + INIT_DONE ---");
    gsp_seq_ctx seqctx;
    seqctx.bar0_va         = bar0_va;
    seqctx.gsp             = gsp;
    seqctx.sec2            = { bar0_va, NV_PSEC_FALCON_BASE, NV_FALCON2_SEC_BASE, NV_PSEC_FBIF_BASE };
    seqctx.chip_id         = boot0;
    seqctx.libos_args_phys = rpc.libos_args.phys;

    const int32_t id_rc = gsp_rpc_wait_for_init_done(rpc, seqctx);

    // GSP-RM is up (or we timed out): release the sysmem boot sources. The RPC
    // plane (rings/args/logs in rpc) stays alive for subsequent command traffic.
    dma_free(sig);
    gsp_free_wprmeta(wprmeta);
    gsp_free_boot_ucode(boot);
    gsp_free_radix3(radix3);

    if (id_rc == RPC_OK) {
        log::info("nvidia: ===== stages 1-10 COMPLETE: GSP-RM IS FULLY UP (INIT_DONE)! =====");

        // ---- Stage 11: post-INIT_DONE handshake + fetch static GPU info ----
        log::info("nvidia: --- Stage 11: SET_GUEST_SYSTEM_INFO + GET_GSP_STATIC_INFO ---");
        const int32_t gsi_rc = gsp_rpc_set_guest_system_info(rpc, seqctx, bar0_va);
        if (gsi_rc == RPC_OK) {
            gsp_static_info sinfo;
            const int32_t si_rc = gsp_rpc_get_static_info(rpc, seqctx, bar0_va, sinfo);
            if (si_rc == RPC_OK) {
                log::info("nvidia: ===== stage 11 COMPLETE: bidirectional RPC works; GSP reports '%s' =====",
                          sinfo.gpu_name);

                // ---- Stages 12-16: display tree + detect + modeset + scanout (first pixel) ----
                const int32_t disp_rc = gsp_display_init(rpc, seqctx, bar0_va);
                if (disp_rc != DISP_OK) {
                    log::warn("nvidia: ===== display init (stages 12-17) INCOMPLETE (rc=%d) -- see last stage above =====", disp_rc);
                }
            } else {
                log::warn("nvidia: ===== stage 11: GET_GSP_STATIC_INFO failed rc=%d =====", si_rc);
            }
        } else {
            log::warn("nvidia: ===== stage 11: SET_GUEST_SYSTEM_INFO failed rc=%d =====", gsi_rc);
        }
    } else {
        log::warn("nvidia: ===== stage 10 INCOMPLETE (rc=%d): GSP-RM did not reach INIT_DONE =====", id_rc);
    }
    return OK;
}

} // namespace nvidia
