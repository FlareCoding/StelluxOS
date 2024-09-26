.intel_syntax noprefix
.code64

.include "common.s"

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

.extern __check_current_elevate_status

.section .ktext

.global __asm_common_isr_entry

# Common entry point for all exceptions and IRQs
__asm_common_isr_entry:
    # Disable interrupts
    cli

    #
    # We need to copy the existing hardware-pushed exception
    # frame onto the kernel stack and then switch to it.
    #
    swapgs

    #
    # Check if the process is user-elevated,
    # if so, switch onto a good kernel stack.
    #
    push rax
    call __check_current_elevate_status
    testb al, 1
    pop rax

    # If we are lowered, we need to restore the gsbase
    # before jumping to the post_stack_switch label
    swapgs

    jz __isr_entry_post_stack_switch

    # If the manual stack switch is indeed needed,
    # switch the gsbase again to get access to the TLS.
    swapgs

    # Clobber rax and r15
    push rax
    push r15

    # Move the top of the kernel stack into rax
    mov rax, gs:[per_cpu_offset_current_kernel_stack]

    # Copy interrupt number (+0x10 offset is due to rax being pushed)
    mov r15, [rsp + 0x10]
    mov [rax], r15

    # Copy error code
    mov r15, [rsp + 0x18]
    mov [rax + 0x08], r15

    # Copy rip
    mov r15, [rsp + 0x20]
    mov [rax + 0x10], r15

    # Copy cs
    mov r15, [rsp + 0x28]
    mov [rax + 0x18], r15

    # Copy rflags
    mov r15, [rsp + 0x30]
    mov [rax + 0x20], r15

    # Copy rsp
    mov r15, [rsp + 0x38]
    mov [rax + 0x28], r15

    # Copy ss
    mov r15, [rsp + 0x40]
    mov [rax + 0x30], r15

    # Restore rax and r15
    pop r15
    pop rax

    # Switch onto the kernel stack
    mov rsp, gs:[per_cpu_offset_current_kernel_stack]

    # Restore original gs
    swapgs

__isr_entry_post_stack_switch:
    # Save CPU state
    PUSHALL             # pushes segment registers and general purpose registers
    
    mov     ax, 0x10    # kernel data segment descriptor
	mov     ds, ax
	mov     es, ax
	# mov     fs, ax
	# mov     gs, ax
    
    # Call C handler
    call __common_isr_entry

    # Restore state
    POPALL              # pops segment registers and general purpose registers

    add rsp, 16         # clean up the pushed error code and interrupt number
    sti                 # Re-enable interrupts
    iretq               # Interrupt return

#
# Routine to be able to switch process context's
# in-place outside of a normal interrupt context.
#
# void __asm_ctx_switch_no_irq(PtRegs* regs);
#
.global __asm_ctx_switch_no_irq

