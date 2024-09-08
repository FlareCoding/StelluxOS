.intel_syntax noprefix
.global __ap_startup_asm

.equ __ap_startup_c_entry_address_location, 0x9000

.equ aprunning_ptr, 0x11000
.equ bspid_ptr, 0x11008
.equ bspdone_ptr, 0x11010

.equ kernel_pml4, 0x15000
.equ stack_top, 0x70000       # Top of the stack region
.equ stack_base, 0x18000      # Base of the stack region
.equ stack_size, 512          # Size of each stack (512 bytes)

.section .ktext
.code16
__ap_startup_asm:
    cli
    cld
    ljmp 0, 0x8040
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
    # get our Local APIC ID
    mov eax, 1
    cpuid
    shr ebx, 24
    mov edi, ebx
    
    #
    # Set up 512 byte stack, one for each core. It is important that all cores must have their own stack.
    # After the jump to C code, a proper 8k stack is going to be allocated for each core.
    #
    mov eax, stack_size           # eax = 0x2000 (8 KB per stack)
    mul edi                       # eax = eax * edi (stack_size * apicid)
    mov edx, eax                  # edx = eax (store result in edx)
    mov eax, stack_top            # eax = 0x70000
    sub eax, edx                  # eax = stack_top - (stack_size * apicid)
    mov esp, eax                  # esp = calculated stack top for this AP
    push edi

    # spinlock, wait for the BSP to finish
1:  pause
    cmp byte ptr [bspdone_ptr], 0
    jz 1b
    lock inc byte ptr [aprunning_ptr]
    # Load CR3 with the address of your 4-level page table
    mov eax, kernel_pml4
    mov cr3, eax
    # Enable PAE
    mov eax, cr4
    or eax, 0x20
    mov cr4, eax
    # Enable Long Mode
    mov ecx, 0xC0000080 # IA32_EFER
    rdmsr
    or eax, 0x100
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
    # Here you are in 64-bit mode
    mov rax, [__ap_startup_c_entry_address_location]
    jmp rax

.section .note.GNU-stack, "", @progbits
