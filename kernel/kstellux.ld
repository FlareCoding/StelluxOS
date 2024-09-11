OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_kentry)

SECTIONS
{
    . = 0xffffffff80000000;

    /* Align to 4KB page */
    . = ALIGN(0x1000);

    __ksymstart = .;

    /* Privileged Text Section */
    .ktext :
    {
        *(.ktext)
        *(.ktext.*)
        . = ALIGN(0x1000);
    }

    /* Privileged Data Section */
    .kdata :
    {
        *(.kdata)
        *(.kdata.*)
        . = ALIGN(0x1000);
    }

    /* Privileged Read-Only Data Section */
    .krodata :
    {
        *(.krodata)
        *(.krodata.*)
        . = ALIGN(0x1000);
    }

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

    /* Unit Test Section */
    .unit_test :
    {
        __unit_tests_start = .;
        KEEP(*(.unit_test))
        __unit_tests_end = .;
        . = ALIGN(0x1000);
    }

    /* Disabled/Unused Unit Test Section */
    .unit_test_unused :
    {
        . = ALIGN(0x1000);
    }

    __ksymend = .;
}