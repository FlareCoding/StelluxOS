.intel_syntax noprefix
#ifdef ARCH_X86_64

#define ASM_FILE
#include <boot/multiboot2.h>

.section .bss.bootstrap

.align 0x1000
tmp_stack:
    .fill 0x1000, 1, 0   /* Allocate 4KB stack for the bootstrap code */

.align 0x1000
pml4_table:
    .fill 512, 8, 0   /* PML4 table */

.align 0x1000
pdpt_table_low:
    .fill 512, 8, 0   /* PDPT for low memory */

.align 0x1000
pdpt_table_high:
    .fill 512, 8, 0   /* PDPT for higher-half memory */

.align 0x1000
pd_table_low:
    .fill 512, 8, 0   /* PD for low memory */

.align 0x1000
pd_table_high:
    .fill 512, 8, 0   /* PD for higher-half memory */

.align 0x1000
page_tables_low:
    .fill 512 * 2, 8, 0 /* Page tables for low memory (2 entries for 4MB) */

.align 0x1000
page_tables_high:
    .fill 512 * 2, 8, 0 /* Page tables for higher-half memory (2 entries for 4MB) */

.section .data.bootstrap
.align 8

gdt32:  
    /* Null Descriptor */
    .quad 0x0000000000000000

    /* Kernel 32-bit Code Segment */
    .quad 0x00cf9a000000ffff

    /* Kernel 32-bit Data Segment */
    .quad 0x00cf92000000ffff

gdtr32:
    .word gdt32_end - gdt32 - 1  /* Size of GDT */
    .quad gdt32               /* Address of GDT */

gdt32_end:

.align 8
gdt64:
    .long 0, 0
    .long 0x0000FFFF, 0x00CF9A00    # 0x08 flat code
    .long 0x0000FFFF, 0x008F9200    # 0x10 flat data
    .long 0x00000068, 0x00CF8900    # 0x18 tss
    .long 0x0000FFFF, 0x00209A00    # 0x20 64-bit flat code
    .long 0x0000FFFF, 0x00209200    # 0x28 64-bit flat data

gdtr64:
    /* 
       - Limit: Size of GDT in bytes minus 1
       - Base: Address of the GDT
    */
    .word gdt64_end - gdt64 - 1  /* Limit (in bytes) */
    .quad gdt64                   /* Base address of GDT */

gdt64_end:

.code32
.section .text.bootstrap
.globl _start
_start:
        jmp real_start

/* Multiboot header -- Safe to place this header in 1st page for GRUB */
.align MULTIBOOT_HEADER_ALIGN

multiboot_header:
    .long MULTIBOOT2_HEADER_MAGIC /* Multiboot magic number */
    .long 0 /* Multiboot Arch i386 */
    .long multiboot_header_end - multiboot_header
    .long -(MULTIBOOT2_HEADER_MAGIC + multiboot_header_end - multiboot_header) /* Checksum */

    /* Information Request Tag: modules (type=3) */
    .align 8
    .short MULTIBOOT_HEADER_TAG_INFORMATION_REQUEST
    .short 0                            /* flags = 0 */
    .long 16                            /* size = 16 bytes total */
    .long MULTIBOOT_TAG_TYPE_MODULE     /* we want modules */
    .long 0                             /* end of the request array */

    /* Module Align Tag */
    /* If you want GRUB to align modules on page boundaries */
    .align 8
    .short MULTIBOOT_HEADER_TAG_MODULE_ALIGN
    .short 0
    .long 8

    /* GOP Framebuffer Tag */
    .short 5        /* Tag type = 5 (Framebuffer) */
    .short 0        /* Flags */
    .long 20        /* Size of the tag in bytes */
    .long 0         /* Preferrer width in pixels (0 = no preference) */
    .long 0         /* Preferrer height in pixels (0 = no preference) */
    .long 32        /* Preferrer depth in bits per pixel (0 = no preference) */

    /* Padding to align to 8-byte boundary */
    .space 4

    /* End Tag */
    .short MULTIBOOT_HEADER_TAG_END
    .short 0
    .long 8
multiboot_header_end:

real_start:
    /* 1) Set up a temporary stack */
    mov esp, offset tmp_stack + 0x1000

    /* save the multiboot structure and magic number on the stack */ 
    push ebx
    push eax

    /* 2) Initialize the EFLAGS register */
    push 2      /* Bit 1 always set to 1 in EFLAGS -- all other bits set to 0 */
    popf        /* See 2-8 Manual vol 3 */

    /* 3) Load the Global Descriptor Table */
    lgdt gdtr32

    /* Flush CS by performing a far jump */
    ljmp 0x08:flush_cs32

