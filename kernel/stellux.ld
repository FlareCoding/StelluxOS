OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_OFFSET = 0xffffffff80000000;
KERNEL_START = 0x100000;

SECTIONS
{
    . = KERNEL_OFFSET + KERNEL_START;
    
    __ksymstart = .;

    /* Bootstrap section at 1MB */
    .bootstrap 1M : AT(1M)
    {
        *(.text.bootstrap)
        *(.data.bootstrap)
        *(.bss.bootstrap)
        . = ALIGN(0x1000);
    }

    /* Higher-half kernel section at 0xffffffff80000000 */
    . += KERNEL_OFFSET;

    .text : AT(ADDR(.text) - KERNEL_OFFSET)
    {
        *(.text)
        . = ALIGN(0x1000);
    }

    .data : AT(ADDR(.data) - KERNEL_OFFSET)
    {
        *(.data)
        *(.data.*)
        . = ALIGN(0x1000);
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_OFFSET)
    {
        *(.rodata)
        *(.rodata.*)
        . = ALIGN(0x1000);
    }

    .bss : AT(ADDR(.bss) - KERNEL_OFFSET)
    {
        *(COMMON)
        *(.bss)
        . = ALIGN(0x1000);
    }

    __privileged_kernel_region_start = .;

    /* Privileged Text Section */
    .ktext : AT(ADDR(.ktext) - KERNEL_OFFSET)
    {
        *(.ktext)
        *(.ktext.*)
        . = ALIGN(0x1000);
    }

    /* Privileged Data Section */
    .kdata : AT(ADDR(.kdata) - KERNEL_OFFSET)
    {
        *(.kdata)
        *(.kdata.*)
        . = ALIGN(0x1000);
    }

    /* Privileged Read-Only Data Section */
    .krodata : AT(ADDR(.krodata) - KERNEL_OFFSET)
    {
        *(.krodata)
        *(.krodata.*)
        . = ALIGN(0x1000);
    }

    __privileged_kernel_region_end = .;

    __per_cpu_start = .;
    .percpu : AT(ADDR(.percpu) - KERNEL_OFFSET) {
        *(.percpu)
    }
    __per_cpu_end = .;
    __per_cpu_size = __per_cpu_end - __per_cpu_start;

    . = ALIGN(0x1000);

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
