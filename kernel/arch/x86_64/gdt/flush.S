/*
 * flush.S - GDT load and segment reload for x86_64
 *
 * Intel syntax assembly.
 * Loads a new GDT, reloads all segment registers, and loads the TSS.
 */

.intel_syntax noprefix

.section .priv.text, "ax", @progbits

/*
 * stlx_x86_64_gdt_flush(const gdt_ptr* gdtr, uint16_t cs, uint16_t ds, uint16_t tss)
 *
 * Parameters (System V AMD64 ABI):
 *   rdi = pointer to gdt_ptr structure
 *   rsi = kernel code segment selector (CS)
 *   rdx = kernel data segment selector (DS)
 *   rcx = TSS selector
 *
 * Order of operations:
 *   1. Loads the GDT via lgdt
 *   2. Reloads CS via a far return
 *   3. Reloads DS, ES, SS with the data segment selector
 *   4. Zeros FS selector
 *   5. Zeros GS selector while preserving IA32_GS_BASE (per-CPU base)
 *   6. Loads the TSS via ltr
 */
.global stlx_x86_64_gdt_flush
.type stlx_x86_64_gdt_flush, @function
stlx_x86_64_gdt_flush:
    /* Load the new GDT */
    lgdt [rdi]

    /* Save parameters we'll need after segment reload */
    mov rax, rsi        /* CS selector */
    mov r8, rdx         /* DS selector */
    mov r9, rcx         /* TSS selector */

    /*
     * Reload CS via far return.
     * We push the new CS and the return address, then use retfq.
     */
    lea r10, [rip + .Lreload_cs]
    push rax            /* Push new CS */
    push r10            /* Push return address */
    retfq               /* Far return: pops RIP and CS */

.Lreload_cs:
    /*
     * Now running with new CS. Reload data segment registers.
     */
    mov ax, r8w
    mov ds, ax
    mov es, ax
    mov ss, ax

    /*
     * Zero FS selector.
     */
    xor eax, eax
    mov fs, ax

    /*
     * Save IA32_GS_BASE, zero GS selector, then restore IA32_GS_BASE.
     * This preserves the per-CPU base set by percpu::init_bsp().
     */
    mov ecx, 0xC0000101     /* IA32_GS_BASE */
    rdmsr                   /* EDX:EAX = current GS base */
    mov r10d, eax           /* Save low 32 bits */
    mov r11d, edx           /* Save high 32 bits */

    xor eax, eax
    mov gs, ax              /* Zero GS selector (clears hidden base) */

    mov eax, r10d           /* Restore low 32 bits */
    mov edx, r11d           /* Restore high 32 bits */
    mov ecx, 0xC0000101     /* IA32_GS_BASE */
    wrmsr                   /* Restore GS base */

    /*
     * Load the Task State Segment.
     */
    ltr r9w

    ret

.size stlx_x86_64_gdt_flush, . - stlx_x86_64_gdt_flush
