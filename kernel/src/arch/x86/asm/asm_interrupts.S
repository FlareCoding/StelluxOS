.intel_syntax noprefix
#ifdef ARCH_X86_64

.code64
#include "common.S"

.extern common_isr_entry

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
    # At this point, the stack has:
    # ------------------
    #       gs
    #       fs
    #       ...
    # ------------------
    # We want to pop gs and fs values off the stack, but
    # not restore them into actual segment registers as
    # that will force-zero-out the hidden shadow gsbase
    # and fsbase registers, which will cause further bugs.
    #
    # If someone figures out how to prevent that from happening,
    # a better solution to this behavior will be much appreciated!
    #
    pop rax
    # mov gs, ax  Commented out to prevent zeroing out of gsbase
    pop rax
    # mov fs, ax  Commented out to prevent zeroing out of fsbase
    # ------------------------------------------------------------

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

.section .ktext
.global asm_common_isr_entry

# Common entry point for all exceptions and IRQs
asm_common_isr_entry:
    # Disable interrupts
    cli

    #
    # Check if the process is user-elevated,
    # and if so, we must manually switch onto
    # a good system stack.
    #
    push rax
    this_cpu_read rax, current_task
    mov rax, [rax + 0x100] # current_task->elevated
    testb al, 1
    pop rax
    jz __isr_entry_post_stack_switch

    # Clobber rax and r15
    push rax
    push r15

    # Get the top of the current system stack
    this_cpu_read rax, current_system_stack
    sub rax, 0x40

    # Copy pushed r15
    mov r15, [rsp + 0x00]
    mov [rax + 0x00], r15

    # Copy pushed rax
    mov r15, [rsp + 0x08]
    mov [rax + 0x08], r15

    # Copy interrupt number
    mov r15, [rsp + 0x10]
    mov [rax + 0x10], r15

    # Copy error code
    mov r15, [rsp + 0x18]
    mov [rax + 0x18], r15

    # Copy rip
    mov r15, [rsp + 0x20]
    mov [rax + 0x20], r15

    # Copy cs
    mov r15, [rsp + 0x28]
    mov [rax + 0x28], r15

    # Copy rflags
    mov r15, [rsp + 0x30]
    mov [rax + 0x30], r15

    # Copy rsp
    mov r15, [rsp + 0x38]
    mov [rax + 0x38], r15

    # Copy ss
    mov r15, [rsp + 0x40]
    mov [rax + 0x40], r15

    # Switch onto the system stack
    mov rsp, rax

    # Restore rax and r15 from the new stack
    pop r15
    pop rax

__isr_entry_post_stack_switch:
    # Save CPU state
    PUSHALL             # pushes segment registers and general purpose registers
    
    mov     ax, 0x10    # kernel data segment descriptor
	mov     ds, ax
	mov     es, ax
    
    # Call C handler
    call common_isr_entry

    # Restore state
    POPALL              # pops segment registers and general purpose registers

    add rsp, 16         # clean up the pushed error code and interrupt number
    sti                 # Re-enable interrupts
    iretq               # Interrupt return

# Exception entry points
.global asm_exc_handler_div
.global asm_exc_handler_db
.global asm_exc_handler_nmi
.global asm_exc_handler_bp
.global asm_exc_handler_of
.global asm_exc_handler_br
.global asm_exc_handler_ud
.global asm_exc_handler_nm
.global asm_exc_handler_df
.global asm_exc_handler_cso
.global asm_exc_handler_ts
.global asm_exc_handler_np
.global asm_exc_handler_ss
.global asm_exc_handler_gp
.global asm_exc_handler_pf
.global asm_exc_handler_mf
.global asm_exc_handler_ac
.global asm_exc_handler_mc
.global asm_exc_handler_xm
.global asm_exc_handler_ve
.global asm_exc_handler_cp
.global asm_exc_handler_hv
.global asm_exc_handler_vc
.global asm_exc_handler_sx

