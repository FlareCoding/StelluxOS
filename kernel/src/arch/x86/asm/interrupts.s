.intel_syntax noprefix
.code64

.extern __common_isr_entry

.macro PUSHALL
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    mov rax, ds
    push rax
    mov rax, es
    push rax
    mov rax, fs
    push rax
    mov rax, gs
    push rax
.endm

.macro POPALL
    pop rax
    mov gs, ax
    pop rax
    mov fs, ax
    pop rax
    mov es, ax
    pop rax
    mov ds, ax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
.endm

.extern __check_current_elevate_status

.global __asm_common_isr_entry
.text

# Common entry point for all exceptions and IRQs
__asm_common_isr_entry:
    #
    # Check if the process is user-elevated,
    # if so, switch onto a good kernel stack.
    #
    # push rax                                # preserve syscall return value
    # call __check_current_elevate_status     # check the elevate status
    # testb al, 0x1                           # if the result is non-zero, then task is elevated
    # pop rax                                 # restore syscall return value

    # Save CPU state
    PUSHALL             # pushes segment registers and general purpose registers
    
    mov     ax, 0x10    # kernel data segment descriptor
	mov     ds, ax
	mov     es, ax
	mov     fs, ax
	mov     gs, ax
    
    # Call C handler
    call __common_isr_entry

    # Restore state
    POPALL              # pops segment registers and general purpose registers

    add rsp, 16         # clean up the pushed error code and interrupt number
    iretq               # Interrupt return

# Exception entry points
.global __asm_exc_handler_div
.global __asm_exc_handler_db
.global __asm_exc_handler_nmi
.global __asm_exc_handler_bp
.global __asm_exc_handler_of
.global __asm_exc_handler_br
.global __asm_exc_handler_ud
.global __asm_exc_handler_nm
.global __asm_exc_handler_df
.global __asm_exc_handler_cso
.global __asm_exc_handler_ts
.global __asm_exc_handler_np
.global __asm_exc_handler_ss
.global __asm_exc_handler_gp
.global __asm_exc_handler_pf
.global __asm_exc_handler_mf
.global __asm_exc_handler_ac
.global __asm_exc_handler_mc
.global __asm_exc_handler_xm
.global __asm_exc_handler_ve
.global __asm_exc_handler_cp
.global __asm_exc_handler_hv
.global __asm_exc_handler_vc
.global __asm_exc_handler_sx

# IRQ entry points
.global __asm_irq_handler_0
.global __asm_irq_handler_1
.global __asm_irq_handler_2
.global __asm_irq_handler_3
.global __asm_irq_handler_4
.global __asm_irq_handler_5
.global __asm_irq_handler_6
.global __asm_irq_handler_7
.global __asm_irq_handler_8
.global __asm_irq_handler_9
.global __asm_irq_handler_10
.global __asm_irq_handler_11
.global __asm_irq_handler_12
.global __asm_irq_handler_13
.global __asm_irq_handler_14
.global __asm_irq_handler_15

# ----------- EXCEPTIONS ----------- #
__asm_exc_handler_div:
    push 0      # error code
    push 0      # interrupt number
    jmp __asm_common_isr_entry

__asm_exc_handler_db:
    push 0
    push 1
    jmp __asm_common_isr_entry

__asm_exc_handler_nmi:
    push 0
    push 2
    jmp __asm_common_isr_entry

__asm_exc_handler_bp:
    push 0
    push 3
    jmp __asm_common_isr_entry

__asm_exc_handler_of:
    push 0
    push 4
    jmp __asm_common_isr_entry

__asm_exc_handler_br:
    push 0
    push 5
    jmp __asm_common_isr_entry

__asm_exc_handler_ud:
    push 0
    push 6
    jmp __asm_common_isr_entry

__asm_exc_handler_nm:
    push 0
    push 7
    jmp __asm_common_isr_entry

__asm_exc_handler_df:
    push 8
    jmp __asm_common_isr_entry

__asm_exc_handler_cso:
    push 0
    push 9
    jmp __asm_common_isr_entry

__asm_exc_handler_ts:
    push 10
    jmp __asm_common_isr_entry

__asm_exc_handler_np:
    push 11
    jmp __asm_common_isr_entry

__asm_exc_handler_ss:
    push 12
    jmp __asm_common_isr_entry

__asm_exc_handler_gp:
    push 13
    jmp __asm_common_isr_entry

__asm_exc_handler_pf:
    push 14
    jmp __asm_common_isr_entry

__asm_exc_handler_mf:
    push 0
    push 16
    jmp __asm_common_isr_entry

__asm_exc_handler_ac:
    push 17
    jmp __asm_common_isr_entry

__asm_exc_handler_mc:
    push 0
    push 18
    jmp __asm_common_isr_entry

__asm_exc_handler_xm:
    push 0
    push 19
    jmp __asm_common_isr_entry

__asm_exc_handler_ve:
    push 0
    push 20
    jmp __asm_common_isr_entry

__asm_exc_handler_cp:
    push 21
    jmp __asm_common_isr_entry

__asm_exc_handler_hv:
    push 0
    push 28
    jmp __asm_common_isr_entry
    
__asm_exc_handler_vc:
    push 29
    jmp __asm_common_isr_entry

__asm_exc_handler_sx:
    push 30
    jmp __asm_common_isr_entry

# ------------- IRQS ------------- #
__asm_irq_handler_0:
    push 0
    push 32
    jmp __asm_common_isr_entry

__asm_irq_handler_1:
    push 0
    push 33
    jmp __asm_common_isr_entry

__asm_irq_handler_2:
    push 0
    push 34
    jmp __asm_common_isr_entry

__asm_irq_handler_3:
    push 0
    push 35
    jmp __asm_common_isr_entry

__asm_irq_handler_4:
    push 0
    push 36
    jmp __asm_common_isr_entry

__asm_irq_handler_5:
    push 0
    push 37
    jmp __asm_common_isr_entry

__asm_irq_handler_6:
    push 0
    push 38
    jmp __asm_common_isr_entry

__asm_irq_handler_7:
    push 0
    push 39
    jmp __asm_common_isr_entry

__asm_irq_handler_8:
    push 0
    push 40
    jmp __asm_common_isr_entry

__asm_irq_handler_9:
    push 0
    push 41
    jmp __asm_common_isr_entry

__asm_irq_handler_10:
    push 0
    push 42
    jmp __asm_common_isr_entry

__asm_irq_handler_11:
    push 0
    push 43
    jmp __asm_common_isr_entry

__asm_irq_handler_12:
    push 0
    push 44
    jmp __asm_common_isr_entry

__asm_irq_handler_13:
    push 0
    push 45
    jmp __asm_common_isr_entry

__asm_irq_handler_14:
    push 0
    push 46
    jmp __asm_common_isr_entry

__asm_irq_handler_15:
    push 0
    push 47
    jmp __asm_common_isr_entry

.section .note.GNU-stack,"",@progbits
