OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

/* Higher-half kernel base address */
KERNEL_VADDR = 0xffffffff80000000;

PHDRS {
    text PT_LOAD FLAGS(5);    /* r-x: code */
    rodata PT_LOAD FLAGS(4);  /* r--: read-only data */
    data PT_LOAD FLAGS(6);    /* rw-: read-write data */
}

SECTIONS {
    . = KERNEL_VADDR;
    __stlx_kern_start = .;

    /* Limine requests (must be in a specific section for Limine to find them) */
    .limine_requests : AT(ADDR(.limine_requests) - KERNEL_VADDR) {
        KEEP(*(.limine_requests))
    } :text

    /* Code */
    .text : AT(ADDR(.text) - KERNEL_VADDR) {
        *(.text .text.*)
        /* Privileged code in same segment */
        . = ALIGN(4096);
        __stlx_kern_priv_start = .;
        *(.priv.text .priv.text.*)
    } :text

    /* Read-only data */
    . = ALIGN(4096);
    .rodata : AT(ADDR(.rodata) - KERNEL_VADDR) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        /* Privileged rodata */
        . = ALIGN(4096);
        __priv_rodata_start = .;
        *(.priv.rodata .priv.rodata.*)
        __rodata_end = .;
    } :rodata

    /* Read-write data - ALL RW data in one segment
     * This includes: .data, .bss.percpu, .bss, .priv.data, .priv.bss */
    . = ALIGN(4096);
    .data : AT(ADDR(.data) - KERNEL_VADDR) {
        *(.data .data.*)
    } :data

    /* Per-CPU BSS template */
    . = ALIGN(4096);
    .bss.percpu : AT(ADDR(.bss.percpu) - KERNEL_VADDR) {
        __percpu_start = .;

        KEEP(*(.bss.percpu..base .bss.percpu..base.*))

        . = ALIGN(64);
        *(.bss.percpu..cacheline_aligned .bss.percpu..cacheline_aligned.*)

        *(.bss.percpu .bss.percpu.*)

        . = ALIGN(4096);
        __percpu_end = .;
    } :data

    /* Fail build if per-CPU area exceeds static CPU0 allocation */
    ASSERT(__percpu_end - __percpu_start <= 0x2000,
           "Per-CPU section exceeds 8KB - increase CPU0_AREA_SIZE")

    /* BSS */
    .bss : AT(ADDR(.bss) - KERNEL_VADDR) {
        *(COMMON)
        *(.bss .bss.*)
    } :data
    __bss_end = .;

    /* Boundary between unprivileged and privileged data */
    . = ALIGN(4096);
    __priv_data_start = .;

    /* Privileged read-write data */
    .priv.data : AT(ADDR(.priv.data) - KERNEL_VADDR) {
        *(.priv.data .priv.data.*)
    } :data

    /* Privileged BSS */
    .priv.bss : AT(ADDR(.priv.bss) - KERNEL_VADDR) {
        *(.priv.bss .priv.bss.*)
    } :data

    /* Unit test descriptors (empty in production builds, zero overhead) */
    . = ALIGN(4096);
    .stlx_unit_test : AT(ADDR(.stlx_unit_test) - KERNEL_VADDR) SUBALIGN(8) {
        __stlx_unit_test_start = .;
        KEEP(*(.stlx_unit_test.0.*))
        KEEP(*(.stlx_unit_test.1.*))
        KEEP(*(.stlx_unit_test.2.*))
        KEEP(*(.stlx_unit_test.3.*))
        __stlx_unit_test_end = .;
    } :data

    .stlx_unit_test_hooks : AT(ADDR(.stlx_unit_test_hooks) - KERNEL_VADDR) SUBALIGN(8) {
        __stlx_unit_test_hooks_start = .;
        KEEP(*(.stlx_unit_test_hooks))
        __stlx_unit_test_hooks_end = .;
    } :data

    /* Ensure end symbols are page-aligned to avoid partial page issues */
    . = ALIGN(4096);
    __stlx_kern_priv_end = .;
    __stlx_kern_end = .;

    /* Discard unwanted sections */
    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
}