# IRQ entry points
.global asm_irq_handler_0
.global asm_irq_handler_1
.global asm_irq_handler_2
.global asm_irq_handler_3
.global asm_irq_handler_4
.global asm_irq_handler_5
.global asm_irq_handler_6
.global asm_irq_handler_7
.global asm_irq_handler_8
.global asm_irq_handler_9
.global asm_irq_handler_10
.global asm_irq_handler_11
.global asm_irq_handler_12
.global asm_irq_handler_13
.global asm_irq_handler_14
.global asm_irq_handler_15
.global asm_irq_handler_16
.global asm_irq_handler_17
.global asm_irq_handler_18
.global asm_irq_handler_19
.global asm_irq_handler_20
.global asm_irq_handler_21
.global asm_irq_handler_22
.global asm_irq_handler_23
.global asm_irq_handler_24
.global asm_irq_handler_25
.global asm_irq_handler_26
.global asm_irq_handler_27
.global asm_irq_handler_28
.global asm_irq_handler_29
.global asm_irq_handler_30
.global asm_irq_handler_31
.global asm_irq_handler_32
.global asm_irq_handler_33
.global asm_irq_handler_34
.global asm_irq_handler_35
.global asm_irq_handler_36
.global asm_irq_handler_37
.global asm_irq_handler_38
.global asm_irq_handler_39
.global asm_irq_handler_40
.global asm_irq_handler_41
.global asm_irq_handler_42
.global asm_irq_handler_43
.global asm_irq_handler_44
.global asm_irq_handler_45
.global asm_irq_handler_46
.global asm_irq_handler_47
.global asm_irq_handler_48
.global asm_irq_handler_49
.global asm_irq_handler_50
.global asm_irq_handler_51
.global asm_irq_handler_52
.global asm_irq_handler_53
.global asm_irq_handler_54
.global asm_irq_handler_55
.global asm_irq_handler_56
.global asm_irq_handler_57
.global asm_irq_handler_58
.global asm_irq_handler_59
.global asm_irq_handler_60
.global asm_irq_handler_61
.global asm_irq_handler_62
.global asm_irq_handler_63
.global asm_irq_handler_64

# ----------- EXCEPTIONS ----------- #
asm_exc_handler_div:
    push 0      # error code
    push 0      # interrupt number
    jmp asm_common_isr_entry

asm_exc_handler_db:
    push 0
    push 1
    jmp asm_common_isr_entry

asm_exc_handler_nmi:
    push 0
    push 2
    jmp asm_common_isr_entry

asm_exc_handler_bp:
    push 0
    push 3
    jmp asm_common_isr_entry

asm_exc_handler_of:
    push 0
    push 4
    jmp asm_common_isr_entry

asm_exc_handler_br:
    push 0
    push 5
    jmp asm_common_isr_entry

asm_exc_handler_ud:
    push 0
    push 6
    jmp asm_common_isr_entry

asm_exc_handler_nm:
    push 0
    push 7
    jmp asm_common_isr_entry

asm_exc_handler_df:
    push 0
    push 8
    jmp asm_common_isr_entry

asm_exc_handler_cso:
    push 0
    push 9
    jmp asm_common_isr_entry

asm_exc_handler_ts:
    push 10
    jmp asm_common_isr_entry

asm_exc_handler_np:
    push 11
    jmp asm_common_isr_entry

asm_exc_handler_ss:
    push 12
    jmp asm_common_isr_entry

asm_exc_handler_gp:
    push 13
    jmp asm_common_isr_entry

asm_exc_handler_pf:
    push 14
    jmp asm_common_isr_entry

asm_exc_handler_mf:
    push 0
    push 16
    jmp asm_common_isr_entry

asm_exc_handler_ac:
    push 17
    jmp asm_common_isr_entry

asm_exc_handler_mc:
    push 0
    push 18
    jmp asm_common_isr_entry

asm_exc_handler_xm:
    push 0
    push 19
    jmp asm_common_isr_entry

asm_exc_handler_ve:
    push 0
    push 20
    jmp asm_common_isr_entry

asm_exc_handler_cp:
    push 21
    jmp asm_common_isr_entry

asm_exc_handler_hv:
    push 0
    push 28
    jmp asm_common_isr_entry
    
asm_exc_handler_vc:
    push 29
    jmp asm_common_isr_entry

asm_exc_handler_sx:
    push 30
    jmp asm_common_isr_entry

# ------------- IRQS ------------- #
asm_irq_handler_0:
    push 0
    push 32
    jmp asm_common_isr_entry

asm_irq_handler_1:
    push 0
    push 33
    jmp asm_common_isr_entry

asm_irq_handler_2:
    push 0
    push 34
    jmp asm_common_isr_entry

asm_irq_handler_3:
    push 0
    push 35
    jmp asm_common_isr_entry

asm_irq_handler_4:
    push 0
    push 36
    jmp asm_common_isr_entry

asm_irq_handler_5:
    push 0
    push 37
    jmp asm_common_isr_entry

asm_irq_handler_6:
    push 0
    push 38
    jmp asm_common_isr_entry

asm_irq_handler_7:
    push 0
    push 39
    jmp asm_common_isr_entry

asm_irq_handler_8:
    push 0
    push 40
    jmp asm_common_isr_entry

asm_irq_handler_9:
    push 0
    push 41
    jmp asm_common_isr_entry

asm_irq_handler_10:
    push 0
    push 42
    jmp asm_common_isr_entry

asm_irq_handler_11:
    push 0
    push 43
    jmp asm_common_isr_entry

asm_irq_handler_12:
    push 0
    push 44
    jmp asm_common_isr_entry

asm_irq_handler_13:
    push 0
    push 45
    jmp asm_common_isr_entry

