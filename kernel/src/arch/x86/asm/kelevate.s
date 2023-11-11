.intel_syntax noprefix
.code64
.include "common.s"

.global __call_lowered_entry_asm
.global __klower_asm

.text

#
# rdi - entry
# rsi - stack
# rdx - rflags
#
__call_lowered_entry_asm:
    # Save current stack values
    mov gs:[per_cpu_offset_current_kernel_stack], rsp
    mov gs:[per_cpu_offset_current_user_stack], rsi

    mov ax, __USER_DS | 0x3
    mov ds, ax
    mov es, ax

    # Construct an interrupt frame on stack
    push    __USER_DS       # regs.hwframe->ss
    push    rsi             # regs.hwframe->rsp
    push    rdx	            # regs.hwframe->rflags
    push    __USER_CS	    # regs.hwframe->cs
    push    rdi	            # regs.hwframe->rip

    swapgs
    iretq

.section .note.GNU-stack, "", @progbits
