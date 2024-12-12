.intel_syntax noprefix
.global asm_ap_startup

.equ cpus_running_ptr,          0x9000
.equ stack_index_ptr,           0x9004
.equ page_table_address_ptr,    0x9008
.equ c_entry_address_ptr,       0x9010
.equ acpi_processor_id_ptr,     0x9018

.equ stack_size, 512

/*
==============================================================
                     STACK REGION LAYOUT
==============================================================
   Stack Region Top: 0x11000
--------------------------------------------------------------
   |                      Processor Stacks                    |
   |                                                          |
   | Each AP processor's stack is calculated using the index: |
   |   [stack_index_ptr]                                      |
   |                                                          |
   | Initial stack size for each AP processor: 512 bytes      |
   |                                                          |
   | Once in long mode, each AP processor allocates its own   |
   | safe stack space in C/C++ code.                          |
--------------------------------------------------------------

                            STACK REGION
                            -------------
                            0x11000 (Top)
                               +--------+
 AP Stack (Index 0)    512 B   |        |
                            -> | Stack0 |
                               +--------+
 AP Stack (Index 1)    512 B   |        |
                            -> | Stack1 |
                               +--------+
 AP Stack (Index 2)    512 B   |        |
                            -> | Stack2 |
                               +--------+
   ... (More AP stacks based on the index) ...
                               +--------+
 AP Stack (Index N)    512 B   |        |
                            -> | StackN |
                               +--------+

--------------------------------------------------------------
   - Each AP (Application Processor) uses a stack based on 
     its unique index. The stack index is managed using the 
     [stack_index_ptr].
   - Initial stack size is temporary (512 bytes) until the 
     processor transitions to long mode and can safely 
     allocate a larger, independent stack in C/C++ code.
==============================================================
*/


.section .ktext
.code16
asm_ap_startup:
    cli
    cld
    ljmp 0, 0x8080
    .align 16
_L8010_GDT_table:
    .long 0, 0
    .long 0x0000FFFF, 0x00CF9A00    # 0x08 flat code
    .long 0x0000FFFF, 0x008F9200    # 0x10 flat data
    .long 0x00000068, 0x00CF8900    # 0x18 tss
    .long 0x0000FFFF, 0x00209A00    # 0x20 64-bit flat code
    .long 0x0000FFFF, 0x00209200    # 0x28 64-bit flat data
_L8040_GDT_value:
    .word _L8040_GDT_value - _L8010_GDT_table - 1
    .long 0x8010
    .long 0, 0
    .align 64
_L8080:
    xor ax, ax
    mov ds, ax
    lgdt [0x8040]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    ljmp 8, 0x80A0

.align 32
.code32
_L80A0:
    mov ax, 0x10
    mov ds, ax
    mov ss, ax
    
    # Get our Local APIC ID
    mov eax, 1
    cpuid
    shr ebx, 24
    mov edi, ebx

    # Calculate the stack pointer for this AP
    # stack_ptr = 0x11000 - (stack_index_ptr * stack_size)
    mov eax, dword ptr [stack_index_ptr]   # Load the current stack index into EAX (setup by smp init code)

    mov ecx, stack_size                    # Load the stack size (512 bytes)
    mul ecx                                # EAX = EAX * ECX (stack_index * stack_size)

    mov edx, 0x11000                       # Top of the stack region
    sub edx, eax                           # Calculate the stack pointer: 0x11000 - (stack_index * stack_size)

    mov esp, edx                           # Set the calculated stack pointer
    mov ebp, esp                           # Initialize the base pointer for the stack

    # Load the page table address into CR3
    mov eax, dword ptr [page_table_address_ptr]
    mov cr3, eax

    # Enable long mode by setting the relevant bits in CR4 and EFER
    mov eax, cr4
    or eax, 0x20                          # Set PAE bit
    mov cr4, eax

    # Enable PSE
    mov eax, cr4
    or eax, (1 << 4)
    mov cr4, eax

    mov ecx, 0xC0000080                   # IA32_EFER MSR
    rdmsr
    or eax, 0x100                         # Enable Long Mode (LME)
    wrmsr

    # Enable Paging
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    # Far jump to flush the pipeline and start executing 64-bit code
    ljmp 0x20, 0x8140

.align 64
.code64
_L8140:
    # Increment the number of running CPUs
    lock inc dword ptr [cpus_running_ptr]

    #
    # RDI - LAPIC ID
    # RSI - ACPI Processor Index/ID 
    #
    mov rsi, qword ptr [acpi_processor_id_ptr]

    # Here you are in 64-bit mode
    mov rax, [c_entry_address_ptr]
    call rax

.section .note.GNU-stack, "", @progbits
