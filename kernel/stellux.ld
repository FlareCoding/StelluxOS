OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_OFFSET = 0xffffffff80000000;
KERNEL_START = 0x100000;

SECTIONS
{
    . = KERNEL_OFFSET + KERNEL_START;

    /* Bootstrap section at 1MB */
    .bootstrap 1M : AT(1M) {
        *(.text.bootstrap)
        *(.data.bootstrap)
        *(.bss.bootstrap)
    }

    /* Higher-half kernel section at 0xffffffff80000000 */
    . += KERNEL_OFFSET;

    .text : AT(ADDR(.text) - KERNEL_OFFSET) {
        *(.text)
    }

    .data : AT(ADDR(.data) - KERNEL_OFFSET) {
        *(.data)
    }

    .bss : AT(ADDR(.bss) - KERNEL_OFFSET) {
        *(COMMON)
        *(.bss)
    }
}