asm_irq_handler_14:
    push 0
    push 46
    jmp asm_common_isr_entry

asm_irq_handler_15:
    push 0
    push 47
    jmp asm_common_isr_entry

asm_irq_handler_16:
    push 0
    push 48
    jmp asm_common_isr_entry

asm_irq_handler_17:
    push 0
    push 49
    jmp asm_common_isr_entry

asm_irq_handler_18:
    push 0
    push 50
    jmp asm_common_isr_entry

asm_irq_handler_19:
    push 0
    push 51
    jmp asm_common_isr_entry

asm_irq_handler_20:
    push 0
    push 52
    jmp asm_common_isr_entry

asm_irq_handler_21:
    push 0
    push 53
    jmp asm_common_isr_entry

asm_irq_handler_22:
    push 0
    push 54
    jmp asm_common_isr_entry

asm_irq_handler_23:
    push 0
    push 55
    jmp asm_common_isr_entry

asm_irq_handler_24:
    push 0
    push 56
    jmp asm_common_isr_entry

asm_irq_handler_25:
    push 0
    push 57
    jmp asm_common_isr_entry

asm_irq_handler_26:
    push 0
    push 58
    jmp asm_common_isr_entry

asm_irq_handler_27:
    push 0
    push 59
    jmp asm_common_isr_entry

asm_irq_handler_28:
    push 0
    push 60
    jmp asm_common_isr_entry

asm_irq_handler_29:
    push 0
    push 61
    jmp asm_common_isr_entry

asm_irq_handler_30:
    push 0
    push 62
    jmp asm_common_isr_entry

asm_irq_handler_31:
    push 0
    push 63
    jmp asm_common_isr_entry

asm_irq_handler_32:
    push 0
    push 64
    jmp asm_common_isr_entry

asm_irq_handler_33:
    push 0
    push 65
    jmp asm_common_isr_entry

asm_irq_handler_34:
    push 0
    push 66
    jmp asm_common_isr_entry

asm_irq_handler_35:
    push 0
    push 67
    jmp asm_common_isr_entry

asm_irq_handler_36:
    push 0
    push 68
    jmp asm_common_isr_entry

asm_irq_handler_37:
    push 0
    push 69
    jmp asm_common_isr_entry

asm_irq_handler_38:
    push 0
    push 70
    jmp asm_common_isr_entry

asm_irq_handler_39:
    push 0
    push 71
    jmp asm_common_isr_entry

asm_irq_handler_40:
    push 0
    push 72
    jmp asm_common_isr_entry

asm_irq_handler_41:
    push 0
    push 73
    jmp asm_common_isr_entry

asm_irq_handler_42:
    push 0
    push 74
    jmp asm_common_isr_entry

asm_irq_handler_43:
    push 0
    push 75
    jmp asm_common_isr_entry

asm_irq_handler_44:
    push 0
    push 76
    jmp asm_common_isr_entry

asm_irq_handler_45:
    push 0
    push 77
    jmp asm_common_isr_entry

asm_irq_handler_46:
    push 0
    push 78
    jmp asm_common_isr_entry

asm_irq_handler_47:
    push 0
    push 79
    jmp asm_common_isr_entry

asm_irq_handler_48:
    push 0
    push 80
    jmp asm_common_isr_entry

asm_irq_handler_49:
    push 0
    push 81
    jmp asm_common_isr_entry

asm_irq_handler_50:
    push 0
    push 82
    jmp asm_common_isr_entry

asm_irq_handler_51:
    push 0
    push 83
    jmp asm_common_isr_entry

asm_irq_handler_52:
    push 0
    push 84
    jmp asm_common_isr_entry

asm_irq_handler_53:
    push 0
    push 85
    jmp asm_common_isr_entry

asm_irq_handler_54:
    push 0
    push 86
    jmp asm_common_isr_entry

asm_irq_handler_55:
    push 0
    push 87
    jmp asm_common_isr_entry

asm_irq_handler_56:
    push 0
    push 88
    jmp asm_common_isr_entry

asm_irq_handler_57:
    push 0
    push 89
    jmp asm_common_isr_entry

asm_irq_handler_58:
    push 0
    push 90
    jmp asm_common_isr_entry

asm_irq_handler_59:
    push 0
    push 91
    jmp asm_common_isr_entry

asm_irq_handler_60:
    push 0
    push 92
    jmp asm_common_isr_entry

asm_irq_handler_61:
    push 0
    push 93
    jmp asm_common_isr_entry

asm_irq_handler_62:
    push 0
    push 94
    jmp asm_common_isr_entry

asm_irq_handler_63:
    push 0
    push 95
    jmp asm_common_isr_entry

asm_irq_handler_64:
    push 0
    push 96
    jmp asm_common_isr_entry

.section .note.GNU-stack, "", @progbits

#endif // ARCH_X86_64
