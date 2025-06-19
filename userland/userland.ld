OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5); /* R+X */
    data PT_LOAD FLAGS(6); /* R+W */
}

SECTIONS
{
    . = 0x400000;

    .init : {
        KEEP (*(SORT_NONE(.init)))
    } :text

    .text : {
        *(.text.unlikely .text.*_unlikely .text.unlikely.*)
        *(.text.exit .text.exit.*)
        *(.text.startup .text.startup.*)
        *(.text.hot .text.hot.*)
        *(.text .stub .text.* .gnu.linkonce.t.*)
        *(.gnu.warning)
    } :text

    .fini : {
        KEEP (*(SORT_NONE(.fini)))
    } :text

    PROVIDE (__etext = .);
    PROVIDE (_etext = .);
    PROVIDE (etext = .);

    . = ALIGN(0x1000); /* Page align for new segment */

    .rodata : {
        *(.rodata .rodata.* .gnu.linkonce.r.*)
    } :data

    .rodata1 : { *(.rodata1) } :data

    .eh_frame_hdr : { *(.eh_frame_hdr) *(.eh_frame_entry .eh_frame_entry.*) } :data
    .eh_frame : ONLY_IF_RO { KEEP (*(.eh_frame)) *(.eh_frame.*) } :data
    .gcc_except_table : ONLY_IF_RO { *(.gcc_except_table .gcc_except_table.*) } :data
    .gnu_extab : ONLY_IF_RO { *(.gnu_extab*) } :data

    /* Thread Local Storage sections */
    .tdata : {
        PROVIDE_HIDDEN (__tdata_start = .);
        *(.tdata .tdata.* .gnu.linkonce.td.*)
    } :data
    .tbss : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) } :data

    .preinit_array : ALIGN(8) {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } :data

    .init_array : ALIGN(8) {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
        PROVIDE_HIDDEN (__init_array_end = .);
    } :data

    .fini_array : ALIGN(8) {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } :data

    .ctors : ALIGN(8) {
        KEEP (*crtbegin.o(.ctors))
        KEEP (*crtbegin?.o(.ctors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
        KEEP (*(SORT(.ctors.*)))
        KEEP (*(.ctors))
    } :data

    .dtors : ALIGN(8) {
        KEEP (*crtbegin.o(.dtors))
        KEEP (*crtbegin?.o(.dtors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
        KEEP (*(SORT(.dtors.*)))
        KEEP (*(.dtors))
    } :data

    .jcr : { KEEP (*(.jcr)) } :data

    .data.rel.ro : { 
        *(.data.rel.ro.local* .gnu.linkonce.d.rel.ro.local.*) 
        *(.data.rel.ro .data.rel.ro.* .gnu.linkonce.d.rel.ro.*) 
    } :data

    .dynamic : { *(.dynamic) } :data

    .got : { *(.got) *(.igot) } :data
    .got.plt : { *(.got.plt) *(.igot.plt) } :data

    .data : ALIGN(8) {
        *(.data .data.* .gnu.linkonce.d.*)
        SORT(CONSTRUCTORS)
    } :data

    .data1 : { *(.data1) } :data

    _edata = .; 
    PROVIDE (edata = .);

    . = ALIGN(8);
    __bss_start = .;
    .bss : ALIGN(8) {
        *(.dynbss)
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(. != 0 ? 64 / 8 : 1);
    } :data

    . = ALIGN(64 / 8);
    _end = .; 
    PROVIDE (end = .);

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
} 