flush_cs32:
    /* Reload segment registers to use new descriptors */
    mov ax, 0x10       /* 32-bit data segment selector */
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

setup_paging:
    /* Step 1: Link PML4 to PDPTs */
    mov eax, offset pdpt_table_low        /* Physical address of PDPT for low memory */
    or eax, 0x3                          /* Present and Writable flags */
    mov [pml4_table], eax                /* PML4[0] -> PDPT for low memory */

    mov eax, offset pdpt_table_high      /* Physical address of PDPT for higher-half memory */
    or eax, 0x3                          /* Present and Writable flags */
    mov [pml4_table + 511 * 8], eax      /* PML4[511] -> PDPT for higher-half memory */

    /* Step 2: Link PDPT to PD */
    mov eax, offset pd_table_low         /* Physical address of PD for low memory */
    or eax, 0x3                          /* Present and Writable flags */
    mov [pdpt_table_low], eax            /* PDPT[0] -> PD for low memory */

    mov eax, offset pd_table_high        /* Physical address of PD for higher-half memory */
    or eax, 0x3                          /* Present and Writable flags */
    mov [pdpt_table_high + 510 * 8], eax /* PDPT[510] -> PD for higher-half memory */

    /* Step 3: Map physical memory into both PDs */
    lea edi, [pd_table_low]              /* PD starts at edi (low memory) */
    mov eax, offset page_tables_low      /* First Page Table physical address for low memory */
    mov ecx, 2                           /* Map 2 entries (4MB) */

map_pd_low:
    or eax, 0x3                          /* Present and Writable flags */
    mov [edi], eax                       /* Set PD entry to point to the page table */
    add edi, 8                           /* Move to next PD entry */
    add eax, 0x1000                      /* Next page table physical address */
    loop map_pd_low

    lea edi, [pd_table_high]             /* PD[0] starts at edi */
    mov eax, offset page_tables_high     /* First Page Table physical address for higher-half */
    mov ecx, 2                           /* Map 2 entries (4MB) */

map_pd_high:
    or eax, 0x3                          /* Present and Writable flags */
    mov [edi], eax                       /* Set PD entry to point to the page table */
    add edi, 8                           /* Move to next PD entry */
    add eax, 0x1000                      /* Next page table physical address */
    loop map_pd_high

    /* Step 4: Fill Page Tables for low memory */
    mov eax, 0x0                         /* Start of physical memory */
    lea edi, [page_tables_low]           /* Start of Page Tables for low memory */
    mov ecx, 512 * 2                     /* Map 4MB in 4KB pages */

map_pages_low:
    or eax, 0x3                          /* Present and Writable flags */
    mov [edi], eax                       /* Set PTE */
    add edi, 8                           /* Move to next PTE */
    add eax, 0x1000                      /* Next 4KB physical page */
    loop map_pages_low

    /* Step 5: Fill Page Tables for higher-half memory */
    mov eax, 0x0                         /* Start of physical memory */
    lea edi, [page_tables_high]          /* Start of Page Tables for higher-half memory */
    mov ecx, 512 * 2                     /* Map 4MB in 4KB pages */

map_pages_high:
    or eax, 0x3                          /* Present and Writable flags */
    mov [edi], eax                       /* Set PTE */
    add edi, 8                           /* Move to next PTE */
    add eax, 0x1000                      /* Next 4KB physical page */
    loop map_pages_high

enable_long_mode:
    # Move the multiboot parameters into
    # registers before the far jump and flush.
    pop edi
    pop esi

    mov eax, offset pml4_table
    mov cr3, eax

    # Enable PAE
    mov eax, cr4
    or eax, 0x20
    mov cr4, eax

    # Enable PSE
    mov eax, cr4
    or eax, (1 << 4)
    mov cr4, eax
    
    # Enable Long Mode and NXE
    mov ecx, 0xC0000080  # IA32_EFER
    rdmsr
    or eax, 0x100        # Set Long Mode Enable (bit 8)
    or eax, 0x800        # Set No-Execute Enable (bit 11)
    wrmsr
    
    # Enable Paging
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    lgdt gdtr64

    ljmp 0x20:long_mode_start

.align 64
.code64
.section .text.bootstrap

.extern init

long_mode_start:
    /* Reload segment registers to use new descriptors */
    mov ax, 0x28       /* 64-bit data segment selector */
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    # Setup long mode stack
    add rsp, offset KERNEL_OFFSET

    # Adjust the MBI pointer to be in the higher half
    add rsi, offset KERNEL_OFFSET
    
    mov rax, offset init
    call rax

loop:
    hlt
    jmp loop

.section .note.GNU-stack, "", @progbits

#endif // ARCH_X86_64

