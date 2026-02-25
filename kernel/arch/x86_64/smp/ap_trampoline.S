.intel_syntax noprefix

.section .priv.text, "ax", @progbits

.global asm_ap_trampoline
.global asm_ap_trampoline_end

/*
 * AP trampoline — copied to physical 0x8000 at runtime.
 *
 * All internal references use hardcoded absolute addresses based on the
 * runtime location (0x8000), NOT the link-time address. These are encoded
 * as immediates, not relocations, so they survive the memcpy.
 *
 * Startup data layout at physical 0x9000:
 *   +0x00  page_table_phys  (uint64_t)
 *   +0x08  stack_top        (uint64_t, higher-half VA)
 *   +0x10  logical_id       (uint64_t)
 *   +0x18  c_entry          (uint64_t, higher-half VA)
 *
 * Temp stack for 32-bit transition: grows down from 0x9FF0.
 */

/* ------------------------------------------------------------------ */
/* 16-bit real-mode entry (AP wakes here via SIPI vector 0x8)         */
/* ------------------------------------------------------------------ */
.code16
asm_ap_trampoline:
    cli
    cld
    ljmp 0, 0x8080

    /* Embedded GDT at 0x8010 (6 entries, 48 bytes) */
    .align 16
_ap_gdt_table:                          /* 0x8010 */
    .long 0, 0                          /* 0x00: NULL */
    .long 0x0000FFFF, 0x00CF9A00        /* 0x08: 32-bit code, flat */
    .long 0x0000FFFF, 0x008F9200        /* 0x10: 32-bit data, flat */
    .long 0x00000068, 0x00CF8900        /* 0x18: TSS placeholder */
    .long 0x0000FFFF, 0x00209A00        /* 0x20: 64-bit code */
    .long 0x0000FFFF, 0x00209200        /* 0x28: 64-bit data */

    /* GDT pointer at 0x8040 */
_ap_gdt_ptr:                            /* 0x8040 */
    .word _ap_gdt_ptr - _ap_gdt_table - 1  /* limit = 47 = 0x2F */
    .long 0x8010                            /* base (runtime addr) */

    .align 64

/* ------------------------------------------------------------------ */
/* 16-bit: set up segments and enter protected mode                   */
/* ------------------------------------------------------------------ */
                                        /* 0x8080 */
_ap_real_mode:
    xor ax, ax
    mov ds, ax

    lgdt [0x8040]

    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    ljmp 8, 0x80A0

/* ------------------------------------------------------------------ */
/* 32-bit protected mode: set CR3/CR0/CR4/EFER, enable paging         */
/* ------------------------------------------------------------------ */
    .align 32
.code32                                 /* 0x80A0 */
_ap_prot_mode:
    mov ax, 0x10
    mov ds, ax
    mov ss, ax

    mov esp, 0x9FF0
    mov ebp, esp

    /* Load page table root from startup data (low 32 bits, asserted < 4GB) */
    mov eax, dword ptr [0x9000]
    mov cr3, eax

    /* CR0: clear CD+NW, set WP+NE+MP+PE+ET */
    mov eax, cr0
    and eax, ~((1 << 30) | (1 << 29))
    or  eax, (1 << 16) | (1 << 5) | (1 << 4) | (1 << 1) | (1 << 0)
    mov cr0, eax

    /* CR4: PAE + PGE + OSFXSR + OSXMMEXCPT + FSGSBASE */
    mov eax, cr4
    or  eax, (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10) | (1 << 16)
    mov cr4, eax

    /* EFER: SCE + LME + NXE */
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 0) | (1 << 8) | (1 << 11)
    wrmsr

    /* Enable paging */
    mov eax, cr0
    or  eax, (1 << 31)
    mov cr0, eax

    ljmp 0x20, 0x8100

/* ------------------------------------------------------------------ */
/* 64-bit long mode: load stack, args, call C entry                   */
/* ------------------------------------------------------------------ */
    .align 64
.code64                                 /* 0x8100 */
_ap_long_mode:
    /* Use register-indirect addressing for all startup data access
       to avoid RIP-relative relocation issues after memcpy */
    mov rbx, 0x9000

    mov rsp, [rbx + 0x08]              /* stack_top (higher-half VA) */
    mov rbp, rsp

    mov rdi, [rbx + 0x10]              /* logical_id (first C arg) */

    mov rax, [rbx + 0x18]              /* c_entry (higher-half VA) */
    call rax

    /* Should never return */
    cli
_ap_halt:
    hlt
    jmp _ap_halt

asm_ap_trampoline_end:

.section .note.GNU-stack, "", @progbits
