OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_stlx_crt_start)

SECTIONS
{
    . = 0x400000;

    .text : {
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(0x1000) {
        *(.data .data.*)
    }

    .bss : ALIGN(0x1000) {
        *(COMMON)
        *(.bss .bss.*)
    }

    .init_array : {
        __init_array_start = .;
        KEEP(*(.init_array))
        __init_array_end = .;
    }
}