__asm_ctx_switch_no_irq:
    #
    # PtRegs is now stored in rdi
    # Push all the fields onto the stack
    #
    # ---- hwframe ----
    mov rax, [rdi + 0xC8] # regs->ss
    push rax

    mov rax, [rdi + 0xC0] # regs->rsp
    push rax

    mov rax, [rdi + 0xB8] # regs->rflags
    push rax

    mov rax, [rdi + 0xB0] # regs->cs
    push rax

    mov rax, [rdi + 0xA8] # regs->rip
    push rax

    #
    # ---- rest of the registers ----
    #
    mov rax, [rdi + 0xA0] # regs->error
    push rax

    mov rax, [rdi + 0x98] # regs->intno
    push rax

    mov rax, [rdi + 0x90] # regs->rax
    push rax

    mov rax, [rdi + 0x88] # regs->rcx
    push rax

    mov rax, [rdi + 0x80] # regs->rdx
    push rax

    mov rax, [rdi + 0x78] # regs->rbx
    push rax

    mov rax, [rdi + 0x70] # regs->rbp
    push rax

    mov rax, [rdi + 0x68] # regs->rsi
    push rax

    mov rax, [rdi + 0x60] # regs->rdi
    push rax

    mov rax, [rdi + 0x58] # regs->r8
    push rax

    mov rax, [rdi + 0x50] # regs->r9
    push rax

    mov rax, [rdi + 0x48] # regs->r10
    push rax

    mov rax, [rdi + 0x40] # regs->r11
    push rax

    mov rax, [rdi + 0x38] # regs->r12
    push rax

    mov rax, [rdi + 0x30] # regs->r13
    push rax

    mov rax, [rdi + 0x28] # regs->r14
    push rax

    mov rax, [rdi + 0x20] # regs->r15
    push rax

    mov rax, [rdi + 0x18] # regs->ds
    push rax

    mov rax, [rdi + 0x10] # regs->es
    push rax

    mov rax, [rdi + 0x08] # regs->fs
    push rax

    mov rax, [rdi + 0x00] # regs->gs
    push rax

    #
    # Now that the PtRegs are properly constructed on the,
    # stack we can pop them and iret into the new context.
    #
    POPALL              # Pop segment registers and general purpose registers

    add rsp, 16         # Clean up the pushed error code and interrupt number
    sti                 # Re-enable interrupts
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
.global __asm_irq_handler_16
.global __asm_irq_handler_17
.global __asm_irq_handler_18
.global __asm_irq_handler_19
.global __asm_irq_handler_20
.global __asm_irq_handler_21
.global __asm_irq_handler_22
.global __asm_irq_handler_23
.global __asm_irq_handler_24
.global __asm_irq_handler_25
.global __asm_irq_handler_26
.global __asm_irq_handler_27
.global __asm_irq_handler_28
.global __asm_irq_handler_29
.global __asm_irq_handler_30
.global __asm_irq_handler_31
.global __asm_irq_handler_32
.global __asm_irq_handler_33
.global __asm_irq_handler_34
.global __asm_irq_handler_35
.global __asm_irq_handler_36
.global __asm_irq_handler_37
.global __asm_irq_handler_38
.global __asm_irq_handler_39
.global __asm_irq_handler_40
.global __asm_irq_handler_41
.global __asm_irq_handler_42
.global __asm_irq_handler_43
.global __asm_irq_handler_44
.global __asm_irq_handler_45
.global __asm_irq_handler_46
.global __asm_irq_handler_47
.global __asm_irq_handler_48
.global __asm_irq_handler_49
.global __asm_irq_handler_50
.global __asm_irq_handler_51
.global __asm_irq_handler_52
.global __asm_irq_handler_53
.global __asm_irq_handler_54
.global __asm_irq_handler_55
.global __asm_irq_handler_56
.global __asm_irq_handler_57
.global __asm_irq_handler_58
.global __asm_irq_handler_59
.global __asm_irq_handler_60
.global __asm_irq_handler_61
.global __asm_irq_handler_62
.global __asm_irq_handler_63
.global __asm_irq_handler_64

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

__asm_irq_handler_16:
    push 0
    push 48
    jmp __asm_common_isr_entry

__asm_irq_handler_17:
    push 0
    push 49
    jmp __asm_common_isr_entry

__asm_irq_handler_18:
    push 0
    push 50
    jmp __asm_common_isr_entry

__asm_irq_handler_19:
    push 0
    push 51
    jmp __asm_common_isr_entry

__asm_irq_handler_20:
    push 0
    push 52
    jmp __asm_common_isr_entry

__asm_irq_handler_21:
    push 0
    push 53
    jmp __asm_common_isr_entry

__asm_irq_handler_22:
    push 0
    push 54
    jmp __asm_common_isr_entry

__asm_irq_handler_23:
    push 0
    push 55
    jmp __asm_common_isr_entry

__asm_irq_handler_24:
    push 0
    push 56
    jmp __asm_common_isr_entry

__asm_irq_handler_25:
    push 0
    push 57
    jmp __asm_common_isr_entry

