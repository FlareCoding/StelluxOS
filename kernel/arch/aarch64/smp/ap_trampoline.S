/*
 * AArch64 AP Trampoline
 *
 * Copied to a DMA32-allocated physical page at runtime.
 * AP starts here via PSCI CPU_ON with:
 *   x0 = logical_id (from PSCI context_id)
 *   MMU off, D-cache off, I-cache undefined, EL1, DAIF masked
 *
 * Startup data is at a fixed offset (+0x100) from the entry point,
 * filled by the BSP before calling PSCI CPU_ON.
 *
 * Layout within the trampoline page:
 *   +0x000: entry point (asm_ap_trampoline)
 *   +0x100: startup data (ap_startup_data, 64 bytes)
 *     +0x00: mair_el1    (uint64_t)
 *     +0x08: tcr_el1     (uint64_t)
 *     +0x10: ttbr1_el1   (uint64_t, kernel page table root)
 *     +0x18: ttbr0_el1   (uint64_t, identity map L0 table phys)
 *     +0x20: sctlr_el1   (uint64_t, with M+C+I bits set)
 *     +0x28: stack_top    (uint64_t, higher-half VA)
 *     +0x30: vbar_el1     (uint64_t, higher-half VA)
 *     +0x38: c_entry      (uint64_t, higher-half VA)
 */

.section .priv.text, "ax", @progbits

.global asm_ap_trampoline
.global asm_ap_trampoline_end
.type asm_ap_trampoline, @function

asm_ap_trampoline:
    /* Save logical_id (from PSCI context_id in x0) */
    mov x19, x0

    /* Compute physical address of startup data (entry + 0x100) */
    adr x20, asm_ap_trampoline
    add x20, x20, #0x100

    /* Load and write MAIR_EL1 */
    ldr x1, [x20, #0x00]
    msr mair_el1, x1

    /* Load and write TCR_EL1 */
    ldr x1, [x20, #0x08]
    msr tcr_el1, x1

    /* Load and write TTBR1_EL1 (kernel page tables) */
    ldr x1, [x20, #0x10]
    msr ttbr1_el1, x1

    /* Load and write TTBR0_EL1 (identity map for this page) */
    ldr x1, [x20, #0x18]
    msr ttbr0_el1, x1

    /* Ensure all system register writes are visible before TLB ops */
    dsb sy
    isb

    /* Invalidate all EL1 TLB entries (inner-shareable broadcast for SMP) */
    tlbi vmalle1is

    /* Wait for TLB invalidation to complete */
    dsb ish
    isb

    /* Enable MMU + D-cache + I-cache via SCTLR_EL1 */
    ldr x1, [x20, #0x20]
    msr sctlr_el1, x1
    isb

    /*
     * MMU is now on. PC is still at the trampoline's physical address,
     * which is identity-mapped via TTBR0. All higher-half addresses
     * are accessible via TTBR1.
     */

    /* Set stack pointer (higher-half VA, mapped via TTBR1) */
    ldr x1, [x20, #0x28]
    mov sp, x1

    /* Set VBAR_EL1 (exception vectors, higher-half VA) */
    ldr x1, [x20, #0x30]
    msr vbar_el1, x1
    isb

    /* Branch to C entry function (higher-half VA) */
    ldr x1, [x20, #0x38]
    mov x0, x19
    br  x1

    /* Should never reach here */
.Lap_halt:
    wfi
    b .Lap_halt

.size asm_ap_trampoline, . - asm_ap_trampoline

asm_ap_trampoline_end:
