OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_kentry)

SECTIONS
{
    /* Align to 4KB page */
    . = ALIGN(0x1000);

    /* Text Section */
    .text :
    {
        KEEP(*(.multiboot))
        *(.text)
        *(.text.*)
        . = ALIGN(0x1000);
    }

    /* Data Section */
    .data :
    {
        *(.data)
        *(.data.*)
        . = ALIGN(0x1000);
    }

    /* Read-Only Data Section */
    .rodata :
    {
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(0x1000);
    }

    /* BSS Section */
    .bss :
    {
        *(.bss)
        *(COMMON)
        . = ALIGN(0x1000);
    }
}