.intel_syntax noprefix
.code64

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

#
# Saves the current CPU state into a cpu context structure
# C function signature:
#      void __asm_save_context(CpuContext* ctx)
# $rdi holds ctx
.global __asm_save_cpu_context

#
# Sets the current CPU state to the given context and uses iret to switch context 
# C function signature:
#      void __asm_restore_cpu_context_and_iret(CpuContext* ctx)
#
.global __asm_restore_cpu_context_and_iret

#
# rdi --> CpuContext* ctx
#
__asm_save_cpu_context:
    # General purpose registers
    mov [rdi], rax        # rax
    mov [rdi + 8], rbx    # rbx
    mov [rdi + 16], rcx   # rcx
    mov [rdi + 24], rdx   # rdx
    mov [rdi + 32], rsi   # rsi
    mov [rdi + 40], rdi   # rdi
    mov [rdi + 48], rbp   # rbp
    mov [rdi + 56], r8    # r8
    mov [rdi + 64], r9    # r9
    mov [rdi + 72], r10   # r10
    mov [rdi + 80], r11   # r11
    mov [rdi + 88], r12   # r12
    mov [rdi + 96], r13   # r13
    mov [rdi + 104], r14  # r14
    mov [rdi + 112], r15  # r15

    ret

#
# rdi --> CpuContext* ctx
#
__asm_restore_cpu_context_and_iret:
    # Save rdi from ctx
    mov rax, [rdi + 40]
    push rax

    # Save rbp from ctx
    mov rax, [rdi + 48]
    push rax

    # Restore general-purpose registers first
    mov rax, [rdi]       # Load rax from ctx
    mov rbx, [rdi + 8]   # Load rbx from ctx
    mov rcx, [rdi + 16]  # Load rcx from ctx
    mov rdx, [rdi + 24]  # Load rdx from ctx
    mov rsi, [rdi + 32]  # Load rsi from ctx

    mov r8, [rdi + 56]   # Load r8 from ctx
    mov r9, [rdi + 64]   # Load r9 from ctx
    mov r10, [rdi + 72]  # Load r10 from ctx
    mov r11, [rdi + 80]  # Load r11 from ctx
    mov r12, [rdi + 88]  # Load r12 from ctx
    mov r13, [rdi + 96]  # Load r13 from ctx
    mov r14, [rdi + 104] # Load r14 from ctx
    mov r15, [rdi + 112] # Load r15 from ctx

    # Restore Segment Registers
    mov ax, [rdi + 152] # Load ds from ctx
    mov ds, ax
    mov ax, [rdi + 160] # Load es from ctx
    mov es, ax
    mov ax, [rdi + 168] # Load fs from ctx
    mov fs, ax
    mov ax, [rdi + 176] # Load gs from ctx
    mov gs, ax

    # Pop the saved value of rbp and rdi from ctx off the stack
    pop rbp

    # Push SS, RSP, RFLAGS, CS, RIP onto the stack
    # Order matters because iretq will pop them off in reverse order
    push [rdi + 184]  # Push ss
    push [rdi + 128]  # Push rsp
    push [rdi + 136]  # Push rflags
    push [rdi + 144]   # Push cs
    push [rdi + 120]       # Push rip

    # Restore the new context's rdi
    mov rdi, [rsp + 0x28]

    # Execute iretq to switch to the new context
    iretq

.section .note.GNU-stack,"",@progbits