__asm_irq_handler_26:
    push 0
    push 58
    jmp __asm_common_isr_entry

__asm_irq_handler_27:
    push 0
    push 59
    jmp __asm_common_isr_entry

__asm_irq_handler_28:
    push 0
    push 60
    jmp __asm_common_isr_entry

__asm_irq_handler_29:
    push 0
    push 61
    jmp __asm_common_isr_entry

__asm_irq_handler_30:
    push 0
    push 62
    jmp __asm_common_isr_entry

__asm_irq_handler_31:
    push 0
    push 63
    jmp __asm_common_isr_entry

__asm_irq_handler_32:
    push 0
    push 64
    jmp __asm_common_isr_entry

__asm_irq_handler_33:
    push 0
    push 65
    jmp __asm_common_isr_entry

__asm_irq_handler_34:
    push 0
    push 66
    jmp __asm_common_isr_entry

__asm_irq_handler_35:
    push 0
    push 67
    jmp __asm_common_isr_entry

__asm_irq_handler_36:
    push 0
    push 68
    jmp __asm_common_isr_entry

__asm_irq_handler_37:
    push 0
    push 69
    jmp __asm_common_isr_entry

__asm_irq_handler_38:
    push 0
    push 70
    jmp __asm_common_isr_entry

__asm_irq_handler_39:
    push 0
    push 71
    jmp __asm_common_isr_entry

__asm_irq_handler_40:
    push 0
    push 72
    jmp __asm_common_isr_entry

__asm_irq_handler_41:
    push 0
    push 73
    jmp __asm_common_isr_entry

__asm_irq_handler_42:
    push 0
    push 74
    jmp __asm_common_isr_entry

__asm_irq_handler_43:
    push 0
    push 75
    jmp __asm_common_isr_entry

__asm_irq_handler_44:
    push 0
    push 76
    jmp __asm_common_isr_entry

__asm_irq_handler_45:
    push 0
    push 77
    jmp __asm_common_isr_entry

__asm_irq_handler_46:
    push 0
    push 78
    jmp __asm_common_isr_entry

__asm_irq_handler_47:
    push 0
    push 79
    jmp __asm_common_isr_entry

__asm_irq_handler_48:
    push 0
    push 80
    jmp __asm_common_isr_entry

__asm_irq_handler_49:
    push 0
    push 81
    jmp __asm_common_isr_entry

__asm_irq_handler_50:
    push 0
    push 82
    jmp __asm_common_isr_entry

__asm_irq_handler_51:
    push 0
    push 83
    jmp __asm_common_isr_entry

__asm_irq_handler_52:
    push 0
    push 84
    jmp __asm_common_isr_entry

__asm_irq_handler_53:
    push 0
    push 85
    jmp __asm_common_isr_entry

__asm_irq_handler_54:
    push 0
    push 86
    jmp __asm_common_isr_entry

__asm_irq_handler_55:
    push 0
    push 87
    jmp __asm_common_isr_entry

__asm_irq_handler_56:
    push 0
    push 88
    jmp __asm_common_isr_entry

__asm_irq_handler_57:
    push 0
    push 89
    jmp __asm_common_isr_entry

__asm_irq_handler_58:
    push 0
    push 90
    jmp __asm_common_isr_entry

__asm_irq_handler_59:
    push 0
    push 91
    jmp __asm_common_isr_entry

__asm_irq_handler_60:
    push 0
    push 92
    jmp __asm_common_isr_entry

__asm_irq_handler_61:
    push 0
    push 93
    jmp __asm_common_isr_entry

__asm_irq_handler_62:
    push 0
    push 94
    jmp __asm_common_isr_entry

__asm_irq_handler_63:
    push 0
    push 95
    jmp __asm_common_isr_entry

__asm_irq_handler_64:
    push 0
    push 96
    jmp __asm_common_isr_entry

.section .note.GNU-stack,"",@progbits
